#include <gui/gui.hpp>

auto gui_client::open(cap_t session) -> status_t {
    auto s = close(); if (s != STATUS_OK) return s;
    s = session_.bind(session, "gui.session", 1, 1, gui_scaled_buffers);
    if (s == STATUS_OK) s = events_.open(_startup().alloc(), 64, 0);
    if (s == STATUS_OK) s = input_.open(events_.ends[0].selector(), &_events());
    if (s != STATUS_OK) return s;
    const auto sender = _channel_mint(events_.ends[1].selector(), _startup().cspace, 1, RIGHT_SEND);
    if (sender.status != STATUS_OK) return sender.status;
    ubus_msg q, r; auto idx = q.give(_cap_owner{sender.value}); if (!idx) return idx.error();
    q.data.cap("events", *idx); s = session_.invoke("Connect", std::move(q), r);
    if (s == STATUS_OK) s = events_.ends[1].close();
    return s;
}
auto gui_client::close() -> status_t {
    auto s = input_.close(); if (s != STATUS_OK) return s;
    // Event receiver lifetime terminates the session, even if its controller lives.
    s = events_.close(); if (s != STATUS_OK) return s;
    next_ = 0; return session_.close();
}
auto gui_client::create(std::string_view title, uint32_t width, uint32_t height, uint32_t flags, gui_limits limits) -> std::expected<gui_event, status_t> {
    ubus_msg q, r; q.data.put("title", title).put("width", uint64_t{width}).put("height", uint64_t{height}).put("flags", uint64_t{flags});
    const std::array<uint32_t, 4> bounds{limits.min_width, limits.min_height, limits.max_width, limits.max_height};
    q.data.array("limits", std::span{bounds});
    const auto s = session_.invoke("Create", std::move(q), r); if (s != STATUS_OK) return std::unexpected(s);
    const auto bytes = r.data["configure"].as<std::span<const std::byte>>();
    if (!bytes || bytes->size() != sizeof(gui_event)) return std::unexpected(STATUS_PEER_FAULT);
    gui_event e; ::memcpy(&e, bytes->data(), sizeof(e)); return e;
}
auto gui_client::allocate(uint64_t window, uint32_t width, uint32_t height, uint32_t format) -> std::expected<gui_buffer, status_t> {
    ubus_msg q, r; q.data.put("window", window).put("width", uint64_t{width}).put("height", uint64_t{height}).put("format", uint64_t{format});
    auto s = session_.invoke("Allocate", std::move(q), r); if (s != STATUS_OK) return std::unexpected(s);
    const auto b = r.data["pixels"].as<ubus_buffer>();
    const auto number = r.data["id"].as<uint32_t>(), stride = r.data["stride"].as<uint32_t>();
    const auto size = gui_bytes(width, height);
    if (!b || b->offset || !number || !stride || *stride != width * 4 || !size || b->size != *size) return std::unexpected(STATUS_PEER_FAULT);
    auto map = _vm_area::map(_vm(), r.take(r.data["pixels"]), 0, b->size, VM_READ | VM_WRITE);
    if (!map) return std::unexpected(map.error());
    return gui_buffer{std::move(*map), *number, width, height, *stride, format};
}
auto gui_client::destroy(uint64_t window, gui_buffer& b) -> status_t {
    if (b.busy) return STATUS_BUSY;
    ubus_msg q, r; q.data.put("window", window).put("buffer", uint64_t{b.id});
    auto s = session_.invoke("Destroy", std::move(q), r);
    if (s == STATUS_OK) { s = b.pixels.close(); b.id = 0; } return s;
}
auto gui_client::commit(uint64_t window, uint64_t serial, gui_buffer& b, gui_rect damage, bool frame) -> status_t {
    if (b.busy || next_ == UINT64_MAX) return STATUS_BUSY;
    if (damage.empty()) damage = {0, 0, int32_t(b.width), int32_t(b.height)};
    const std::array<uint64_t, 9> args{window, serial, b.id, ++next_, uint64_t(damage.x), uint64_t(damage.y), uint64_t(damage.width), uint64_t(damage.height), frame};
    ubus_msg q, r; q.data.array("args", std::span{args});
    b.busy = true; b.commit = next_;
    const auto s = session_.invoke("Commit", std::move(q), r);
    if (s != STATUS_OK) {
        // Only an explicit service rejection proves the lease was not taken.
        if (r.status == s && s != STATUS_OK) b.busy = false;
        else (void)close();
    }
    return s;
}
auto gui_client::configure(uint64_t window, std::optional<std::string_view> title, std::optional<bool> fullscreen) -> status_t {
    ubus_msg q, r; q.data.put("window", window);
    if (title) q.data.put("title", *title);
    if (fullscreen) q.data.put("fullscreen", *fullscreen);
    return session_.invoke("Configure", std::move(q), r);
}
auto gui_client::remove(uint64_t window) -> status_t {
    ubus_msg q, r; q.data.put("window", window); return session_.invoke("Remove", std::move(q), r);
}
auto gui_client::poll(gui_event& e) -> status_t {
    ubus_request q; const auto s = input_.receive(q).status; if (s != STATUS_OK) return s;
    const auto bytes = q.data["event"].as<std::span<const std::byte>>();
    if (q.reply || q.count || !bytes || bytes->size() != sizeof(e)) return STATUS_PEER_FAULT;
    ::memcpy(&e, bytes->data(), sizeof(e));
    return STATUS_OK;
}
