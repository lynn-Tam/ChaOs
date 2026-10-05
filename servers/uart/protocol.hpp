#pragma once

#include <stdint.h>

namespace myos::console {

// The UART queue alone can place a prompt after output from every writer.
enum class Operation : uint64_t { Bytes, Prompt };

} // namespace myos::console
