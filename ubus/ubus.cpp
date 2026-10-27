#include "ubus.hpp"
#include <vm.hpp>
#include <algorithm>

static constexpr uint16_t ubus_version = 2;
static constexpr uint16_t ubus_invoke = 1, ubus_reply = 2, ubus_event = 3, ubus_oneway = 4, ubus_bulk = 0x8000;
struct ubus_envelope { uint16_t version, kind; uint32_t bytes; int64_t status; uint64_t deadline; };
static_assert(sizeof(ubus_envelope) == 24);

auto ubus_msg::attach(cap_t cap, word_t rights) -> std::expected<size_t, status_t> {
    if (!cap) return std::unexpected(STATUS_INVALID_CAP);
    if (count == transfers.size()) return std::unexpected(STATUS_NO_MEMORY);
    transfers[count] = {cap, rights, CAP_COPY, 0}; return count++;
}
auto ubus_msg::offer(_cap_owner&& cap, word_t rights) -> std::expected<size_t, status_t> {
    const auto index = attach(cap.selector(), rights);
    if (index) caps[*index] = std::move(cap);
    return index;
}
auto ubus_msg::give(_cap_owner&& cap) -> std::expected<size_t, status_t> {
    const auto index = offer(std::move(cap), 0);
    if (index) transfers[*index].operation = CAP_MOVE;
    return index;
}
void ubus_msg::commit() {
    for (size_t i = 0; i < count; ++i) if (transfers[i].operation == CAP_MOVE) (void)caps[i].release();
}
void ubus_msg::forward() {
    for (size_t i = 0; i < count; ++i)
        if (caps[i]) transfers[i] = {caps[i].selector(), 0, CAP_MOVE, 0};
}
auto ubus_msg::clone() const -> std::expected<ubus_msg, status_t> {
    ubus_msg copy; copy.data = data; copy.status = status; copy.tag = tag; copy.deadline = deadline;
    for (size_t i = 0; i < count; ++i) {
        const auto source = caps[i] ? caps[i].selector() : transfers[i].source;
        const auto view = _cap_copy<CapInfo>(_cap_info(source));
        if (!view) return std::unexpected(view.error());
        const auto rights = transfers[i].rights ? transfers[i].rights : view->view.rights;
        const auto duplicated = _cap_duplicate(source, _startup().cspace, rights);
        if (duplicated.status != STATUS_OK) return std::unexpected(duplicated.status);
        const auto index = copy.offer(_cap_owner{duplicated.value}, rights);
        if (!index) return std::unexpected(index.error());
    }
    return copy;
}
static auto ubus_index(ubus_value value) -> std::optional<size_t> {
    if (const auto slice = value.as<ubus_buffer>()) return slice->cap;
    if (value.type() != ubus_type::Cap || value.data().size() != sizeof(uint64_t)) return {};
    uint64_t index; ::memcpy(&index, value.data().data(), sizeof(index)); return index;
}
auto ubus_msg::capability(ubus_value value) const -> cap_t {
    const auto index = ubus_index(value);
    return index && *index < count ? caps[*index].selector() : 0;
}
auto ubus_msg::take(ubus_value value) -> _cap_owner {
    const auto index = ubus_index(value);
    return index && *index < count ? std::move(caps[*index]) : _cap_owner{};
}
auto ubus_msg::slice(ubus_value value) const -> std::expected<ubus_msg, status_t> {
    ubus_msg selected;
    if (!selected.data.assign(value.bytes())) return std::unexpected(STATUS_BAD_ARGS);
    std::array<size_t, CHANNEL_MAX_CAPS> indices; indices.fill(SIZE_MAX);
    const auto visit = [&](auto&& self, ubus_value field) -> status_t {
        if (field.type() == ubus_type::Cap || field.type() == ubus_type::Buffer) {
            const auto original = ubus_index(field);
            if (!original || *original >= count) return STATUS_BAD_ARGS;
            auto& index = indices[*original];
            if (index == SIZE_MAX) {
                index = selected.count++;
                selected.transfers[index] = transfers[*original];
                selected.transfers[index].source = caps[*original] ? caps[*original].selector() : transfers[*original].source;
            }
            const uint64_t rebased = index;
            ::memcpy(const_cast<std::byte*>(field.data().data()), &rebased, sizeof(rebased));
        } else for (const auto child : field.values()) {
            const auto status = self(self, child); if (status != STATUS_OK) return status;
        }
        return STATUS_OK;
    };
    const auto status = visit(visit, selected.data.view());
    if (status != STATUS_OK) return std::unexpected(status);
    return selected;
}
static auto ubus_caps_valid(ubus_value value, size_t count) -> bool {
    if (value.type() == ubus_type::Cap) return ubus_index(value).value_or(SIZE_MAX) < count;
    if (value.type() == ubus_type::Buffer) return value.as<ubus_buffer>()->cap < count;
    for (auto child : value.values()) if (!ubus_caps_valid(child, count)) return false;
    return true;
}
auto ubus_wire::encode(const ubus_msg& message, uint16_t kind, size_t maxcaps) -> status_t {
    size = count = 0; words.fill(0);
    const auto bytes = message.data.bytes();
    if (bytes.size() > ubus_body_limit || !message.data.valid() || !ubus_caps_valid(message.data.view(), message.count)
        || message.count > maxcaps) return STATUS_BAD_ARGS;
    const bool bulk = bytes.size() > sizeof(words) - sizeof(ubus_envelope);
    if (bulk && message.count == maxcaps) return STATUS_NO_MEMORY;
    if (bulk) {
        const auto capacity = ubus_align((bytes.size() + 4095) & ~size_t{4095});
        if (!body || this->capacity < capacity) {
            const auto created = _memory_create(_startup().alloc(), capacity, VM_READ | VM_WRITE);
            if (created.status != STATUS_OK) return created.status;
            body = _cap_owner{created.value}; this->capacity = capacity;
        }
        const auto status = _mem_write(body.selector(), 0, bytes).status;
        if (status != STATUS_OK) return status;
        caps[count++] = {body.selector(), RIGHT_MAP | RIGHT_DUPLICATE, CAP_COPY, 0};
    } else ::memcpy(reinterpret_cast<std::byte*>(words.data()) + sizeof(ubus_envelope), bytes.data(), bytes.size());
    std::copy_n(message.transfers.begin(), message.count, caps.begin() + count); count += message.count;
    const ubus_envelope envelope{ubus_version, static_cast<uint16_t>(kind | (bulk ? ubus_bulk : 0)),
        static_cast<uint32_t>(bytes.size()), message.status, message.deadline};
    ::memcpy(words.data(), &envelope, sizeof(envelope));
    size = bulk ? sizeof(envelope) / 8 : ubus_align(sizeof(envelope) + bytes.size()) / 8;
    return STATUS_OK;
}
static auto ubus_decode(_ch_packet packet, ubus_msg& result, _cap_owner* reply = nullptr, size_t body_limit = ubus_body_limit) -> status_t {
    result = {};
    // Take the kernel Reply before interpreting any untrusted control bytes.
    size_t first{};
    if (reply && packet.called) {
        if (!packet.count) return STATUS_PEER_FAULT;
        *reply = std::move(packet.capabilities[0]); first = 1;
    }
    if (packet.word_count < sizeof(ubus_envelope) / 8) return STATUS_PEER_FAULT;
    ubus_envelope envelope; ::memcpy(&envelope, packet.words.data(), sizeof(envelope));
    const auto kind = envelope.kind & ~ubus_bulk;
    if (envelope.version != ubus_version || (reply ? (packet.called ? kind != ubus_invoke : kind != ubus_oneway && kind != ubus_event) : packet.called || (kind != ubus_reply && kind != ubus_event))
        || envelope.bytes < sizeof(ubus_head) || !std::in_range<status_t>(envelope.status)) return STATUS_PEER_FAULT;
    std::span<std::byte> bytes;
    if (envelope.kind & ubus_bulk) {
        if (first == packet.count || packet.word_count != sizeof(envelope) / 8) return STATUS_PEER_FAULT;
        const auto memory = packet.capabilities[first++].selector();
        // The allocation is charged to this process; the protocol does not
        // pretend that an untrusted peer can bypass its actual heap budget.
        if (envelope.bytes > ubus_body_limit) return STATUS_PEER_FAULT;
        if (envelope.bytes > body_limit) return STATUS_EXHAUSTED;
        bytes = result.data.storage(envelope.bytes);
        if (_mem_read(memory, 0, bytes).status != STATUS_OK) return STATUS_PEER_FAULT;
    } else {
        if (sizeof(envelope) + envelope.bytes > packet.word_count * 8) return STATUS_PEER_FAULT;
        const auto* data = reinterpret_cast<const std::byte*>(packet.words.data()) + sizeof(envelope);
        bytes = result.data.storage(envelope.bytes);
        ::memcpy(bytes.data(), data, bytes.size());
    }
    if (!result.data.valid()) return STATUS_PEER_FAULT;
    result.count = packet.count - first;
    for (size_t i = 0; i < result.count; ++i) {
        result.caps[i] = std::move(packet.capabilities[first + i]);
        result.transfers[i] = {result.caps[i].selector(), 0, CAP_COPY, 0};
    }
    result.status = envelope.status; result.tag = packet.tag;
    result.badge = packet.badge; result.deadline = envelope.deadline;
    return ubus_caps_valid(result.data.view(), result.count) ? STATUS_OK : STATUS_PEER_FAULT;
}
static auto ubus_emit(cap_t endpoint, ubus_msg& message, uint16_t kind, bool block) -> status_t {
    ubus_wire wire;
    const auto status = wire.encode(message, kind, CHANNEL_MAX_CAPS);
    if (status != STATUS_OK) return status;
    const auto sent = (block ? _ch_send(endpoint, std::span{wire.words}.first(wire.size), std::span{wire.caps}.first(wire.count), message.tag) : _ch_try_send(endpoint, std::span{wire.words}.first(wire.size), std::span{wire.caps}.first(wire.count), message.tag)).status;
    if (sent == STATUS_OK) message.commit();
    return sent;
}
auto ubus_send(cap_t endpoint, ubus_msg& message, bool block) -> status_t { return ubus_emit(endpoint, message, ubus_event, block); }
auto ubus_notify(cap_t endpoint, ubus_msg& message, bool block) -> status_t { return ubus_emit(endpoint, message, ubus_oneway, block); }
auto ubus_receive(cap_t endpoint, ubus_request& request, bool block) -> status_t {
    request = {};
    _ch_packet packet;
    const auto status = (block ? _ch_recv(endpoint, packet) : _ch_try_recv(endpoint, packet)).status;
    return status == STATUS_OK ? ubus_decode(std::move(packet), request, &request.reply) : status;
}
auto ubus_receive_event(cap_t endpoint, ubus_msg& message, bool block) -> status_t {
    _ch_packet packet;
    const auto status = (block ? _ch_recv(endpoint, packet) : _ch_try_recv(endpoint, packet)).status;
    return status == STATUS_OK ? ubus_decode(std::move(packet), message) : status;
}
auto ubus_rx::receive(ubus_request& request) -> _sys_result {
    request = {};
    _ch_packet packet;
    auto result = input_.receive(packet);
    if (result.status == STATUS_OK) result.status = ubus_decode(std::move(packet), request, &request.reply, body_limit_);
    return result;
}
auto ubus_rx::receive(ubus_msg& message) -> status_t {
    _ch_packet packet;
    const auto status = input_.receive(packet).status;
    return status == STATUS_OK ? ubus_decode(std::move(packet), message, nullptr, body_limit_) : status;
}
auto ubus_call::prepare(cap_t endpoint, size_t bytes) -> status_t {
    if (!endpoint || bytes > ubus_body_limit) return STATUS_BAD_ARGS;
    if (const auto status = rpc_.prepare(_startup().alloc(), &_events()); status != STATUS_OK) return status;
    if (peer_ != endpoint) { wire_ = {}; peer_ = endpoint; }
    if (bytes > sizeof(wire_.words) - sizeof(ubus_envelope) && wire_.capacity < bytes) {
        const auto size = (bytes + 4095) & ~size_t{4095};
        const auto created = _memory_create(_startup().alloc(), size, VM_READ | VM_WRITE);
        if (created.status != STATUS_OK) return created.status;
        _cap_owner memory{created.value};
        for (size_t page = 0; page < size / 4096; ++page) {
            const auto status = _memory_populate(memory.selector(), page).status;
            if (status != STATUS_OK) return status;
        }
        wire_.body = std::move(memory); wire_.capacity = size;
    }
    return STATUS_OK;
}
auto ubus_call::begin(cap_t endpoint, ubus_msg message, bool drain) -> status_t {
    if (rpc_.active()) return STATUS_BUSY;
    if (peer_ != endpoint) { wire_ = {}; peer_ = endpoint; }
    message_ = std::move(message);
    const auto& pending = message_;
    const auto status = wire_.encode(pending, ubus_invoke, CHANNEL_MAX_CAPS - 1);
    if (status != STATUS_OK) { message_ = {}; wire_ = {}; return status; }
    const auto submitted = rpc_.begin(endpoint, _startup().alloc(), &_events(),
        std::span{wire_.words}.first(wire_.size), std::span{wire_.caps}.first(wire_.count), pending.tag, pending.deadline, drain ? _rpc_deadline::Admission : _rpc_deadline::Cancel);
    if (rpc_.sent()) message_.commit();
    if (submitted != STATUS_OK) { message_ = {}; wire_ = {}; }
    return submitted;
}
auto ubus_call::commit() -> status_t {
    const auto status = rpc_.commit();
    if (rpc_.sent()) message_.commit();
    return status;
}
auto ubus_call::poll(ubus_msg& result) -> status_t {
    _ch_packet packet;
    const auto status = rpc_.poll(packet);
    if (rpc_.sent()) message_.commit();
    if (status == STATUS_WOULD_BLOCK) return status;
    message_ = {};
    const auto decoded = status == STATUS_OK ? ubus_decode(std::move(packet), result) : status;
    // A reply retires this peer's read of the request. Keep at most one page;
    // cancellation/errors discard it so an unfinished reader sees its old body.
    if (decoded != STATUS_OK || wire_.capacity > 4096) wire_ = {};
    return decoded;
}
auto ubus_call::cancel() -> status_t {
    const auto status = rpc_.cancel();
    wire_ = {};
    if (status == STATUS_OK) message_ = {};
    return status;
}
auto ubus_call::wait(ubus_msg& result) -> status_t {
    for (;;) {
        const auto status = poll(result);
        if (status != STATUS_WOULD_BLOCK) return status;
        const auto waited = rpc_.wait();
        if (waited != STATUS_OK) { (void)cancel(); return waited; }
    }
}
static auto ubus_matches(ubus_value, const ubus_schema&) -> bool;
static auto ubus_match(ubus_value value, const ubus_field& field) -> bool {
    if (value.type() != field.type || value.data().size() > field.max_bytes) return false;
    if (field.atom) {
        if (value.data().size() < sizeof(ubus_vector)) return false;
        ubus_vector header; ::memcpy(&header, value.data().data(), sizeof(header));
        if (header.atom != *field.atom) return false;
    }
    if (field.fields.empty()) return true;
    if (field.fields.size() == 1 && field.fields.front().name.empty()) {
        for (const auto item : value.values()) if (!ubus_match(item, field.fields.front())) return false;
        return true;
    }
    return ubus_matches(value, field.fields);
}
static auto ubus_matches(ubus_value data, const ubus_schema& schema) -> bool {
    if (data.type() != ubus_type::Record) return false;
    for (const auto& field : schema) {
        const auto value = data[field.name];
        if (value ? !ubus_match(value, field) : !field.optional) return false;
    }
    for (const auto value : data.values())
        if (std::ranges::none_of(schema, [&](const auto& field) { return field.name == value.name(); })) return false;
    return true;
}
static auto ubus_schema_check(ubus_schema& schema, bool unnamed = false, size_t depth = ubus_depth) -> bool {
    if (!depth) return false;
    std::ranges::sort(schema, {}, &ubus_field::name);
    for (size_t i = 0; i < schema.size(); ++i) {
        auto& field = schema[i];
        if ((field.name.empty() && !(unnamed && schema.size() == 1)) || field.type > ubus_type::Vector
            || (i && schema[i - 1].name == field.name)) return false;
        if (field.atom && (field.type != ubus_type::Vector || *field.atom > ubus_atom::F64)) return false;
        if (field.fields.empty()) continue;
        if (field.type != ubus_type::Record && field.type != ubus_type::Array) return false;
        if (field.type == ubus_type::Array && (field.fields.size() != 1 || !field.fields.front().name.empty())) return false;
        if (!ubus_schema_check(field.fields, true, depth - 1)) return false;
    }
    return true;
}
auto ubus_request::reserve_reply(size_t bytes) -> status_t {
    if (bytes > ubus_body_limit) return STATUS_BAD_ARGS;
    if (bytes <= reply_bytes) return STATUS_OK;
    const auto size = (bytes + 4095) & ~size_t{4095};
    const auto created = _memory_create(_startup().alloc(), size, VM_READ | VM_WRITE);
    if (created.status != STATUS_OK) return created.status;
    _cap_owner memory{created.value};
    for (size_t page = 0; page < size / 4096; ++page) {
        const auto status = _memory_populate(memory.selector(), page).status;
        if (status != STATUS_OK) return status;
    }
    reply_body = std::move(memory); reply_bytes = size; return STATUS_OK;
}
auto ubus_request::respond(ubus_msg response, bool* committed) -> status_t {
    if (committed) *committed = false;
    if (!reply) return STATUS_INVALID_OP;
    bool original = true;
    if (response.status == STATUS_OK && method && (!response.data.valid() || !ubus_matches(response.data.view(), method->output))) {
        response = {}; response.status = STATUS_INTERNAL; original = false;
    }
    ubus_wire wire; wire.body = std::move(reply_body); wire.capacity = std::exchange(reply_bytes, 0);
    const auto encoded = wire.encode(response, ubus_reply, CHANNEL_MAX_CAPS);
    if (encoded != STATUS_OK) {
        response = {}; response.status = encoded; original = false;
        (void)wire.encode(response, ubus_reply, CHANNEL_MAX_CAPS);
    }
    const auto status = _ch_try_send(reply.selector(), std::span{wire.words}.first(wire.size), std::span{wire.caps}.first(wire.count)).status;
    if (status == STATUS_OK) { response.commit(); if (committed) *committed = original; }
    reply = {};
    return status == STATUS_CANCELED || status == STATUS_STALE || status == STATUS_CLOSED
        || status == STATUS_PEER_CLOSED ? STATUS_OK : status;
}
static auto ubus_fields(const ubus_schema& fields) -> ubus_data {
    ubus_data result{ubus_type::Array};
    for (const auto& field : fields) {
        ubus_data item; item.put("name", field.name).put("type", static_cast<uint64_t>(field.type)).put("optional", field.optional);
        if (field.max_bytes != UINT64_MAX) item.put("max_bytes", field.max_bytes);
        if (!field.fields.empty()) item.put("fields", ubus_fields(field.fields));
        if (field.atom) item.put("atom", uint64_t(*field.atom));
        result.push(item);
    }
    return result;
}
auto ubus_object::describe(word_t facet) const -> ubus_data {
    ubus_data methods{ubus_type::Array};
    for (size_t i = 0; i < methods_.size(); ++i) {
        const auto& method = *methods_[i];
        if ((facet & method.required) != method.required || (access_ && access_(facet, method.name) != STATUS_OK)) continue;
        ubus_data item;
        item.put("name", method.name).put("token", i + 2).put("input", ubus_fields(method.input)).put("output", ubus_fields(method.output)).put("notify", method.notify);
        methods.push(item);
    }
    ubus_data result;
    result.put("interface", interface_).put("version", version_).put("features", features_).put("methods", methods);
    return result;
}
auto ubus_object::add(std::string name, ubus_schema input, ubus_schema output, ubus_handler handler,
                      word_t required, bool notify) -> status_t {
    if (sealed_) return STATUS_BUSY;
    if (!ubus_schema_check(input) || !ubus_schema_check(output)) return STATUS_BAD_ARGS;
    // Describe wraps fields in object/methods/method containers. Registration
    // must only accept schemas representable by the public value encoding.
    if (!ubus_fields(input).view().valid(ubus_depth - 3) || !ubus_fields(output).view().valid(ubus_depth - 3)) return STATUS_BAD_ARGS;
    if (name.empty() || !handler || std::ranges::any_of(methods_, [&](const auto& m) { return m->name == name; })) return STATUS_BAD_ARGS;
    methods_.push_back(std::make_shared<ubus_method>(std::move(name), std::move(input), std::move(output), std::move(handler), required, notify));
    return STATUS_OK;
}
ubus_object::~ubus_object() {
    if (const auto status = close(); status != STATUS_OK) _thread_exit(status);
}
auto ubus_object::close() -> status_t {
    if (const auto status = input_.close(); status != STATUS_OK) return status;
    if (const auto status = closing_.close(); status != STATUS_OK) return status;
    if (scope_) {
        // Scope owns the transport. Drop local slots before closing it, so the
        // pair never tries to destroy a member already retiring with its Scope.
        for (auto& end : channel_.ends) if (const auto status = end.close(); status != STATUS_OK) return status;
        const auto status = _scope_close(scope_.selector()).status; if (status != STATUS_OK) return status;
        if (const auto status = scope_.close(); status != STATUS_OK) return status;
    } else if (const auto status = channel_.close(); status != STATUS_OK) return status;
    return budget_.close();
}
auto ubus_object::open(size_t depth, cap_t quota, _cap_owner scope, size_t body_limit) -> status_t {
    if (body_limit < sizeof(ubus_head) || body_limit > ubus_body_limit) return STATUS_BAD_ARGS;
    if (endpoint()) return STATUS_BUSY;
    scope_ = std::move(scope);
    auto alloc = _startup().alloc();
    if (quota) {
        const auto cap = _cap_duplicate(quota, _startup().cspace, RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (cap.status != STATUS_OK) return cap.status;
        budget_ = _cap_owner{cap.value};
        if (!scope_) {
            const auto scope = _scope_create(alloc); if (scope.status != STATUS_OK) return scope.status;
            scope_ = _cap_owner{scope.value};
        }
        const auto status = closing_.open(&_events(), quota, STATE_CLOSING | STATE_DRAINED);
        if (status != STATUS_OK) return status;
        alloc.quota = budget_.selector();
    }
    if (scope_) alloc.scope = scope_.selector();
    auto status = channel_.open(alloc, depth, CHANNEL_MAX_CAPS);
    if (status == STATUS_OK) status = input_.open(endpoint(), &_events(), body_limit);
    return status;
}
auto ubus_object::connector(word_t facet) -> std::expected<_cap_owner, status_t> {
    if (facet > UINT64_MAX - 1 || !endpoint()) return std::unexpected(STATUS_BAD_ARGS);
    sealed_ = true;
    const auto cap = _channel_mint(channel_.ends[1].selector(), _startup().cspace, facet + 1,
        RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE);
    if (cap.status != STATUS_OK) return std::unexpected(cap.status);
    return _cap_owner{cap.value};
}
auto ubus_object::handle(word_t facet, word_t rights) -> std::expected<_cap_owner, status_t> {
    if (facet > UINT64_MAX - 1 || !channel_.ends[1]) return std::unexpected(STATUS_BAD_ARGS);
    const auto minted = _channel_mint(channel_.ends[1].selector(), _startup().cspace,
        facet + 1, rights);
    if (minted.status != STATUS_OK) return std::unexpected(minted.status);
    _cap_owner result{minted.value};
    const auto status = channel_.ends[1].close();
    if (status != STATUS_OK) return std::unexpected(status);
    sealed_ = true;
    return result;
}
auto ubus_object::bind(ubus_request& request) -> status_t {
    const auto input = request.data.view();
    const auto interface = input["interface"].as<std::string_view>();
    const auto minimum = input["minimum"].as<uint64_t>(), maximum = input["maximum"].as<uint64_t>();
    const auto features = input["features"].as<uint64_t>();
    ubus_msg response;
    const auto authorized = access_ ? access_(request.facet(), {}) : STATUS_OK;
    if (const auto admitted = _deadline_status(request.deadline); admitted != STATUS_OK) response.status = admitted;
    else if (authorized != STATUS_OK) response.status = authorized;
    else if (!interface || !minimum || !maximum || !features || (!interface->empty() && *interface != interface_)
        || *minimum > version_ || *maximum < version_ || (*features & features_) != *features) response.status = STATUS_BAD_ARGS;
    else {
        response.data = describe(request.facet());
    }
    return request.respond(std::move(response));
}
auto ubus_object::poll() -> status_t {
    ubus_request request;
    const auto received = input_.receive(request);
    auto status = received.status;
    if (status == STATUS_NO_MEMORY && received.value2) {
        status = input_.drop(received.value2);
        return status == STATUS_STALE ? STATUS_OK : status;
    }
    if (status == STATUS_WOULD_BLOCK && budget_) {
        const auto state = _source_info(budget_.selector());
        if (state.status == STATUS_BUSY || state.status == STATUS_RETRY) return STATUS_WOULD_BLOCK;
        if (state.status != STATUS_OK) return STATUS_CLOSED;
        if (!(state.value & STATE_OPEN)) return STATUS_CLOSED;
    }
    if (status == STATUS_WOULD_BLOCK) return status;
    if (status != STATUS_OK) {
        if (request.reply) { ubus_msg error; error.status = status; (void)request.respond(std::move(error)); }
        // RECEIVE consumed this message even when an attached grant retired.
        return status == STATUS_PEER_FAULT || status == STATUS_TRANSFER_FAILED || status == STATUS_BAD_ARGS
            || status == STATUS_BAD_RIGHTS || status == STATUS_EXHAUSTED ? STATUS_OK : status;
    }
    if (request.tag == 1) return request.reply ? bind(request) : STATUS_OK;
    ubus_msg response;
    response.status = _deadline_status(request.deadline);
    const auto facet = request.facet();
    if (!request.badge) response.status = STATUS_BAD_RIGHTS;
    else if (access_ && access_(facet, {}) != STATUS_OK) response.status = STATUS_CLOSED;
    else if (!request.tag) response.data = describe(facet);
    else if (request.tag < 2 || request.tag - 2 >= methods_.size()) response.status = STATUS_INVALID_OP;
    else if (response.status == STATUS_OK) {
        request.method = methods_[request.tag - 2];
        if ((facet & request.method->required) != request.method->required) response.status = STATUS_BAD_RIGHTS;
        else if (access_ && access_(facet, request.method->name) != STATUS_OK) response.status = STATUS_DENIED;
        else if (!request.reply && !request.method->notify) response.status = STATUS_BAD_ARGS;
        else if (!ubus_matches(request.data.view(), request.method->input)) response.status = STATUS_BAD_ARGS;
        else {
            response.status = request.method->handler(request, response);
            if (response.status == STATUS_PENDING && !request.reply) return STATUS_OK;
            if (response.status == STATUS_PENDING) response.status = STATUS_INTERNAL;
        }
    }
    return request.reply ? request.respond(std::move(response)) : STATUS_OK;
}
auto ubus_conn::begin_bind(cap_t connector, std::string_view interface, uint64_t minimum,
                           uint64_t maximum, uint64_t features, uint64_t deadline) -> status_t {
    if (call_.active()) return STATUS_BUSY;
    const auto duplicated = _cap_duplicate(connector, _startup().cspace, RIGHT_SEND | RIGHT_DUPLICATE);
    if (duplicated.status != STATUS_OK) return duplicated.status;
    _cap_owner endpoint{duplicated.value};
    ubus_msg request; request.tag = 1; request.deadline = deadline;
    request.data.put("interface", interface).put("minimum", minimum).put("maximum", maximum).put("features", features);
    const auto status = call_.begin(endpoint.selector(), std::move(request));
    if (status == STATUS_OK) binding_.emplace(std::string{interface}, minimum, maximum, features, std::move(endpoint));
    return status;
}
auto ubus_conn::bound(ubus_msg& response) -> status_t {
    auto pending = std::move(*binding_); binding_.reset();
    if (response.status != STATUS_OK) return response.status;
    return install(std::move(pending.endpoint), response.data.view(), pending.interface,
        pending.minimum, pending.maximum, pending.features);
}
auto ubus_conn::poll_bind() -> status_t {
    if (!binding_) return STATUS_INVALID_OP;
    ubus_msg response;
    const auto status = call_.poll(response);
    if (status == STATUS_OK) return bound(response);
    if (status != STATUS_WOULD_BLOCK) binding_.reset();
    return status;
}
auto ubus_conn::bind(cap_t connector, std::string_view interface, uint64_t minimum,
                     uint64_t maximum, uint64_t features) -> status_t {
    auto status = begin_bind(connector, interface, minimum, maximum, features);
    if (status != STATUS_OK) return status;
    ubus_msg response;
    status = call_.wait(response);
    if (status == STATUS_OK) return bound(response);
    binding_.reset(); return status;
}
auto ubus_conn::install(_cap_owner endpoint, ubus_value object, std::string_view interface,
                        uint64_t minimum, uint64_t maximum, uint64_t features) -> status_t {
    ubus_data description;
    const auto version = object["version"].as<uint64_t>();
    const auto actual = object["interface"].as<std::string_view>();
    const auto supported = object["features"].as<uint64_t>();
    if (!endpoint || !version || !actual || !supported || *version < minimum || *version > maximum
        || (!interface.empty() && *actual != interface) || (*supported & features) != features
        || object["methods"].type() != ubus_type::Array || !description.assign(object.bytes())) return STATUS_PEER_FAULT;
    endpoint_ = std::move(endpoint); description_ = std::move(description); return STATUS_OK;
}
auto ubus_conn::begin_adopt(_cap_owner endpoint, std::string_view interface, uint64_t minimum,
                            uint64_t maximum, uint64_t features, uint64_t deadline) -> status_t {
    if (call_.active()) return STATUS_BUSY;
    ubus_msg request; request.deadline = deadline;
    const auto status = call_.begin(endpoint.selector(), std::move(request));
    if (status == STATUS_OK) binding_.emplace(std::string{interface}, minimum, maximum, features, std::move(endpoint));
    return status;
}
auto ubus_conn::adopt(_cap_owner endpoint, std::string_view interface, uint64_t minimum,
                      uint64_t maximum, uint64_t features) -> status_t {
    auto status = begin_adopt(std::move(endpoint), interface, minimum, maximum, features);
    if (status != STATUS_OK) return status;
    ubus_msg response;
    status = call_.wait(response);
    if (status == STATUS_OK) return bound(response);
    binding_.reset(); return status;
}
auto ubus_conn::state() const -> status_t {
    if (!endpoint_) return STATUS_CLOSED;
    const auto source = _source_info(endpoint(), SOURCE_WRITE);
    return source.status != STATUS_OK ? source.status : source.value & SOURCE_TERMINAL ? STATUS_PEER_CLOSED : STATUS_OK;
}
auto ubus_conn::close() -> status_t {
    binding_.reset();
    const auto status = call_.cancel();
    if (status != STATUS_OK) return status;
    description_ = {};
    return endpoint_.close();
}
auto ubus_conn::release() -> std::expected<_cap_owner, status_t> {
    if (!endpoint_) return std::unexpected(STATUS_CLOSED);
    if (active()) return std::unexpected(STATUS_BUSY);
    description_ = {};
    return std::move(endpoint_);
}
auto ubus_conn::token(std::string_view name) const -> std::optional<word_t> {
    for (auto method : description_["methods"].values())
        if (method["name"].as<std::string_view>() == name) return method["token"].as<word_t>();
    return {};
}
auto ubus_conn::begin(std::string_view name, ubus_msg request, bool drain) -> status_t {
    const auto method = token(name);
    if (!method) return STATUS_INVALID_OP;
    // The native tag carries the method; no duplicate request sequence exists.
    request.tag = *method;
    return call_.begin(endpoint(), std::move(request), drain);
}
auto ubus_conn::invoke(std::string_view name, ubus_msg request, ubus_msg& response) -> status_t {
    const auto status = begin(name, std::move(request));
    if (status != STATUS_OK) return status;
    const auto received = call_.wait(response);
    return received == STATUS_OK ? response.status : received;
}

auto ubus_conn::notify(std::string_view name, ubus_msg request, bool block) -> status_t {
    for (const auto method : description_["methods"].values())
        if (method["name"].as<std::string_view>() == name) {
            if (!method["notify"].as<bool>().value_or(false)) return STATUS_INVALID_OP;
            const auto token = method["token"].as<word_t>();
            if (!token) return STATUS_PEER_FAULT;
            request.tag = *token;
            return ubus_notify(endpoint(), request, block);
        }
    return STATUS_INVALID_OP;
}
