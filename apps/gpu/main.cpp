#include <virtio.hpp>
#include <gui/display.hpp>
#include <deque>

struct gpu_header { uint32_t type{}, flags{1}; uint64_t fence{}; uint32_t ctx{}, ring{}; };
struct gpu_rect { uint32_t x{}, y{}, width{}, height{}; };
struct gpu_cmd { gpu_header head; gpu_rect rect; uint64_t offset{}; uint32_t resource{}, padding{}; };
static vi_dev dev;
static vi_queue control, cursor;
static vi_dma commands, cursor_pixels;
static ubus_object directory{"io.display"};
static ubus_conn parent;
static std::unique_ptr<ubus_object> client;
static _cap_owner events;
static _ev_watch event_watch;
static _cap_desc desc;
static _cap_owner shared;
struct gpu_buffer { vi_dma dma; uint32_t resource{}; bool leased{}; uint64_t sequence{}; };
static std::array<gpu_buffer, 2> buffers;
static std::optional<ubus_request> opening, closing;
static std::deque<dp_event> notices;
static uint32_t width, height, output, resource_id, candidate, building;
static int current = -1;
static uint64_t fence, generation, last_sequence, command_due;
static gpu_rect damage;
static bool stopping, cursor_ready, cursor_dirty;
static uint32_t cursor_x, cursor_y;
static uint64_t cursor_fence;
enum class gpu_step { Info, CursorCreate, CursorAttach, CursorTransfer, Idle, Create, Attach, Transfer, Scanout, Flush, Disable, Detach, Unref };
static gpu_step step = gpu_step::Info;
static bool outstanding;
static status_t last_error;
static uint32_t expected, opcode;
static uint64_t commands_done;
static uint64_t command_start, transfer_ticks, flush_ticks, transferred_bytes, frames_done;

static auto issue(const void* bytes, size_t size, uint32_t response = 0x1100) -> status_t {
    if (outstanding || size < sizeof(gpu_header) || size > 512 || fence == UINT64_MAX) return STATUS_INTERNAL;
    auto* p = reinterpret_cast<std::byte*>(commands.memory.address);
    opcode = static_cast<const gpu_header*>(bytes)->type;
    ::memcpy(p, bytes, size);
    reinterpret_cast<gpu_header*>(p)->fence = ++fence;
    ::memset(p + 512, 0, 512);
    const std::array<vi_queue::part, 2> parts{{{commands.address, uint32_t(size), false}, {commands.address + 512, 512, true}}};
    auto s = control.submit(fence, parts);
    if (s == STATUS_OK) { expected = response; outstanding = true; command_start = _clock_now().value; command_due = command_start + _startup().frequency * 2; control.publish(); }
    return s;
}
static auto command(uint32_t opcode, uint32_t resource = 0, gpu_rect rect = {}) -> status_t {
    gpu_cmd cmd{{opcode}, rect, 0, resource};
    if (opcode == 0x103) { // set_scanout has no offset
        struct { gpu_header h; gpu_rect r; uint32_t scanout, resource; } c{{opcode}, rect, output, resource};
        return issue(&c, sizeof(c));
    }
    if (opcode == 0x104) {
        struct { gpu_header h; gpu_rect r; uint32_t resource, padding; } c{{opcode}, rect, resource, 0};
        return issue(&c, sizeof(c));
    }
    if (opcode == 0x102 || opcode == 0x107) {
        struct { gpu_header h; uint32_t resource, padding; } c{{opcode}, resource, 0};
        return issue(&c, sizeof(c));
    }
    if (opcode == 0x105) cmd.offset = (uint64_t{rect.y} * (resource == 1 ? 64 : width) + rect.x) * 4;
    return issue(&cmd, sizeof(cmd));
}
static auto create(uint32_t resource, uint32_t w, uint32_t h) -> status_t {
    struct { gpu_header h; uint32_t resource, format, width, height; } cmd{{0x101}, resource, resource == 1 ? 1U : 2U, w, h};
    return issue(&cmd, sizeof(cmd));
}
static auto attach(uint32_t resource, const vi_dma& dma) -> status_t {
    struct { gpu_header h; uint32_t resource, count; uint64_t address; uint32_t length, padding; } cmd{{0x106}, resource, 1, dma.address, uint32_t(dma.memory.size), 0};
    return issue(&cmd, sizeof(cmd));
}
static void notify(uint32_t type, uint32_t id, uint64_t seq, status_t status = STATUS_OK) {
    if (!events) return;
    if (notices.size() == 16) _thread_exit(STATUS_INTERNAL);
    notices.push_back({generation, seq, _clock_now().value, type, id, status, 0});
}
static auto finish_open() -> status_t {
    ubus_msg response;
    ubus_data pool{ubus_type::Array};
    for (size_t n = 0; n < buffers.size(); ++n) {
        auto& b = buffers[n];
        auto grant = desc.derive(b.dma.memory.memory.selector(), _cap_mem(RIGHT_MAP | RIGHT_DUPLICATE, 0, b.dma.memory.size / 4096, VM_READ | VM_WRITE));
        if (!grant) return grant.error();
        const auto index = response.give(std::move(*grant)); if (!index) return index.error();
        ubus_data row; row.put("id", uint64_t(n)).put("pixels", ubus_buffer{*index, 0, b.dma.memory.size}); pool.push(row);
    }
    auto handle = client->handle(); if (!handle) return handle.error();
    auto index = response.give(std::move(*handle)); if (!index) return index.error();
    response.data.cap("handle", *index).put("buffers", pool).put("width", uint64_t{width}).put("height", uint64_t{height})
        .put("stride", uint64_t{width} * 4).put("generation", generation).put("output", uint64_t{output});
    const auto status = opening->respond(std::move(response)); opening.reset();
    if (status != STATUS_OK) { stopping = true; last_error = status; }
    return STATUS_OK;
}
static auto advance() -> status_t {
    vi_queue::completion c;
    auto status = control.take(c);
    if (status != STATUS_OK) return status;
    const auto* h = reinterpret_cast<const gpu_header*>(commands.memory.address + 512);
    if (!outstanding || c.cookie != fence || c.size < sizeof(gpu_header) || h->fence != fence || !(h->flags & 1) || h->type != expected) return STATUS_BACKING_FAILED;
    outstanding = false; ++commands_done;
    const auto elapsed = _clock_now().value - command_start;
    if (step == gpu_step::Transfer) { transfer_ticks += elapsed; transferred_bytes += uint64_t(damage.width) * damage.height * 4; }
    if (step == gpu_step::Flush) { flush_ticks += elapsed; ++frames_done; }
    switch (step) {
    case gpu_step::Info: {
        if (c.size != 408) return STATUS_BACKING_FAILED;
        struct info { gpu_rect rect; uint32_t enabled, flags; };
        const auto* modes = reinterpret_cast<const info*>(h + 1);
        for (output = 0; output < 16 && !modes[output].enabled; ++output) {}
        if (output == 16 || !gui_bytes(modes[output].rect.width, modes[output].rect.height)) return STATUS_NOT_FOUND;
        width = modes[output].rect.width; height = modes[output].rect.height;
        auto storage = dev.allocate(64 * 64 * 4); if (!storage) return storage.error();
        cursor_pixels = std::move(*storage);
        auto* pixels = reinterpret_cast<uint32_t*>(cursor_pixels.memory.address);
        for (unsigned y = 0; y < 24; ++y) for (unsigned x = 0; x <= y / 2; ++x)
            pixels[y * 64 + x] = x == 0 || x == y / 2 || y == 23 ? 0xff101820 : 0xfff4f8ff;
        resource_id = 1; step = gpu_step::CursorCreate; return create(1, 64, 64);
    }
    case gpu_step::CursorCreate: step = gpu_step::CursorAttach; return attach(1, cursor_pixels);
    case gpu_step::CursorAttach: step = gpu_step::CursorTransfer; return command(0x105, 1, {0, 0, 64, 64});
    case gpu_step::CursorTransfer: cursor_ready = cursor_dirty = true; step = gpu_step::Idle; break;
    case gpu_step::Create: step = gpu_step::Attach; return attach(buffers[building].resource, buffers[building].dma);
    case gpu_step::Attach:
        if (++building < buffers.size()) { step = gpu_step::Create; return create(buffers[building].resource, width, height); }
        step = gpu_step::Idle; return finish_open();
    case gpu_step::Transfer: step = gpu_step::Scanout; return command(0x103, buffers[candidate].resource, {0, 0, width, height});
    case gpu_step::Scanout: step = gpu_step::Flush; return command(0x104, buffers[candidate].resource, damage);
    case gpu_step::Flush:
        if (current >= 0) { auto& old = buffers[current]; old.leased = false; notify(dp_release, current, old.sequence); }
        current = candidate; notify(dp_frame, candidate, buffers[candidate].sequence); step = gpu_step::Idle; break;
    case gpu_step::Disable: current = -1; building = 0; step = gpu_step::Detach; return command(0x107, buffers[0].resource);
    case gpu_step::Detach: step = gpu_step::Unref; return command(0x102, buffers[building].resource);
    case gpu_step::Unref:
        status = dev.release(buffers[building].dma); if (status != STATUS_OK) return status;
        buffers[building] = {};
        if (++building < buffers.size()) { step = gpu_step::Detach; return command(0x107, buffers[building].resource); }
        if (closing) { (void)closing->respond(); closing.reset(); }
        (void)event_watch.close(); events = {}; notices.clear(); client.reset(); stopping = false; step = gpu_step::Idle; break;
    case gpu_step::Idle: return STATUS_INTERNAL;
    }
    return STATUS_OK;
}
static auto open(ubus_request& q, ubus_msg&) -> status_t {
    if (client || opening) return STATUS_BUSY;
    if (step != gpu_step::Idle) return STATUS_WOULD_BLOCK;
    if (generation == UINT64_MAX || resource_id > UINT32_MAX - buffers.size()) return STATUS_CLOSED;
    const auto bytes = gui_bytes(width, height); if (!bytes || *bytes > UINT32_MAX) return STATUS_NO_MEMORY;
    auto endpoint = q.take(q.data["events"]);
    const auto fact = _source_info(endpoint.selector(), SOURCE_WRITE);
    if (fact.status != STATUS_OK || (fact.value & SOURCE_TERMINAL)) return STATUS_BAD_ARGS;
    auto object = std::make_unique<ubus_object>("io.display.session");
    auto status = object->add("Present", {{"args", ubus_type::Vector}}, {}, [](auto& request, auto&) -> status_t {
        if (stopping || step != gpu_step::Idle || notices.size() > 12) return STATUS_BUSY;
        const auto args = request.data["args"].template vector<uint64_t>();
        if (!args || args->size() != 7) return STATUS_BAD_ARGS;
        const auto a = *args;
        if (a[0] != generation || a[1] >= buffers.size() || !a[2] || a[2] <= last_sequence) return STATUS_STALE;
        if (buffers[a[1]].leased || a[3] > width || a[4] > height || !a[5] || !a[6] || a[5] > width - a[3] || a[6] > height - a[4]) return STATUS_BAD_ARGS;
        candidate = a[1]; last_sequence = a[2]; damage = {uint32_t(a[3]), uint32_t(a[4]), uint32_t(a[5]), uint32_t(a[6])};
        status_t s = command(0x105, buffers[candidate].resource, damage);
        if (s != STATUS_OK) return s;
        buffers[candidate].leased = true; buffers[candidate].sequence = last_sequence; step = gpu_step::Transfer;
        return STATUS_OK;
    });
    if (status == STATUS_OK) status = object->add("Cursor", {{"x", ubus_type::U64}, {"y", ubus_type::U64}}, {}, [](auto& r, auto&) {
        if (stopping) return STATUS_CLOSED;
        cursor_x = std::min<uint64_t>(*r.data["x"].template as<uint64_t>(), width - 1);
        cursor_y = std::min<uint64_t>(*r.data["y"].template as<uint64_t>(), height - 1); cursor_dirty = true; return STATUS_OK;
    });
    if (status == STATUS_OK) status = object->add("Close", {}, {}, [](auto& r, auto&) {
        if (stopping) return STATUS_CLOSED;
        stopping = true; closing.emplace(std::move(r)); return STATUS_PENDING;
    });
    if (status == STATUS_OK) status = object->open(8);
    if (status != STATUS_OK) return status;
    for (auto& b : buffers) {
        auto dma = dev.allocate(*bytes, shared.selector()); if (!dma) {
            for (auto& old : buffers) { auto s = dev.release(old.dma); if (s != STATUS_OK) return s; old = {}; }
            return dma.error();
        }
        b.dma = std::move(*dma); b.resource = ++resource_id;
    }
    status = event_watch.open(&_events(), endpoint.selector(), SOURCE_TERMINAL, 0, WATCH_CHANGED, SOURCE_WRITE);
    if (status != STATUS_OK) {
        for (auto& b : buffers) { const auto released = dev.release(b.dma); if (released != STATUS_OK) return released; b = {}; }
        return status;
    }
    events = std::move(endpoint); client = std::move(object); ++generation; last_sequence = 0;
    opening.emplace(std::move(q)); building = 0; step = gpu_step::Create;
    status = create(buffers[0].resource, width, height);
    if (status != STATUS_OK) _thread_exit(status);
    return STATUS_PENDING;
}
int main() {
    auto status = parent.bind(_startup().bootstrap); if (status != STATUS_OK) return status;
    std::vector<ubus_msg> setup;
    do {
        ubus_msg r; status = parent.invoke("Receive", {}, r); if (status != STATUS_OK) return status;
        ubus_data data; if (!data.assign(r.data["message"].bytes())) return STATUS_BAD_ARGS;
        r.data = std::move(data); setup.push_back(std::move(r));
    } while (setup.back().data["more"].as<bool>().value_or(false));
    for (auto& m : setup) if (m.data["shared"]) shared = m.take(m.data["shared"]);
    if (!shared) return STATUS_BAD_ARGS;
    status = dev.open(setup, 0x1050); if (status != STATUS_OK) return status;
    if ((status = control.open(dev, 0, 64)) != STATUS_OK || (status = cursor.open(dev, 1, 16)) != STATUS_OK) return status;
    auto dma = dev.allocate(4096); if (!dma) return dma.error(); commands = std::move(*dma);
    if ((status = dev.ready()) != STATUS_OK) return status;
    status = directory.add("Open", {{"events", ubus_type::Cap}}, {{"handle", ubus_type::Cap}, {"buffers", ubus_type::Array}, {"width", ubus_type::U64}, {"height", ubus_type::U64}, {"stride", ubus_type::U64}, {"generation", ubus_type::U64}, {"output", ubus_type::U64}}, open);
    if (status == STATUS_OK) status = directory.add("Stat", {}, {{"command", ubus_type::U64}, {"completed", ubus_type::U64}, {"active", ubus_type::Bool}, {"width", ubus_type::U64}, {"height", ubus_type::U64}, {"error", ubus_type::I64}, {"transfer_ticks", ubus_type::U64}, {"flush_ticks", ubus_type::U64}, {"bytes", ubus_type::U64}, {"frames", ubus_type::U64}, {"frequency", ubus_type::U64}, {"irqs", ubus_type::U64}}, [](auto&, auto& r) {
        r.data.put("command", uint64_t(opcode)).put("completed", commands_done).put("active", outstanding).put("width", uint64_t(width)).put("height", uint64_t(height)).put("error", int64_t(last_error))
            .put("transfer_ticks", transfer_ticks).put("flush_ticks", flush_ticks).put("bytes", transferred_bytes).put("frames", frames_done).put("frequency", _startup().frequency).put("irqs", dev.irq_count); return STATUS_OK;
    });
    if (status == STATUS_OK) status = directory.open();
    if (status != STATUS_OK) return status;
    auto connector = directory.connector(); if (!connector) return connector.error();
    ubus_msg pub, r; auto idx = pub.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE); if (!idx) return idx.error();
    pub.data.put("name", "display").put("interface", "io.display").put("version", uint64_t{1}).cap("connector", *idx);
    status = parent.invoke("Publish", std::move(pub), r); if (status != STATUS_OK) return status;
    gpu_header info{0x100}; if ((status = issue(&info, sizeof(info), 0x1101)) != STATUS_OK) return status;
    for (;;) {
        bool again{};
        if ((status = dev.acknowledge()) != STATUS_OK) return status;
        if (outstanding) { status = advance(); if (status == STATUS_OK) again = true; else if (status != STATUS_WOULD_BLOCK) return status; }
        vi_queue::completion done;
        if (cursor.active()) { status = cursor.take(done); if (status == STATUS_OK) again = true; else if (status != STATUS_WOULD_BLOCK) return status; }
        if (cursor_ready && cursor_dirty && !cursor.active()) {
            struct { gpu_header h; uint32_t scanout, x, y, padding, resource, hot_x, hot_y, reserved; } c{{cursor_fence ? 0x301U : 0x300U, 0}, output, cursor_x, cursor_y, 0, 1, 0, 0, 0};
            ::memcpy(reinterpret_cast<void*>(commands.memory.address + 2048), &c, sizeof(c));
            const vi_queue::part p{commands.address + 2048, sizeof(c), false};
            status = cursor.submit(++cursor_fence, {&p, 1}); if (status != STATUS_OK) return status;
            cursor.publish(); cursor_dirty = false; again = true;
        }
        if (events) {
            const auto f = _source_info(events.selector(), SOURCE_WRITE);
            if (f.status != STATUS_OK || (f.value & SOURCE_TERMINAL)) { stopping = true; last_error = f.status == STATUS_OK ? STATUS_PEER_CLOSED : f.status; }
            else {
                while (!notices.empty()) {
                    ubus_msg m; m.data.put("event", std::as_bytes(std::span{&notices.front(), 1}));
                    status = ubus_send(events.selector(), m, false);
                    if (status == STATUS_WOULD_BLOCK) break;
                    if (status != STATUS_OK) { stopping = true; last_error = status; break; }
                    notices.pop_front(); again = true;
                }
                status = event_watch.arm(f.value2).status; if (status != STATUS_OK) { stopping = true; last_error = status; }
            }
        }
        if (stopping && step == gpu_step::Idle && !outstanding) { step = gpu_step::Disable; status = command(0x103); if (status != STATUS_OK) return status; }
        for (size_t n = 0; n < 8; ++n) { status = directory.poll(); if (status == STATUS_WOULD_BLOCK) break; if (status != STATUS_OK) return status; again = true; }
        if (client && !stopping) for (size_t n = 0; n < 8; ++n) {
            status = client->poll(); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) { stopping = true; last_error = status; break; } again = true;
        }
        if (outstanding && _clock_now().value >= command_due) return STATUS_TIMED_OUT;
        if (step == gpu_step::Idle && dev.config_size >= 8 && (vi_read<uint32_t>(dev.config) & 1)) {
            vi_write<uint32_t>(dev.config + 4, 1); return STATUS_STALE;
        }
        if (!again) { status = _ev_wait(&_events(), outstanding ? command_due : 0).status; if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status; }
    }
}
