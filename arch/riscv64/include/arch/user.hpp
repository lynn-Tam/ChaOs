#pragma once

#include <core/types.hpp>
#include <libk/optional.hpp>
#include <mm/addr.hpp>
#include <uapi/vproc.h>

namespace arch {

struct UserStart final {
    kernel::mm::VirtAddr entry{};
    kernel::mm::VirtAddr stack{};
    usize arguments[6]{};
};

[[nodiscard]] auto valid_user_start(UserStart start) noexcept -> bool;
[[nodiscard]] auto valid_user_context(
    const myos_user_context& context) noexcept -> bool;

// Constructs the synthetic first TrapFrame at the top of the Thread-owned
// kernel stack. The returned address is the remaining kernel stack top.
[[nodiscard]] auto prepare_user_stack(
    usize home_stack_top,
    UserStart start) noexcept -> libk::optional<usize>;

// Restores the synthetic frame at home_stack_top and enters U-mode. Later
// traps use the same assembly restore path with a real frame in the same stack.
[[noreturn]] void resume_user(usize home_stack_top) noexcept;

} // namespace arch
