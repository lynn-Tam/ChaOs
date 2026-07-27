#pragma once

#include <fault/terminal.hpp>
#include <ipc/notification_source.hpp>
#include <libk/noncopyable.hpp>

namespace kernel::ipc {
class Notification;
}

namespace kernel::fault {

// A target-owned observation relation.  The relation only projects the
// immutable terminal record to Notification; query() always reads the target.
class TerminalObservation final : private libk::noncopyable_nonmovable {
public:
    TerminalObservation() noexcept;
    ~TerminalObservation() noexcept;

    [[nodiscard]] auto bind(
        TerminalRecord& target,
        ipc::Notification& notification,
        u64 badge) noexcept -> bool;
    void reset() noexcept;

    [[nodiscard]] auto query() const noexcept -> Snapshot;
    [[nodiscard]] auto generation() const noexcept -> u64 {
        return generation_;
    }

private:
    static void notify(void* context) noexcept;
    void closed() noexcept;

    TerminalRecord* target_{};
    ipc::Notification* notification_{};
    TerminalRecord::Observer observer_;
    ipc::NotificationSource source_;
    u64 generation_{};
};

// Fixed relation storage keeps Thread/Vproc object slots page-sized while
// retaining target-owned lifecycle: the target owns the pointer and releases
// the relation before its slot becomes reusable.
[[nodiscard]] auto allocate_observation() noexcept -> TerminalObservation*;
void release_observation(TerminalObservation& observation) noexcept;

} // namespace kernel::fault
