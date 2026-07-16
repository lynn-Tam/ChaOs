#pragma once

#include <libk/string_view.hpp>

namespace platform::console {

void write(char character) noexcept;
void write(libk::StrView text) noexcept;

} // namespace platform::console
