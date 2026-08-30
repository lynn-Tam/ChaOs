#pragma once

#include <core/types.hpp>
#include <libk/noncopyable.hpp>
#include <sync/lock.hpp>
#include <uapi/status.h>

namespace kernel::fault {

enum class Reason : u8 {
    NormalExit,
    // A userspace caller explicitly terminated with a non-OK status. Keep
    // this distinct from a kernel fault, external Stop, and invariant
    // failure so supervisors can preserve the caller's status without
    // manufacturing a fault event.
    ExitFailure,
    Fault,
    Stop,
    Revoked,
    PoolClose,
    InvariantFailure,
};

enum class State : u8 {
    Live,
    Exiting,
    Published,
};

struct Snapshot final {
    u64 sequence{};
    Reason reason{Reason::NormalExit};
    myos_status_t status{};
    usize detail{};
    usize pc{};
    usize address{};
    u8 locus{};
};

// Target-owned single-winner terminal record.  Observers read this value;
// they never submit lifecycle transitions through the observation path.
class TerminalRecord final {
public:
    // Two fixed relation slots keep the target object within its one-page
    // ObjectPool slot while allowing an independent supervisor and debugger
    // observer.  A third observer must be an explicitly sponsored extension,
    // not an implicit resize of every Thread/Vproc object.
    static constexpr usize observer_capacity = 2;

    class Observer final : private libk::noncopyable_nonmovable {
    public:
        using Notify = void (*)(void*) noexcept;

        Observer(void* context, Notify notify) noexcept
            : context_(context), notify_(notify) {}

    private:
        friend class TerminalRecord;
        TerminalRecord* owner_{};
        void* context_{};
        Notify notify_{};
    };

    TerminalRecord() noexcept = default;
    TerminalRecord(const TerminalRecord&) = delete;
    auto operator=(const TerminalRecord&) -> TerminalRecord& = delete;

    [[nodiscard]] auto state() const noexcept -> State;
    [[nodiscard]] auto snapshot() const noexcept -> Snapshot;
    [[nodiscard]] auto claim(
        Reason reason,
        myos_status_t status,
        usize detail = 0,
        usize pc = 0,
        usize address = 0,
        u8 locus = 0) noexcept -> bool;
    [[nodiscard]] auto published() const noexcept -> bool;
    [[nodiscard]] auto attach(Observer& observer) noexcept -> bool;
    void detach(Observer& observer) noexcept;

private:
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Terminal>
        lock_{};
    State state_{State::Live};
    Snapshot value_{};
    Observer* observers_[observer_capacity]{};
};

} // namespace kernel::fault
