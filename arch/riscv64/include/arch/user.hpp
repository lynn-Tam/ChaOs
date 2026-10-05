#pragma once

#include <base/types.hpp>
#include <optional>
#include <mm/types.hpp>

namespace arch {

class UserFrame;

struct UserStart final {
    mm::Virt entry{};
    mm::Virt stack{};
    usize arguments[6]{};
};

[[nodiscard]] auto valid_user_start(UserStart start) noexcept -> bool;
// Constructs the synthetic first TrapFrame at the top of the Thread-owned
// kernel stack. The returned address is the remaining kernel stack top.
[[nodiscard]] auto prepare_user_stack(
    usize home_stack_top,
    UserStart start) noexcept -> std::optional<usize>;

// Constructs a return frame and exposes only its architecture-neutral token.
// Unlike prepare_user_stack(), this is used by cross-domain activations whose
// trap return must move to a different kernel stack.
[[nodiscard]] auto prepare_user_frame(
    usize kernel_stack_top,
    UserStart start) noexcept -> std::optional<UserFrame>;

// Restores the synthetic frame at home_stack_top and enters U-mode. Later
// traps use the same assembly restore path with a real frame in the same stack.
[[noreturn]] void resume_user(usize home_stack_top) noexcept;

} // namespace arch
