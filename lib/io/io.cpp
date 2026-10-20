#include "io.hpp"

static constexpr std::array<std::string_view, 4> io_methods{"Read", "Write", "Flush", "Identify"};
auto io_rx::valid(io_ticket ticket) const -> bool {
    return ticket.slot < entries_.size() && ticket.id && entries_[ticket.slot].command.id == ticket.id;
}
auto io_rx::accept(io_cmd command, ubus_request& call) -> status_t {
    if (stopping_) return STATUS_CLOSED;
    if (command.id <= last_) return STATUS_STALE;
    for (auto& entry : entries_) if (!entry.command.id) {
        entry.command = command; entry.call = std::move(call);
        last_ = command.id; ++active_; return STATUS_PENDING;
    }
    return STATUS_BUSY;
}
auto io_rx::admit(io_ticket& ticket) -> io_admission {
    if (stopping_) return io_admission::Closed;
    entry* first{};
    for (auto& entry : entries_) if (entry.command.id && !entry.admitted
        && (!first || entry.command.id < first->command.id)) first = &entry;
    if (!first) return io_admission::Empty;
    first->admitted = true;
    ticket = {static_cast<uint32_t>(first - entries_.data()), first->command.id};
    return io_admission::Ready;
}
auto io_rx::request(io_ticket ticket) const -> const io_cmd* {
    return valid(ticket) ? &entries_[ticket.slot].command : nullptr;
}
auto io_rx::cancel(uint64_t id) -> status_t {
    for (auto& entry : entries_) if (entry.command.id == id && id) {
        if (entry.committed) return STATUS_BUSY;
        entry.cancelled = true; return STATUS_OK;
    }
    return STATUS_NOT_FOUND;
}
auto io_rx::commit(io_ticket ticket) -> bool {
    if (!valid(ticket) || entries_[ticket.slot].cancelled) return false;
    entries_[ticket.slot].committed = true; return true;
}
auto io_rx::cancelled(io_ticket ticket) const -> bool { return valid(ticket) && entries_[ticket.slot].cancelled; }
auto io_rx::finish(io_ticket ticket, status_t status, uint64_t bytes) -> bool {
    if (!valid(ticket)) return false;
    ubus_msg result; result.status = status; result.data.put("bytes", bytes);
    (void)entries_[ticket.slot].call.respond(std::move(result));
    entries_[ticket.slot] = {}; --active_; return true;
}
auto io_rx::abandon(io_ticket ticket) -> bool {
    if (!stopping_ || !valid(ticket)) return false;
    entries_[ticket.slot] = {}; --active_; return true;
}
void io_rx::stop() {
    stopping_ = true;
    for (size_t i = 0; i < entries_.size(); ++i) {
        auto& entry = entries_[i];
        if (!entry.command.id) continue;
        if (!entry.admitted) (void)finish({static_cast<uint32_t>(i), entry.command.id}, STATUS_CANCELED, 0);
        else if (!entry.committed) entry.cancelled = true;
    }
}

auto io_server::accept(io_operation operation, ubus_request& request, ubus_msg&) -> status_t {
    if (closing_) return STATUS_CLOSED;
    const auto id = *request.data["id"].as<uint64_t>();
    io_cmd command{.id=id, .operation=operation, .deadline=request.deadline};
    if (operation != io_operation::Flush) {
        const auto buffer = request.data["buffer"].as<ubus_buffer>();
        if (!buffer || buffer->offset > memory_.size || buffer->size > memory_.size - buffer->offset
            || buffer->size > io_buffer_size) return STATUS_BAD_ARGS;
        const auto cap = request.capability(request.data["buffer"]);
        const auto queried = sys::object_info(cap);
        if (queried.status != STATUS_OK) return queried.status;
        ObjectInfo info; ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info));
        if (info.object_id != object_id_) return STATUS_DENIED;
        command.buffer_offset = buffer->offset; command.length = buffer->size;
        const auto offset = request.data["offset"].as<uint64_t>();
        command.offset = offset.value_or(0); command.sequential = !offset;
    }
    return queue_.accept(command, request);
}
auto io_server::open(uint64_t value, uint64_t mode) -> status_t {
    if (!(mode & (io_read | io_write)) || (mode & ~(io_read | io_write))) return STATUS_BAD_ARGS;
    const bool writable = mode & io_write;
    value_ = value;
    status_t status{};
    auto memory = vm_area::create(rt_startup().alloc(), rt_vm(), 0, io_payload_size);
    if (!memory) return memory.error();
    memory_ = std::move(*memory);
    const auto inspected = sys::object_info(memory_.memory.selector());
    if (inspected.status != STATUS_OK) return inspected.status;
    ObjectInfo info; ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info)); object_id_ = info.object_id;
    const CapView view{.version=CAP_ATTENUATION_VERSION_CURRENT, .kind=ObjMem, .size=CAP_ATTENUATION_SIZE,
        .rights=RIGHT_MAP | RIGHT_INSPECT | RIGHT_DUPLICATE | RIGHT_REVOKE,
        .words={0, io_payload_size / 4096, VM_READ | (writable ? VM_WRITE : 0)}};
    status = object_.add("Buffer", {}, {{"buffer", ubus_type::Buffer}, {"capacity", ubus_type::U64}, {"sequence", ubus_type::U64}},
        [this, view](auto&, auto& result) {
            if (closing_) return STATUS_CLOSED;
            if (queue_.active()) return STATUS_BUSY;
            if (const auto status = retire(); status != STATUS_OK) return status;
            auto exported = descriptor_.derive(memory_.memory.selector(), view);
            if (!exported) return exported.error();
            export_ = std::move(*exported);
            const auto index = *result.attach(export_.selector(), RIGHT_MAP | RIGHT_INSPECT | RIGHT_DUPLICATE);
            result.data.put("buffer", ubus_buffer{index, 0, io_payload_size}).put("capacity", value_).put("sequence", queue_.sequence());
            return STATUS_OK;
        });
    if (status != STATUS_OK) return status;
    for (size_t i = 0; i < io_methods.size(); ++i) {
        const auto operation = static_cast<io_operation>(i + 1);
        if ((operation == io_operation::Write && !writable) || (operation == io_operation::Read && !(mode & io_read))) continue;
        ubus_schema input{{"id", ubus_type::U64}};
        if (operation != io_operation::Flush) {
            input.push_back({"offset", ubus_type::U64, true}); input.push_back({"buffer", ubus_type::Buffer});
        }
        status = object_.add(std::string{io_methods[i]}, std::move(input), {{"bytes", ubus_type::U64}},
            [this, operation](auto& request, auto& result) { return accept(operation, request, result); });
        if (status != STATUS_OK) return status;
    }
    status = object_.add("Cancel", {{"id", ubus_type::U64}}, {}, [this](auto& request, auto&) {
        return queue_.cancel(*request.data["id"].template as<uint64_t>());
    });
    if (status != STATUS_OK) return status;
    status = object_.add("Close", {}, {}, [this](auto& request, auto&) {
        if (closing_) return STATUS_CLOSED;
        closing_ = true; queue_.stop(); closing_call_.emplace(std::move(request)); return STATUS_PENDING;
    });
    return status == STATUS_OK ? object_.open(io_queue_depth) : status;
}
void io_server::abort() { closing_ = failed_ = true; queue_.stop(); }
auto io_server::poll() -> status_t {
    for (size_t i = 0; i < io_queue_depth; ++i) {
        const auto status = object_.poll();
        if (status == STATUS_WOULD_BLOCK) return STATUS_OK;
        if (status == STATUS_CLOSED || status == STATUS_PEER_CLOSED) { abort(); return STATUS_OK; }
        if (status != STATUS_OK) return status;
    }
    return object_.arm();
}
auto io_server::retire() -> status_t {
    if (export_) {
        const auto status = sys::cap_revoke(export_.selector()).status;
        if (status != STATUS_OK) return status;
        export_ = {};
    }
    return STATUS_OK;
}
io_server::~io_server() {
    if (queue_.active()) sys::exit(STATUS_BUSY);
    if (const auto status = retire(); status != STATUS_OK) sys::exit(status);
}
auto io_server::flush(bool close_ready, status_t close_status) -> status_t {
    if (!closing_ || !close_ready || queue_.active() || done_) return STATUS_OK;
    const auto status = retire();
    if (status != STATUS_OK) return status;
    if (closing_call_) {
        ubus_msg response; response.status = close_status;
        (void)closing_call_->respond(std::move(response)); closing_call_.reset();
    }
    done_ = true; return STATUS_OK;
}

auto io_client::open(cap_owner endpoint, bool writable, uint64_t* capacity) -> status_t {
    if (opened_) return STATUS_BUSY;
    auto status = connection_.adopt(std::move(endpoint));
    if (status != STATUS_OK) return status;
    ubus_msg response;
    status = connection_.invoke("Buffer", {}, response);
    if (status != STATUS_OK) return status;
    const auto buffer = response.data["buffer"].as<ubus_buffer>();
    const auto value = response.data["capacity"].as<uint64_t>();
    const auto sequence = response.data["sequence"].as<uint64_t>();
    if (!buffer || !value || !sequence || buffer->offset || buffer->size != io_payload_size) return STATUS_PEER_FAULT;
    memory_ = response.take(response.data["buffer"]);
    if (!memory_) return STATUS_PEER_FAULT;
    writable_ = writable; events_ = rt_startup().events; error_ = STATUS_OK; next_ = *sequence; opened_ = true;
    if (capacity) *capacity = *value;
    return STATUS_OK;
}
auto io_client::submit(io_cmd& command, void* data, bool exact) -> status_t {
    if (error_) return error_;
    if (!opened_ || next_ == UINT64_MAX || command.length > io_buffer_size
        || command.offset > UINT64_MAX - command.length
        || command.operation < io_operation::Read || command.operation > io_operation::Identify) return STATUS_BAD_ARGS;
    const auto operation = command.operation;
    if (operation == io_operation::Write && (!writable_ || (command.length && !data))) return STATUS_DENIED;
    if (operation == io_operation::Identify && next()) return STATUS_BUSY;
    poll();
    if (error_) return error_;
    // Preserve admission order even when a native send has not yet committed.
    for (const auto& entry : entries_) if (entry && entry->command.id && !entry->ready && !entry->call.sent()) return STATUS_BUSY;
    for (size_t i = 0; i < entries_.size(); ++i) if (!entries_[i] || !entries_[i]->command.id) {
        auto& entry = entries_[i];
        if (!entry) entry = std::make_unique<io_client::entry>();
        command.id = ++next_; command.buffer_offset = operation == io_operation::Identify ? 0 : i * io_buffer_size;
        if (operation == io_operation::Write && command.length) {
            const auto status = mem_write(memory_.selector(), command.buffer_offset,
                {reinterpret_cast<const std::byte*>(data), size_t(command.length)});
            if (status != STATUS_OK) return status;
        }
        const auto method = connection_.token(io_methods[static_cast<size_t>(command.operation) - 1]);
        if (!method) return STATUS_INVALID_OP;
        ubus_msg request; request.deadline = command.deadline; request.tag = *method;
        request.data.put("id", command.id);
        if (operation != io_operation::Flush) {
            const auto cap = *request.attach(memory_.selector(), RIGHT_MAP | RIGHT_INSPECT | RIGHT_DUPLICATE);
            request.data.put("buffer", ubus_buffer{cap, command.buffer_offset, command.length});
            if (!command.sequential) request.data.put("offset", command.offset);
        }
        const auto status = entry->call.begin(connection_.endpoint(), std::move(request), true);
        if (status != STATUS_OK) return status;
        entry->command = command; entry->destination = data; entry->exact = exact; return STATUS_OK;
    }
    return STATUS_BUSY;
}
void io_client::poll() {
    if (!opened_ || error_) return;
    if (const auto status = connection_.state(); status != STATUS_OK) { (void)fail(status); return; }
    for (auto& entry : entries_) if (entry && entry->command.id && !entry->ready) {
        ubus_msg response;
        const auto status = entry->call.poll(response);
        if (status == STATUS_WOULD_BLOCK) continue;
        const auto bytes = response.data["bytes"].as<uint64_t>();
        auto outcome = status != STATUS_OK ? status : response.status;
        if (outcome == STATUS_OK && (!bytes || *bytes > entry->command.length
            || (entry->exact && *bytes != entry->command.length))) { (void)fail(STATUS_PEER_FAULT); return; }
        auto amount = outcome == STATUS_OK ? *bytes : 0;
        if (amount && entry->destination && entry->command.operation != io_operation::Write)
            outcome = copy(entry->command.buffer_offset, {reinterpret_cast<std::byte*>(entry->destination), size_t(amount)});
        if (outcome != STATUS_OK) amount = 0;
        entry->result = {entry->command.id, outcome, amount}; entry->ready = true;
    }
}
auto io_client::completion(io_done& result, uint64_t id) -> status_t {
    poll();
    for (auto& entry : entries_) if (entry && entry->command.id && entry->ready && (!id || entry->command.id == id)) {
        result = entry->result; entry->command = {}; entry->ready = entry->exact = false; entry->destination = nullptr;
        return STATUS_OK;
    }
    return STATUS_WOULD_BLOCK;
}
auto io_client::wait(io_done& result, uint64_t id, uint64_t deadline) -> status_t {
    if (id && !contains(id)) return STATUS_NOT_FOUND;
    for (;;) {
        const auto status = completion(result, id);
        if (status != STATUS_WOULD_BLOCK || !next()) return status;
        const auto waited = ev_wait(events_, deadline).status;
        if (waited != STATUS_OK) return waited;
    }
}
auto io_client::cancel(uint64_t id) -> status_t {
    ubus_msg request, response; request.data.put("id", id);
    return connection_.invoke("Cancel", std::move(request), response);
}
auto io_client::close() -> status_t {
    if (!opened_) return STATUS_BAD_ARGS;
    ubus_msg response;
    const auto status = connection_.invoke("Close", {}, response);
    (void)fail(status == STATUS_OK ? STATUS_CLOSED : status);
    const auto unmapped = memory_.close();
    if (unmapped != STATUS_OK) return unmapped;
    for (auto& entry : entries_) if (entry) { entry->command = {}; entry->ready = false; entry->destination = nullptr; }
    opened_ = false;
    const auto closed = connection_.close();
    return status == STATUS_OK ? closed : status;
}
auto io_client::release() -> std::expected<cap_owner, status_t> {
    if (!opened_) return std::unexpected(STATUS_CLOSED);
    if (next()) return std::unexpected(STATUS_BUSY);
    const auto status = memory_.close();
    if (status != STATUS_OK) return std::unexpected(status);
    auto endpoint = connection_.release();
    if (endpoint) opened_ = false;
    return endpoint;
}
auto io_client::next(uint64_t after) const -> const io_cmd* {
    const io_cmd* first{};
    for (const auto& entry : entries_) if (entry && entry->command.id > after && (!first || entry->command.id < first->id)) first = &entry->command;
    return first;
}
auto io_client::contains(uint64_t id) const -> bool {
    return id && std::ranges::any_of(entries_, [&](const auto& entry) { return entry && entry->command.id == id; });
}
auto io_client::fail(status_t status) -> status_t {
    error_ = status;
    for (auto& entry : entries_) if (entry && entry->command.id && !entry->ready) {
        (void)entry->call.cancel(); entry->result = {entry->command.id, status, 0}; entry->ready = true;
    }
    return status;
}
