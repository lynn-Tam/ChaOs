#pragma once

#include <core/types.hpp>

namespace arch {

struct TrapContextAccess;

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

    // Current kernel breakpoint support assumes 32-bit ebreak. Supporting
    // c.ebreak requires instruction-length decoding.
    void complete_breakpoint() noexcept;
    void complete_syscall() noexcept;

    [[nodiscard]] auto arg(usize index) const noexcept -> usize;
    void set_result(usize index, usize value) noexcept;
    void set_return(usize value) noexcept { set_result(0, value); }

    [[nodiscard]] auto fault_addr() const noexcept -> usize;
    [[nodiscard]] auto snapshot() const noexcept -> TrapSnapshot;

private:
    friend struct TrapContextAccess;

    explicit TrapContext(void* frame) noexcept;

    void* frame_;
};

[[nodiscard]] auto install_trap() noexcept -> bool;

} // namespace arch
