#pragma once

#include <core/types.hpp>
#include <libk/string_view.hpp>
#include <libk/fmt.hpp>
#include <libk/utility.hpp>

namespace kernel::diag::console {

// Normal runtime console sink. Panic output deliberately uses PanicSink and
// never enters this path because normal serialization state may be compromised.
class Sink final {
public:
    auto write(char character) noexcept -> bool;
    auto write(const char* text, usize size) noexcept -> bool;
};

void write(libk::StrView text) noexcept;

template<libk::fmt::fixed_string Format, typename... Args>
void print(Args&&... arguments) noexcept {
    Sink sink{};
    static_cast<void>(libk::fmt::format_to<Format>(
        sink, libk::forward<Args>(arguments)...));
}

} // namespace kernel::diag::console
