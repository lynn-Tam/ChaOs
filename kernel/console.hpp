#pragma once

#include <base/types.hpp>
#include <cpu.hpp>
#include <libk/fmt.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <sync.hpp>
#include <utility>

namespace console {

// Raw allocation-free formatting shared by ordinary and emergency output.
class Sink final {
public:
    auto write(char c) noexcept -> bool { arch::putchar(c); return true; }
    auto write(const char* p, usize n) noexcept -> bool {
        for (usize i = 0; i < n; ++i) arch::putchar(p[i]);
        return true;
    }
};

template<libk::fmt::fixed_string Format, typename... Args>
void raw(Args&&... arguments) noexcept {
    Sink sink{};
    if (!libk::fmt::format_to<Format>(sink, std::forward<Args>(arguments)...))
        sink.write("<format error>\n", 15);
}

// A leaf lock: firmware output acquires no kernel locks. Mask IRQs so the
// current CPU cannot reenter it. Panic uses raw after claiming its sole writer,
// since a stopped peer may retain this lock.
inline constinit libk::TicketSpinLock mutex;
template<libk::fmt::fixed_string Format, typename... Args>
void print(Args&&... arguments) noexcept {
    sync::Lock guard{mutex};
    raw<Format>(std::forward<Args>(arguments)...);
}

} // namespace console
