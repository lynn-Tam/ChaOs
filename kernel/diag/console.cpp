#include <diag/console.hpp>

#include <arch/console.hpp>

namespace kernel::diag::console {

auto Sink::write(char character) noexcept -> bool {
    arch::console::write(character);
    return true;
}

auto Sink::write(const char* text, usize size) noexcept -> bool {
    arch::console::write(libk::StrView{text, size});
    return true;
}

void write(libk::StrView text) noexcept {
    arch::console::write(text);
}

} // namespace kernel::diag::console
