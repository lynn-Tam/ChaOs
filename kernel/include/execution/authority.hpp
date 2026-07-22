#pragma once

#include <cap/grant.hpp>
#include <cap/resolved.hpp>
#include <execution/stop.hpp>
#include <libk/noncopyable.hpp>
#include <sync/lock.hpp>
#include <libk/variant.hpp>

namespace kernel {

class Thread;
class Vproc;

namespace mm {
class MemoryObject;
class VSpace;
}

namespace execution {

// Persistent capability authority for one user execution lane.  Resolving a
// capability authorizes construction only momentarily; these attachments keep
// the roots and registered runtime backing authorized for every later dispatch.
// Revoking any source stops the lane before releasing the complete relation.
class Authority final : private libk::noncopyable_nonmovable {
    struct Link final : private libk::noncopyable_nonmovable {
        Link(Authority& owner, const cap::GrantAttachmentOps& ops) noexcept
            : owner(&owner), attachment(this, ops) {}

        Authority* owner{};
        cap::GrantAttachment attachment;
        cap::GrantWork work{};
    };

public:
    explicit Authority(Thread& thread) noexcept;
    explicit Authority(Vproc& vproc) noexcept;
    ~Authority() noexcept;

    [[nodiscard]] auto attach(
        const cap::Resolved<kernel::mm::VSpace>& vspace,
        const cap::Resolved<cap::CSpace>& cspace) noexcept
        -> libk::Expected<void, cap::GrantError>;
    [[nodiscard]] auto attach_runtime(
        const cap::Resolved<kernel::mm::MemoryObject>& control,
        const cap::Resolved<kernel::mm::MemoryObject>& events) noexcept
        -> libk::Expected<void, cap::GrantError>;
    [[nodiscard]] auto attach_arm(
        const cap::Resolved<kernel::mm::MemoryObject>& code,
        const cap::Resolved<kernel::mm::MemoryObject>& stack) noexcept
        -> libk::Expected<void, cap::GrantError>;
    void detach_arm() noexcept;
    [[nodiscard]] auto active() const noexcept -> bool;

    // Called only after the dispatcher has removed the target's scheduling
    // relation and no CPU can observe its ExecutionBinding.
    void target_stopped() noexcept;

private:
    static void invalidate(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void released(void* context) noexcept;

    void invalidate(Link& link, cap::GrantWork&& work) noexcept;
    void stopped() noexcept;
    void reset() noexcept;
    void drain(Link& link) noexcept;
    void start_stop() noexcept;

    using Target = libk::variant<Thread*, Vproc*>;

    static const cap::GrantAttachmentOps ops_;

    Target target_;
    mutable kernel::sync::SpinLock<
        kernel::sync::LockClass::ExecutionAuthority> lock_{};
    Link vspace_;
    Link cspace_;
    Link control_;
    Link events_;
    Link code_;
    Link stack_;
    Stop stop_;
    bool arm_attaching_{};
    bool start_armed_{};
    bool ended_{};
};

} // namespace execution
} // namespace kernel
