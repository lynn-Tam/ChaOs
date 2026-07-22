#pragma once

#include <core/types.hpp>
#include <uapi/syscall.h>

namespace kernel::syscall {

enum class Continuation : u8 {
    Invalid,
    Immediate,
    SplitPhase,
    ThreadBlocking,
    LanePark,
    Resume,
    Terminal,
};

enum class TargetMask : u8 {
    None = 0,
    Thread = 1,
    Vproc = 2,
    Both = 3,
};

enum class Locus : u8 {
    ThreadBase,
    EndpointLeaf,
    VprocBase,
    VprocUpcall,
};

enum class LocusMask : u8 {
    None = 0,
    ThreadBase = 1U << static_cast<u8>(Locus::ThreadBase),
    EndpointLeaf = 1U << static_cast<u8>(Locus::EndpointLeaf),
    VprocBase = 1U << static_cast<u8>(Locus::VprocBase),
    VprocUpcall = 1U << static_cast<u8>(Locus::VprocUpcall),
    Thread = static_cast<u8>(ThreadBase) | static_cast<u8>(EndpointLeaf),
    Vproc = static_cast<u8>(VprocBase) | static_cast<u8>(VprocUpcall),
    All = static_cast<u8>(Thread) | static_cast<u8>(Vproc),
};

struct Policy final {
    Continuation continuation{Continuation::Invalid};
    TargetMask targets{TargetMask::None};
    LocusMask loci{LocusMask::None};

    [[nodiscard]] constexpr auto allows_thread() const noexcept -> bool {
        return (static_cast<u8>(targets)
            & static_cast<u8>(TargetMask::Thread)) != 0;
    }
    [[nodiscard]] constexpr auto allows_vproc() const noexcept -> bool {
        return (static_cast<u8>(targets)
            & static_cast<u8>(TargetMask::Vproc)) != 0;
    }
    [[nodiscard]] constexpr auto allows(Locus locus) const noexcept -> bool {
        return (static_cast<u8>(loci)
            & (u8{1} << static_cast<u8>(locus))) != 0;
    }
};

// This is the closed continuation contract for the published syscall ABI.
// Handlers may reject arguments or object state, but cannot change the
// continuation class according to a user mode bit or the current target kind.
[[nodiscard]] constexpr auto policy(usize number) noexcept -> Policy {
    switch (number) {
    case MYOS_SYS_EXIT:
        return {Continuation::Terminal, TargetMask::Both,
            static_cast<LocusMask>(
                static_cast<u8>(LocusMask::ThreadBase)
                | static_cast<u8>(LocusMask::Vproc))};
    case MYOS_SYS_CAP_REVOKE:
    case MYOS_SYS_RESOURCE_CLOSE:
    case MYOS_SYS_NOTIFICATION_WAIT:
    case MYOS_SYS_ENDPOINT_CALL:
        return {Continuation::ThreadBlocking, TargetMask::Thread,
            LocusMask::Thread};
    case MYOS_SYS_VPROC_RETURN:
        return {Continuation::Resume, TargetMask::Vproc,
            LocusMask::VprocUpcall};
    case MYOS_SYS_VPROC_PARK:
        return {Continuation::LanePark, TargetMask::Vproc,
            LocusMask::VprocBase};
    case MYOS_SYS_ENDPOINT_REPLY:
    case MYOS_SYS_ENDPOINT_ABORT:
        return {Continuation::Resume, TargetMask::Thread,
            LocusMask::EndpointLeaf};
    case MYOS_SYS_VM_MAP:
    case MYOS_SYS_VM_UNMAP:
    case MYOS_SYS_VM_PROTECT:
        return {Continuation::SplitPhase, TargetMask::Both, LocusMask::All};
    case MYOS_SYS_VPROC_ARM:
    case MYOS_SYS_VPROC_CHECKPOINT:
    case MYOS_SYS_OPERATION_POLL:
    case MYOS_SYS_OPERATION_CANCEL:
    case MYOS_SYS_OPERATION_FINISH:
    case MYOS_SYS_TUNNEL_CONNECT:
    case MYOS_SYS_TUNNEL_INVOKE:
    case MYOS_SYS_TUNNEL_ACK:
    case MYOS_SYS_TUNNEL_CLOSE:
    case MYOS_SYS_NOTIFICATION_BIND_VPROC:
    case MYOS_SYS_NOTIFICATION_UNBIND_VPROC:
        return {Continuation::Immediate, TargetMask::Vproc,
            LocusMask::Vproc};
    case MYOS_SYS_YIELD:
    case MYOS_SYS_SC_BIND:
    case MYOS_SYS_EXECUTION_START:
    case MYOS_SYS_CAP_CLOSE:
    case MYOS_SYS_CAP_DUPLICATE:
    case MYOS_SYS_CAP_DELEGATE:
    case MYOS_SYS_CAP_MOVE:
    case MYOS_SYS_OBJECT_DESTROY:
    case MYOS_SYS_VM_CREATE_REGION:
    case MYOS_SYS_VM_RESERVE:
    case MYOS_SYS_VM_GUARD:
    case MYOS_SYS_VM_DESTROY_REGION:
    case MYOS_SYS_RESOURCE_CREATE_CHILD:
    case MYOS_SYS_MEMORY_CREATE:
    case MYOS_SYS_VSPACE_CREATE:
    case MYOS_SYS_CSPACE_CREATE:
    case MYOS_SYS_SC_CREATE:
    case MYOS_SYS_THREAD_CREATE:
    case MYOS_SYS_NOTIFICATION_CREATE:
    case MYOS_SYS_VPROC_CREATE:
    case MYOS_SYS_ENDPOINT_CREATE:
    case MYOS_SYS_MEMORY_SEAL:
    case MYOS_SYS_NOTIFICATION_SIGNAL:
    case MYOS_SYS_NOTIFICATION_TAKE:
    case MYOS_SYS_ENDPOINT_CLOSE:
    case MYOS_SYS_ENDPOINT_MINT:
        return {Continuation::Immediate, TargetMask::Both, LocusMask::All};
    case MYOS_SYS_TUNNEL_OPEN:
        return {Continuation::Immediate, TargetMask::Vproc,
            LocusMask::Vproc};
    default:
        return {};
    }
}

static_assert(policy(MYOS_SYS_ENDPOINT_CALL).continuation
    == Continuation::ThreadBlocking);
static_assert(!policy(MYOS_SYS_ENDPOINT_CALL).allows_vproc());
static_assert(policy(MYOS_SYS_ENDPOINT_CALL).allows(Locus::ThreadBase));
static_assert(policy(MYOS_SYS_ENDPOINT_CALL).allows(Locus::EndpointLeaf));
static_assert(policy(MYOS_SYS_ENDPOINT_ABORT).continuation
    == Continuation::Resume);
static_assert(policy(MYOS_SYS_ENDPOINT_ABORT).allows_thread());
static_assert(!policy(MYOS_SYS_ENDPOINT_ABORT).allows_vproc());
static_assert(policy(MYOS_SYS_ENDPOINT_ABORT).allows(Locus::EndpointLeaf));
static_assert(!policy(MYOS_SYS_ENDPOINT_ABORT).allows(Locus::ThreadBase));
static_assert(policy(MYOS_SYS_VPROC_RETURN).allows_vproc());
static_assert(!policy(MYOS_SYS_VPROC_RETURN).allows_thread());
static_assert(policy(MYOS_SYS_VPROC_RETURN).allows(Locus::VprocUpcall));
static_assert(!policy(MYOS_SYS_VPROC_RETURN).allows(Locus::VprocBase));
static_assert(policy(MYOS_SYS_VPROC_PARK).allows(Locus::VprocBase));
static_assert(!policy(MYOS_SYS_VPROC_PARK).allows(Locus::VprocUpcall));
static_assert(policy(MYOS_SYS_EXIT).allows(Locus::ThreadBase));
static_assert(!policy(MYOS_SYS_EXIT).allows(Locus::EndpointLeaf));

} // namespace kernel::syscall
