#include <diag/console.hpp>

#include <platform/console.hpp>

namespace kernel::diag::console {

auto Sink::write(char character) noexcept -> bool {
    platform::console::write(character);
    return true;
}

auto Sink::write(const char* text, usize size) noexcept -> bool {
    platform::console::write(libk::StrView{text, size});
    return true;
}

void write(libk::StrView text) noexcept {
    platform::console::write(text);
}

} // namespace kernel::diag::console
