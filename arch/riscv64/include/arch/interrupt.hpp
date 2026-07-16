#pragma once

namespace arch {

// Captures only the local supervisor interrupt-enable bit. Restoring this
// token never overwrites unrelated sstatus state changed in the guarded scope.
class InterruptState final {
public:
    [[nodiscard]] constexpr auto enabled() const noexcept -> bool {
        return enabled_;
    }

private:
    friend auto disable_interrupts() noexcept -> InterruptState;
    friend void restore_interrupts(InterruptState) noexcept;

    explicit constexpr InterruptState(bool enabled) noexcept
        : enabled_(enabled) {}

    bool enabled_{};
};

[[nodiscard]] auto disable_interrupts() noexcept -> InterruptState;
void enable_interrupts() noexcept;
void restore_interrupts(InterruptState state) noexcept;

} // namespace arch
