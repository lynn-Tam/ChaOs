#pragma once

#include <stdint.h>

namespace sys::console {

// The UART queue alone can place a prompt after output from every writer.
enum class Operation : uint64_t { Bytes, Prompt };

} // namespace sys::console
