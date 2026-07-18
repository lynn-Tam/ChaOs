#include "kernel/syscall/internal.hpp"

#include <core/kernel_state.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_runtime.hpp>
#include <mm/memory_object.hpp>
#include <object/memory_pool.hpp>
#include <object/resource_pool.hpp>
#include <resource/pool.hpp>
#include <thread/thread.hpp>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

[[nodiscard]] auto memory_status(kernel::mm::MemoryError error) noexcept
    -> myos_status_t {
    switch (error) {
    case kernel::mm::MemoryError::OutOfMemory:
    case kernel::mm::MemoryError::ResourceExhausted:
    case kernel::mm::MemoryError::GenerationExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case kernel::mm::MemoryError::Busy:
    case kernel::mm::MemoryError::InvalidState:
    case kernel::mm::MemoryError::AttachmentState:
        return MYOS_STATUS_BUSY;
    case kernel::mm::MemoryError::BackingFailed:
        return MYOS_STATUS_BACKING_FAILED;
    case kernel::mm::MemoryError::InvalidSize:
    case kernel::mm::MemoryError::InvalidRange:
    case kernel::mm::MemoryError::InvalidAccess:
    case kernel::mm::MemoryError::InvalidMemoryType:
    case kernel::mm::MemoryError::NotBacked:
    case kernel::mm::MemoryError::OwnershipMismatch:
        return MYOS_STATUS_BAD_ARGS;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] auto grant_status(cap::GrantError error) noexcept
    -> myos_status_t {
    switch (error) {
    case cap::GrantError::OutOfMemory:
    case cap::GrantError::QuotaExceeded:
    case cap::GrantError::GenerationExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case cap::GrantError::RightsViolation:
        return MYOS_STATUS_BAD_RIGHTS;
    case cap::GrantError::InvalidKey:
    case cap::GrantError::WrongKind:
        return MYOS_STATUS_INVALID_CAP;
    case cap::GrantError::InvalidState:
    case cap::GrantError::RevocationConflict:
        return MYOS_STATUS_BUSY;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] auto seal(Invocation& invocation) noexcept -> Result {
    auto memory = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(invocation.trap.arg(0)),
        cap::Rights::of(cap::Right::Manage));
    if (!memory) {
        return returned(cap_status(memory.error()));
    }
    auto sealed = memory.value()->seal();
    return returned(sealed ? MYOS_STATUS_OK : memory_status(sealed.error()));
}

[[nodiscard]] auto close_pool(Invocation& invocation) noexcept -> Result {
    Thread* const thread = invocation.target.thread();
    if (thread == nullptr) {
        return returned(MYOS_STATUS_INVALID_OP);
    }
    if (thread->waiting()) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto pool = invocation.cspace.resolve<kernel::resource::ResourcePool>(
        handle_of(invocation.trap.arg(0)),
        cap::Rights::of(cap::Right::Close));
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    auto self = pool.value().reference();
    if (!self) {
        return returned(MYOS_STATUS_BUSY);
    }
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    CpuRegistry* const cpus = invocation.cpu.runtime().owner_registry;
    KASSERT(kernel != nullptr && cpus != nullptr);
    auto closed = kernel->grants().close_pool(
        pool.value().object(), self.value(), *thread, *cpus);
    if (!closed) {
        return returned(grant_status(closed.error()));
    }
    if (closed.value() == kernel::operation::State::Waiting) {
        return Result{MYOS_STATUS_OK, 0, Disposition::Block};
    }
    thread->resume_wait(invocation.trap);
    return returned(MYOS_STATUS_OK);
}

} // namespace

auto handle_object(usize operation, Invocation& invocation) noexcept -> Result {
    switch (operation) {
    case MYOS_SYS_MEMORY_SEAL:
        return seal(invocation);
    case MYOS_SYS_RESOURCE_CLOSE:
        return close_pool(invocation);
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
