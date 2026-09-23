#pragma once

#include <cap/grant.hpp>
#include <cap/resolved.hpp>
#include <execution/stop.hpp>
#include <libk/noncopyable.hpp>
#include <libk/variant.hpp>
#include <object/object_cleanup.hpp>
#include <sync/lock.hpp>

namespace kernel {
class Thread;
class Vproc;
namespace mm { class MemoryObject; class VSpace; }

namespace execution {

// Persistent authority for one execution lane. Revocation stops execution;
// retirement additionally waits for every detached authority callback to drain.
class Authority final : private libk::noncopyable_nonmovable {
    enum Root : usize { VSpace, CSpace, Control, Events, Code, Stack, Count };
    struct Link final {
        Link(Authority& owner, const cap::GrantAttachmentOps& ops) noexcept
            : attachment(&owner, ops) {}
        cap::GrantAttachment attachment;
        cap::GrantWork work{};
    };
public:
    explicit Authority(Thread& thread) noexcept;
    explicit Authority(Vproc& vproc) noexcept;
    ~Authority() noexcept;

    [[nodiscard]] auto attach(const cap::Resolved<mm::VSpace>& vspace,
        const cap::Resolved<cap::CSpace>& cspace) noexcept -> libk::Expected<void, cap::GrantError>;
    [[nodiscard]] auto attach_runtime(const cap::Resolved<mm::MemoryObject>& control,
        const cap::Resolved<mm::MemoryObject>& events) noexcept -> libk::Expected<void, cap::GrantError>;
    [[nodiscard]] auto attach_arm(const cap::Resolved<mm::MemoryObject>& code,
        const cap::Resolved<mm::MemoryObject>& stack) noexcept -> libk::Expected<void, cap::GrantError>;
    void detach_arm() noexcept;
    [[nodiscard]] auto active() const noexcept -> bool;
    void target_stopped() noexcept;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

private:
    template<usize Index> static auto ops() noexcept -> const cap::GrantAttachmentOps&;
    template<usize First, class A, class B>
    auto attach_pair(const cap::Resolved<A>& first, const cap::Resolved<B>& second) noexcept
        -> libk::Expected<void, cap::GrantError>;
    void invalidate(usize index, cap::GrantWork&& work) noexcept;
    void released(usize index) noexcept;
    void detach(usize index) noexcept;
    void drain(Link& link) noexcept;
    void start_stop() noexcept;
    void stopped() noexcept;
    void finish_retire() noexcept;

    libk::variant<Thread*, Vproc*> target_;
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::ExecutionAuthority> lock_{};
    Link links_[Count];
    Stop stop_;
    object::ObjectCleanup cleanup_{};
    // A bit owns the relation until detach returns true or released acknowledges
    // its last callback. detached_ elects one detacher per attachment generation.
    u8 pending_{}, detached_{};
    bool attaching_{}, start_armed_{}, ended_{};
};

} // namespace execution
} // namespace kernel
