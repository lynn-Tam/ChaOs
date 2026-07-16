#pragma once

#include <core/types.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <mm/vspace.hpp>

namespace kernel::mm {

// Bounded executor index for VSpace continuations. VSpace pending state is the
// work truth; this queue only says that the state is currently actionable.
class VSpaceExecutor final : private libk::noncopyable_nonmovable {
    using Queue = libk::IntrusiveList<VSpace, &VSpace::work_hook_>;

public:
    using Notifier = libk::delegate<void() noexcept>;

    VSpaceExecutor() noexcept = default;
    ~VSpaceExecutor() noexcept;

    void submit(VSpace& space) noexcept;
    [[nodiscard]] auto run(VmContext context, usize budget) noexcept -> bool;
    [[nodiscard]] auto pending() const noexcept -> bool;

    void bind_notifier(Notifier notifier) noexcept;
    void unbind_notifier() noexcept;

private:
    [[nodiscard]] auto take() noexcept -> VSpace*;

    mutable libk::TicketSpinLock lock_{};
    Queue queue_{};
    Notifier notifier_{};
};

} // namespace kernel::mm
