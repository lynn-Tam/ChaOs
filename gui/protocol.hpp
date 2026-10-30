#pragma once
#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>

// Little endian B,G,R,X/A; ARGB pixels contain premultiplied channels.
enum : uint32_t { gui_xrgb = 1, gui_argb = 2 };
inline constexpr uint64_t gui_scaled_buffers = 1;
struct gui_rect {
    int32_t x{}, y{}, width{}, height{};
    auto empty() const -> bool { return width <= 0 || height <= 0; }
};
// Fit a nonempty raster into a logical viewport, preserving square pixels.
inline auto gui_fit(gui_rect r, uint32_t width, uint32_t height) -> gui_rect {
    const auto outer = r;
    if (uint64_t(r.width) * height > uint64_t(r.height) * width) {
        r.width = std::max<uint64_t>(1, uint64_t(r.height) * width / height);
        r.x += (outer.width - r.width) / 2;
    } else {
        r.height = std::max<uint64_t>(1, uint64_t(r.width) * height / width);
        r.y += (outer.height - r.height) / 2;
    }
    return r;
}
inline auto gui_intersect(gui_rect a, gui_rect b) -> gui_rect {
    const auto x = std::max<int64_t>(a.x, b.x), y = std::max<int64_t>(a.y, b.y);
    const auto right = std::min(int64_t{a.x} + a.width, int64_t{b.x} + b.width);
    const auto bottom = std::min(int64_t{a.y} + a.height, int64_t{b.y} + b.height);
    return {int32_t(x), int32_t(y), int32_t(std::max<int64_t>(0, right - x)), int32_t(std::max<int64_t>(0, bottom - y))};
}
inline void gui_damage(gui_rect& a, gui_rect b) {
    if (b.empty()) return;
    if (a.empty()) { a = b; return; }
    const auto x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    a = {x, y, std::max(a.x + a.width, b.x + b.width) - x, std::max(a.y + a.height, b.y + b.height) - y};
}
inline auto gui_bytes(uint64_t width, uint64_t height) -> std::optional<size_t> {
    if (!width || !height || width > INT32_MAX / 4 || height > INT32_MAX || width * 4 > (SIZE_MAX - 4095) / height) return {};
    return (width * height * 4 + 4095) & ~size_t{4095};
}
enum : uint32_t { gui_configure = 1, gui_release, gui_frame, gui_focus, gui_reset, gui_key, gui_pointer, gui_text, gui_close, gui_display };
enum : uint32_t { gui_used = 1, gui_discarded, gui_hidden, gui_failed };
enum : uint32_t { gui_fullscreen = 1, gui_resizable = 2, gui_keep_aspect = 4 };
// Fields are selected by type: serial is configure/commit/seat epoch;
// code is buffer ID/key/button; value is state/result/Unicode codepoint.
struct gui_event {
    uint64_t window{}, serial{}, sequence{}, time{};
    uint32_t type{}, flags{};
    int32_t x{}, y{}, width{}, height{}, code{}, value{};
};
static_assert(sizeof(gui_event) == 64);

struct gui_limits { uint32_t min_width{64}, min_height{32}, max_width{INT32_MAX}, max_height{INT32_MAX}; };
