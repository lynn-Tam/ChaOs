#include "ubus.hpp"
#include <algorithm>

static constexpr uint16_t ubus_invoke = 1, ubus_reply = 2, ubus_event = 3, ubus_oneway = 4, ubus_bulk = 0x8000;
struct ubus_envelope { uint16_t version, kind; uint32_t bytes; int64_t status; uint64_t deadline; };
static_assert(sizeof(ubus_envelope) == 24);

auto ubus_msg::attach(cap_t cap, word_t rights) -> std::optional<size_t> {
    if (!cap || count == transfers.size()) return {};
    transfers[count] = {cap, rights, CAP_COPY, 0}; return count++;
}
auto ubus_msg::offer(cap_owner cap, word_t rights) -> std::optional<size_t> {
    const auto index = attach(cap.selector(), rights);
    if (index) caps[*index] = std::move(cap);
    return index;
}
auto ubus_msg::give(cap_owner cap) -> std::optional<size_t> {
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
        const auto view = cap_info(source);
        if (!view) return std::unexpected(view.error());
        const auto rights = transfers[i].rights ? transfers[i].rights : view->view.rights;
        const auto duplicated = sys::cap_duplicate(source, rt_startup().cspace, rights);
        if (duplicated.status != STATUS_OK) return std::unexpected(duplicated.status);
        (void)copy.offer(cap_owner{duplicated.value}, rights);
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
auto ubus_msg::take(ubus_value value) -> cap_owner {
    const auto index = ubus_index(value);
    return index && *index < count ? std::move(caps[*index]) : cap_owner{};
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
    body = {}; size = count = 0; words.fill(0);
    const auto bytes = message.data.bytes();
    if (bytes.size() > ubus_body_limit || !message.data.valid() || !ubus_caps_valid(message.data.view(), message.count)
        || message.count > maxcaps) return STATUS_BAD_ARGS;
    const bool bulk = bytes.size() > sizeof(words) - sizeof(ubus_envelope);
    if (bulk && message.count == maxcaps) return STATUS_NO_MEMORY;
    if (bulk) {
        const auto created = sys::memory_create(rt_startup().alloc(), ubus_align((bytes.size() + 4095) & ~size_t{4095}), VM_READ | VM_WRITE);
        if (created.status != STATUS_OK) return created.status;
        body = cap_owner{created.value};
        const auto status = mem_write(body.selector(), 0, bytes);
        if (status != STATUS_OK) return status;
        caps[count++] = {body.selector(), RIGHT_MAP | RIGHT_DUPLICATE, CAP_COPY, 0};
    } else ::memcpy(reinterpret_cast<std::byte*>(words.data()) + sizeof(ubus_envelope), bytes.data(), bytes.size());
    std::copy_n(message.transfers.begin(), message.count, caps.begin() + count); count += message.count;
    const ubus_envelope envelope{1, static_cast<uint16_t>(kind | (bulk ? ubus_bulk : 0)),
        static_cast<uint32_t>(bytes.size()), message.status, message.deadline};
    ::memcpy(words.data(), &envelope, sizeof(envelope));
    size = bulk ? sizeof(envelope) / 8 : ubus_align(sizeof(envelope) + bytes.size()) / 8;
    return STATUS_OK;
}
static auto ubus_decode(ch_packet packet, ubus_msg& result, cap_owner* reply = nullptr) -> status_t {
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
    if (envelope.version != 1 || (reply ? kind != (packet.called ? ubus_invoke : ubus_oneway) : packet.called || (kind != ubus_reply && kind != ubus_event))
        || envelope.bytes < sizeof(ubus_head) || !std::in_range<status_t>(envelope.status)) return STATUS_PEER_FAULT;
    std::span<std::byte> bytes;
    if (envelope.kind & ubus_bulk) {
        if (first == packet.count || packet.word_count != sizeof(envelope) / 8) return STATUS_PEER_FAULT;
        const auto memory = packet.capabilities[first++].selector();
        // The allocation is charged to this process; the protocol does not
        // pretend that an untrusted peer can bypass its actual heap budget.
        if (envelope.bytes > ubus_body_limit) return STATUS_PEER_FAULT;
        bytes = result.data.storage(envelope.bytes);
        if (mem_read(memory, 0, bytes) != STATUS_OK) return STATUS_PEER_FAULT;
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
    const auto sent = ch_send(endpoint, std::span{wire.words}.first(wire.size), block,
        std::span{wire.caps}.first(wire.count), message.tag).status;
    if (sent == STATUS_OK) message.commit();
    return sent;
}
auto ubus_send(cap_t endpoint, ubus_msg& message, bool block) -> status_t { return ubus_emit(endpoint, message, ubus_event, block); }
auto ubus_notify(cap_t endpoint, ubus_msg& message, bool block) -> status_t { return ubus_emit(endpoint, message, ubus_oneway, block); }
auto ubus_receive(cap_t endpoint, ubus_request& request, bool block) -> status_t {
    request = {};
    ch_packet packet;
    const auto status = ch_recv(endpoint, packet, block).status;
    return status == STATUS_OK ? ubus_decode(std::move(packet), request, &request.reply) : status;
}
auto ubus_receive_event(cap_t endpoint, ubus_msg& message, bool block) -> status_t {
    ch_packet packet;
    const auto status = ch_recv(endpoint, packet, block).status;
    return status == STATUS_OK ? ubus_decode(std::move(packet), message) : status;
}
auto ubus_rx::receive(ubus_request& request) -> status_t {
    request = {};
    ch_packet packet;
    const auto status = input_.receive(packet);
    return status == STATUS_OK ? ubus_decode(std::move(packet), request, &request.reply) : status;
}
auto ubus_rx::receive(ubus_msg& message) -> status_t {
    ch_packet packet;
    const auto status = input_.receive(packet);
    return status == STATUS_OK ? ubus_decode(std::move(packet), message) : status;
}
auto ubus_call::begin(cap_t endpoint, ubus_msg message, bool drain) -> status_t {
    if (rpc_.active()) return STATUS_BUSY;
    message_ = std::move(message);
    const auto& pending = message_;
    const auto status = wire_.encode(pending, ubus_invoke, CHANNEL_MAX_CAPS - 1);
    if (status != STATUS_OK) { message_ = {}; wire_ = {}; return status; }
    const auto submitted = rpc_.begin(endpoint, rt_startup().alloc(), rt_startup().events,
        std::span{wire_.words}.first(wire_.size), std::span{wire_.caps}.first(wire_.count), pending.tag, pending.deadline, drain);
    if (rpc_.sent()) message_.commit();
    if (submitted != STATUS_OK) { message_ = {}; wire_ = {}; }
    return submitted;
}
auto ubus_call::poll(ubus_msg& result) -> status_t {
    ch_packet packet;
    const auto status = rpc_.poll(packet);
    if (rpc_.sent()) message_.commit();
    if (status == STATUS_WOULD_BLOCK) return status;
    message_ = {}; wire_ = {};
    return status == STATUS_OK ? ubus_decode(std::move(packet), result) : status;
}
auto ubus_call::cancel() -> status_t {
    const auto status = rpc_.cancel();
    if (status == STATUS_OK) { message_ = {}; wire_ = {}; }
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
auto ubus_request::respond(ubus_msg response, bool* committed) -> status_t {
    if (committed) *committed = false;
    if (!reply) return STATUS_INVALID_OP;
    bool original = true;
    if (response.status == STATUS_OK && method && (!response.data.valid() || !ubus_matches(response.data.view(), method->output))) {
        response = {}; response.status = STATUS_INTERNAL; original = false;
    }
    ubus_wire wire;
    const auto encoded = wire.encode(response, ubus_reply, CHANNEL_MAX_CAPS);
    if (encoded != STATUS_OK) {
        response = {}; response.status = encoded; original = false;
        (void)wire.encode(response, ubus_reply, CHANNEL_MAX_CAPS);
    }
    const auto status = ch_send(reply.selector(), std::span{wire.words}.first(wire.size), false,
        std::span{wire.caps}.first(wire.count)).status;
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
    if (const auto status = close(); status != STATUS_OK) sys::exit(status);
}
auto ubus_object::close() -> status_t {
    if (const auto status = input_.close(); status != STATUS_OK) return status;
    if (const auto status = closing_.close(); status != STATUS_OK) return status;
    if (scope_) {
        // Scope owns the transport. Drop local slots before closing it, so the
        // pair never tries to destroy a member already retiring with its Scope.
        for (auto& end : channel_.ends) if (const auto status = end.close(); status != STATUS_OK) return status;
        const auto status = sys::scope_close(scope_.selector()).status; if (status != STATUS_OK) return status;
        if (const auto status = scope_.close(); status != STATUS_OK) return status;
    } else if (const auto status = channel_.close(); status != STATUS_OK) return status;
    return budget_.close();
}
auto ubus_object::open(size_t depth, cap_t quota, cap_owner scope) -> status_t {
    if (endpoint()) return STATUS_BUSY;
    scope_ = std::move(scope);
    auto alloc = rt_startup().alloc();
    if (quota) {
        const auto cap = sys::cap_duplicate(quota, rt_startup().cspace, RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (cap.status != STATUS_OK) return cap.status;
        budget_ = cap_owner{cap.value};
        if (!scope_) {
            const auto scope = sys::scope_create(alloc); if (scope.status != STATUS_OK) return scope.status;
            scope_ = cap_owner{scope.value};
        }
        const auto status = closing_.open(rt_startup().events, quota, STATE_CLOSING | STATE_DRAINED);
        if (status != STATUS_OK) return status;
        alloc.quota = budget_.selector();
    }
    if (scope_) alloc.scope = scope_.selector();
    auto status = channel_.open(alloc, depth, CHANNEL_MAX_CAPS);
    if (status == STATUS_OK) status = input_.open(endpoint(), rt_startup().events);
    return status;
}
auto ubus_object::connector(word_t facet) -> std::expected<cap_owner, status_t> {
    if (facet > (UINT64_MAX - 2) / 2 || !endpoint()) return std::unexpected(STATUS_BAD_ARGS);
    sealed_ = true;
    const auto cap = sys::channel_mint(channel_.ends[1].selector(), rt_startup().cspace, facet * 2 + 1,
        RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE);
    if (cap.status != STATUS_OK) return std::unexpected(cap.status);
    return cap_owner{cap.value};
}
auto ubus_object::handle(word_t facet, word_t rights) -> std::expected<cap_owner, status_t> {
    if (facet > (UINT64_MAX - 2) / 2 || !channel_.ends[1]) return std::unexpected(STATUS_BAD_ARGS);
    const auto minted = sys::channel_mint(channel_.ends[1].selector(), rt_startup().cspace,
        facet * 2 + 2, rights);
    if (minted.status != STATUS_OK) return std::unexpected(minted.status);
    cap_owner result{minted.value};
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
    const auto authorized = access_ ? access_(request.badge / 2, {}) : STATUS_OK;
    if (const auto admitted = rt_admit(request.deadline); admitted != STATUS_OK) response.status = admitted;
    else if (authorized != STATUS_OK) response.status = authorized;
    else if (!interface || !minimum || !maximum || !features || (!interface->empty() && *interface != interface_)
        || *minimum > version_ || *maximum < version_ || (*features & features_) != *features) response.status = STATUS_BAD_ARGS;
    else {
        const auto facet = request.badge / 2;
        // +2 keeps all business badges nonzero; connector uses the odd value.
        const auto cap = sys::channel_mint(channel_.ends[1].selector(), rt_startup().cspace,
            facet * 2 + 2, RIGHT_SEND | RIGHT_DUPLICATE);
        if (cap.status != STATUS_OK) response.status = cap.status;
        else {
            response.data.put("object", describe(facet));
            response.data.cap("endpoint", *response.give(cap_owner{cap.value}));
        }
    }
    return request.respond(std::move(response));
}
auto ubus_object::poll() -> status_t {
    ubus_request request;
    const auto status = input_.receive(request);
    if (status == STATUS_WOULD_BLOCK && budget_) {
        const auto state = sys::source_info(budget_.selector());
        if (state.status == STATUS_BUSY || state.status == STATUS_RETRY) return STATUS_WOULD_BLOCK;
        if (state.status != STATUS_OK) return STATUS_CLOSED;
        if (!(state.value & STATE_OPEN)) return STATUS_CLOSED;
    }
    if (status == STATUS_WOULD_BLOCK) return status;
    if (status != STATUS_OK) {
        if (request.reply) { ubus_msg error; error.status = status; (void)request.respond(std::move(error)); }
        return status == STATUS_PEER_FAULT || status == STATUS_BAD_ARGS || status == STATUS_BAD_RIGHTS || status == STATUS_NO_MEMORY ? STATUS_OK : status;
    }
    if ((request.badge & 1) && request.tag == 1) return request.reply ? bind(request) : STATUS_OK;
    ubus_msg response;
    response.status = rt_admit(request.deadline);
    const auto facet = request.badge >= 2 ? (request.badge - 2) / 2 : 0;
    if (!request.badge || (request.badge & 1)) response.status = STATUS_BAD_RIGHTS;
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
    ubus_msg request; request.tag = 1; request.deadline = deadline;
    request.data.put("interface", interface).put("minimum", minimum).put("maximum", maximum).put("features", features);
    const auto status = call_.begin(connector, std::move(request));
    if (status == STATUS_OK) binding_.emplace(std::string{interface}, minimum, maximum, features);
    return status;
}
auto ubus_conn::bound(ubus_msg& response) -> status_t {
    auto pending = std::move(*binding_); binding_.reset();
    if (response.status != STATUS_OK) return response.status;
    return install(response.take(response.data["endpoint"]), response.data["object"], pending.interface,
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
auto ubus_conn::install(cap_owner endpoint, ubus_value object, std::string_view interface,
                        uint64_t minimum, uint64_t maximum, uint64_t features) -> status_t {
    ubus_data description;
    const auto version = object["version"].as<uint64_t>();
    const auto actual = object["interface"].as<std::string_view>();
    const auto supported = object["features"].as<uint64_t>();
    if (!endpoint || !version || !actual || !supported || *version < minimum || *version > maximum
        || (!interface.empty() && *actual != interface) || (*supported & features) != features
        || object["methods"].type() != ubus_type::Array || !description.assign(object.bytes())) return STATUS_PEER_FAULT;
    ev_watch peer;
    const auto watched = peer.open(rt_startup().events, endpoint.selector(), SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
    if (watched != STATUS_OK) return watched;
    peer_ = std::move(peer); endpoint_ = std::move(endpoint); description_ = std::move(description); return STATUS_OK;
}
auto ubus_conn::adopt(cap_owner endpoint, std::string_view interface, uint64_t minimum,
                      uint64_t maximum, uint64_t features) -> status_t {
    ubus_msg response;
    auto status = call_.begin(endpoint.selector(), {});
    if (status == STATUS_OK) status = call_.wait(response);
    if (status == STATUS_OK) status = response.status;
    return status == STATUS_OK ? install(std::move(endpoint), response.data.view(), interface, minimum, maximum, features) : status;
}
auto ubus_conn::state() const -> status_t {
    if (!endpoint_) return STATUS_CLOSED;
    const auto source = sys::source_info(endpoint(), SOURCE_WRITE);
    return source.status != STATUS_OK ? source.status : source.value & SOURCE_TERMINAL ? STATUS_PEER_CLOSED : STATUS_OK;
}
auto ubus_conn::close() -> status_t {
    binding_.reset();
    const auto status = call_.cancel();
    if (status != STATUS_OK) return status;
    const auto unwatch = peer_.close();
    if (unwatch != STATUS_OK) return unwatch;
    description_ = {};
    return endpoint_.close();
}
auto ubus_conn::release() -> std::expected<cap_owner, status_t> {
    if (!endpoint_) return std::unexpected(STATUS_CLOSED);
    if (active()) return std::unexpected(STATUS_BUSY);
    const auto status = peer_.close();
    if (status != STATUS_OK) return std::unexpected(status);
    description_ = {};
    return std::move(endpoint_);
}
auto ubus_conn::token(std::string_view name) const -> std::optional<word_t> {
    for (auto method : description_["methods"].values())
        if (method["name"].as<std::string_view>() == name) return method["token"].as<word_t>();
    return {};
}
auto ubus_conn::begin(std::string_view name, ubus_msg request) -> status_t {
    const auto method = token(name);
    if (!method) return STATUS_INVALID_OP;
    // The native tag carries the method; no duplicate request sequence exists.
    request.tag = *method;
    return call_.begin(endpoint(), std::move(request));
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
