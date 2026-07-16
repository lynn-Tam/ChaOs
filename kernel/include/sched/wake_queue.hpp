#pragma once

#include <cpu/ipi_delivery.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <sched/binding.hpp>

namespace kernel::sched {

// Target-CPU-owned remote request mailbox. Remote producers append under the
// queue lock; only the target CPU consumes. FIFO order is diagnostic rather
// than scheduling policy. Binding::wake_hook_ is unique membership. The list
// is canonical request state; IpiDelivery only owns retained edge delivery.
class WakeQueue final : private libk::noncopyable_nonmovable {
    using Queue = libk::IntrusiveList<Binding, &Binding::wake_hook_>;

public:
    struct PostResult final {
        bool accepted{};
    };

    [[nodiscard]] auto post(Binding& binding) noexcept -> PostResult;
    [[nodiscard]] auto claim_transport() noexcept
        -> libk::optional<kernel::IpiDelivery::Token>;
    void transport_failed(kernel::IpiDelivery::Token token) noexcept;
    [[nodiscard]] auto take() noexcept -> Binding*;
    void complete(Binding& binding) noexcept;
    [[nodiscard]] auto size() const noexcept -> usize;

private:
    mutable libk::TicketSpinLock lock_{};
    Queue queue_{};
    kernel::IpiDelivery delivery_{};
};

} // namespace kernel::sched
