#pragma once

#include <base/types.hpp>
#include <cpu.hpp>
#include <libk/fmt.hpp>
#include <utility>

namespace console {

// A raw, allocation-free sink shared by ordinary and emergency output.
// Serialization, if needed, belongs to the caller.
class Sink final {
public:
    auto write(char c) noexcept -> bool { arch::putchar(c); return true; }
    auto write(const char* p, usize n) noexcept -> bool {
        for (usize i = 0; i < n; ++i) arch::putchar(p[i]);
        return true;
    }
};

template<libk::fmt::fixed_string Format, typename... Args>
void print(Args&&... arguments) noexcept {
    Sink sink{};
    if (!libk::fmt::format_to<Format>(sink, std::forward<Args>(arguments)...))
        sink.write("<format error>\n", 15);
}

} // namespace console
