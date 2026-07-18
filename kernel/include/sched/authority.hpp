#pragma once

#include <cap/grant.hpp>
#include <cap/resolved.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <execution/stop.hpp>
#include <execution/target.hpp>

namespace kernel {
class Thread;
class Vproc;
}

namespace kernel::sched {

class SchedulingContext;

// Operation-owned authority behind one SC->execution binding. Cap handles may
// be closed after bind, but revoking either authorizing Grant stops the target.
// Storage belongs to the stable SchedulingContext and therefore outlives the
// dispatcher-owned Binding that is removed during the stop.
class BindingAuthority final : private libk::noncopyable_nonmovable {
    struct Link final : private libk::noncopyable_nonmovable {
        Link(BindingAuthority& owner, const cap::GrantAttachmentOps& ops) noexcept
            : owner(&owner), attachment(this, ops) {}

        BindingAuthority* owner{};
        cap::GrantAttachment attachment;
        cap::GrantWork work{};
    };

public:
    explicit BindingAuthority(Thread& thread) noexcept;
    explicit BindingAuthority(Vproc& vproc) noexcept;
    ~BindingAuthority() noexcept;

    [[nodiscard]] auto attach(
        const cap::Resolved<SchedulingContext>& context,
        const cap::Resolved<Thread>& thread) noexcept
        -> libk::Expected<void, cap::GrantError>;
    [[nodiscard]] auto attach(
        const cap::Resolved<SchedulingContext>& context,
        const cap::Resolved<Vproc>& vproc) noexcept
        -> libk::Expected<void, cap::GrantError>;
    void release() noexcept;
    [[nodiscard]] auto reusable() const noexcept -> bool;

private:
    static void invalidate(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void released(void* context) noexcept;
    void invalidate(Link& link, cap::GrantWork&& work) noexcept;
    void stopped() noexcept;
    void drain(Link& link) noexcept;
    static const cap::GrantAttachmentOps ops_;

    execution::Target target_{};
    mutable libk::TicketSpinLock lock_{};
    Link context_;
    Link target_cap_;
    execution::Stop stop_;
    bool start_armed_{};
    bool ended_{};
};

} // namespace kernel::sched
