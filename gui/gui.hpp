#pragma once
#include <gui/protocol.hpp>
#include <ubus/ubus.hpp>
#include <vm.hpp>

struct gui_buffer {
    _vm_area pixels;
    uint32_t id{}, width{}, height{}, stride{}, format{gui_xrgb};
    uint64_t commit{};
    bool busy{};
};
// No automatic thread or renderer. One owner pumps the ordered event stream.
class gui_client {
    ubus_conn session_;
    _ch_pair events_;
    ubus_rx input_;
    uint64_t next_{};
public:
    auto open(cap_t session) -> status_t;
    auto close() -> status_t;
    auto create(std::string_view title, uint32_t width, uint32_t height, uint32_t flags = gui_resizable, gui_limits = {}) -> std::expected<gui_event, status_t>;
    // Raster size is independent of the window's configured logical size.
    auto allocate(uint64_t window, uint32_t width, uint32_t height, uint32_t format = gui_xrgb) -> std::expected<gui_buffer, status_t>;
    auto destroy(uint64_t window, gui_buffer&) -> status_t;
    auto commit(uint64_t window, uint64_t serial, gui_buffer&, gui_rect damage = {}, bool frame = true) -> status_t;
    auto configure(uint64_t window, std::optional<std::string_view> title = {}, std::optional<bool> fullscreen = {}) -> status_t;
    auto remove(uint64_t window) -> status_t;
    auto poll(gui_event&) -> status_t;
    auto endpoint() const -> cap_t { return session_.endpoint(); }
};
