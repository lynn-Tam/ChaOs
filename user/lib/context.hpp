#pragma once

#include <uapi/types.h>

namespace myos {

using UserEntry = void (*)(myos_word_t, myos_word_t) noexcept;

// Leaves the runtime stack and starts a user-managed continuation on the
// supplied stack.  The selected architecture provides the narrow register
// transition; a returning continuation exits the current execution target.
extern "C" [[noreturn]] void user_enter(
    UserEntry entry,
    myos_word_t stack,
    myos_word_t arg0,
    myos_word_t arg1) noexcept;

} // namespace myos
