#pragma once

#include <core/types.hpp>
#include <uapi/vproc.h>

namespace arch {

struct TrapContextAccess;
struct UserStart;

// Opaque identity of a complete user return frame resident on a kernel-owned
// stack.  The kernel may retain and later restore this token, but selected-arch
// code remains the only layer that knows the frame layout.
class UserFrame final {
public:
    UserFrame() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return raw_ != nullptr;
    }

private:
    friend struct TrapContextAccess;
    explicit UserFrame(void* raw) noexcept : raw_(raw) {}

    void* raw_{};
};

struct TrapSnapshot final {
    usize gpr[31]{};
    usize pc{};
    usize status{};
    usize cause{};
    usize fault_address{};
};

// Non-owning view of the return state for one active trap. Kernel trap policy
// may inspect and update it but cannot construct a raw architecture frame.
class TrapContext final {
public:
    TrapContext(const TrapContext&) = delete;
    auto operator=(const TrapContext&) -> TrapContext& = delete;

    [[nodiscard]] auto pc() const noexcept -> usize;
    void set_pc(usize pc) noexcept;

    // Breakpoint completion is currently supervisor-only. User breakpoints
    // remain fault-policy events and are never fetched through a raw user VA.
    void complete_breakpoint() noexcept;
    void complete_syscall() noexcept;

    [[nodiscard]] auto arg(usize index) const noexcept -> usize;
    // The architecture-neutral syscall ABI publishes status plus two bounded
    // result words in a0-a2. Other indices are not writable through this view.
    void set_result(usize index, usize value) noexcept;
    void set_return(usize value) noexcept { set_result(0, value); }

    [[nodiscard]] auto fault_addr() const noexcept -> usize;
    [[nodiscard]] auto snapshot() const noexcept -> TrapSnapshot;
    void save_user(myos_user_context& output) const noexcept;
    [[nodiscard]] auto load_user(
        const myos_user_context& input) noexcept -> bool;
    [[nodiscard]] auto load_user_start(
        const UserStart& start) noexcept -> bool;

    [[nodiscard]] auto frame() const noexcept -> UserFrame;
    void redirect(UserFrame frame) noexcept;

private:
    friend struct TrapContextAccess;

    explicit TrapContext(void* frame) noexcept;

    void* frame_;
};

[[nodiscard]] auto install_trap() noexcept -> bool;

} // namespace arch
