#pragma once

#include <base/types.hpp>
#include <arch/console.hpp>
#include <libk/string_view.hpp>
#include <libk/fmt.hpp>
#include <utility>

namespace console {

// A raw, allocation-free sink shared by ordinary and emergency output.
// Serialization, if needed, belongs to the caller.
class Sink final {
public:
    auto write(char c) noexcept -> bool { arch::console::write(c); return true; }
    auto write(const char* p, usize n) noexcept -> bool {
        arch::console::write(libk::StrView{p, n}); return true;
    }
};

template<libk::fmt::fixed_string Format, typename... Args>
void print(Args&&... arguments) noexcept {
    Sink sink{};
    if (!libk::fmt::format_to<Format>(sink, std::forward<Args>(arguments)...))
        arch::console::write(libk::StrView{"<format error>\n"});
}

} // namespace console
