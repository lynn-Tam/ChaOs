#pragma once

#include <libk/string_view.hpp>

namespace arch::console {

void write(char character) noexcept;
void write(libk::StrView text) noexcept;

} // namespace arch::console
