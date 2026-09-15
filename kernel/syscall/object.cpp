#include "kernel/syscall/internal.hpp"

#include <core/kernel_state.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_runtime.hpp>
#include <mm/memory_object.hpp>
#include <object/memory_pool.hpp>
#include <object/resource_pool.hpp>
#include <object/notification_pool.hpp>
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
    case kernel::mm::MemoryError::Pending:
    case kernel::mm::MemoryError::Pressure:
        return MYOS_STATUS_WOULD_BLOCK;
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

[[nodiscard]] auto write_memory(Invocation& invocation) noexcept -> Result {
    auto memory = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(cap::Right::Manage));
    if (!memory) return returned(cap_status(memory.error()));
    const usize offset = invocation.trap.arg(1);
    const usize source = invocation.trap.arg(2);
    const usize size = invocation.trap.arg(3);
    if (size == 0 || size > mm::page_size - (offset & (mm::page_size - 1)))
        return returned(MYOS_STATUS_BAD_ARGS);
    const auto effective = memory.value().authority();
    const auto* authority = libk::get_if<cap::MemoryAuthority>(&effective.data);
    if (authority == nullptr
        || !authority->range.contains(mm::ObjectRange{offset / mm::page_size, 1})
        || !authority->access.contains(mm::Access::Write)
        || !authority->types.contains(mm::MemoryType::Normal))
        return returned(MYOS_STATUS_BAD_RIGHTS);
    auto* buffer = invocation.target.ipc_buffer();
    if (buffer == nullptr) return returned(MYOS_STATUS_BAD_ARGS);
    auto access = buffer->access();
    if (!access) return returned(MYOS_STATUS_RETRY);
    // The source lease pins bytes without acquiring locks during the copy.
    // A destination aliased by this IPC mapping is rejected by write(), which
    // requires private storage with no attachments or other page leases.
    const auto bytes = access.value().bytes(source, size);
    if (bytes.empty()) return returned(MYOS_STATUS_BAD_ARGS);
    auto written = memory.value()->write(offset, bytes);
    return returned(written ? MYOS_STATUS_OK : memory_status(written.error()));
}

[[nodiscard]] auto populate_memory(Invocation& invocation) noexcept -> Result {
    auto* thread = invocation.target.thread();
    if (thread == nullptr) return returned(MYOS_STATUS_INVALID_OP);
    if (thread->waiting()) return returned(MYOS_STATUS_BUSY);
    auto memory = invocation.cspace.resolve<mm::MemoryObject>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(cap::Right::Manage));
    if (!memory) return returned(cap_status(memory.error()));
    const usize page = invocation.trap.arg(1);
    const auto effective = memory.value().authority();
    const auto* authority = libk::get_if<cap::MemoryAuthority>(&effective.data);
    if (authority == nullptr || !authority->range.contains(mm::ObjectRange{page, 1}))
        return returned(MYOS_STATUS_BAD_RIGHTS);
    if (page >= memory.value()->size() / mm::page_size) return returned(MYOS_STATUS_BAD_ARGS);
    auto reference = memory.value().reference();
    if (!reference) return returned(MYOS_STATUS_BUSY);
    auto& access = thread->current_wait().page_access();
    const auto kind = access.populate(libk::move(reference).value(), page);
    if (kind == mm::FaultKind::Pending || kind == mm::FaultKind::Pressure) {
        auto* cpus = invocation.cpu.runtime().owner_registry;
        KASSERT(cpus != nullptr && thread->begin_wait(access.completion(), *cpus));
        access.arm();
        return Result{MYOS_STATUS_OK, 0, Disposition::Block};
    }
    const auto status = access.status();
    access.reset();
    return returned(status);
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
    static_cast<void>(thread->resume_wait(invocation.trap));
    return returned(MYOS_STATUS_OK);
}

[[nodiscard]] auto close_pool_async(Invocation& invocation) noexcept -> Result {
    auto pool = invocation.cspace.resolve<resource::ResourcePool>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(cap::Right::Close));
    // Receive authority owns the destination inbox and may choose its event
    // bits. A borrowed Signal capability cannot inject arbitrary badges.
    auto notification = invocation.cspace.resolve<ipc::Notification>(
        handle_of(invocation.trap.arg(1)), cap::Rights::of(cap::Right::Receive));
    if (!pool || !notification) return returned(cap_status(!pool ? pool.error() : notification.error()));
    const auto badge = invocation.trap.arg(2);
    if (badge == 0) return returned(MYOS_STATUS_BAD_ARGS);
    auto self = pool.value().reference();
    auto destination = notification.value().reference();
    if (!self || !destination) return returned(MYOS_STATUS_BUSY);
    const auto id = destination.value().id();
    auto& objects = invocation.cpu.runtime().kernel->objects();
    const resource::RefundNotifier notifier{&objects,
        [](void* context, usize slot, u64 generation, u64 bits) noexcept {
            // Weak generation identity: a dead inbox drops the delivery. No
            // reference can keep an object in the closing subtree alive.
            auto target = static_cast<object::ObjectStore*>(context)->pin_notification(
                {slot, generation, object::ObjectKind::Notification});
            if (target) static_cast<void>(target.value()->signal(bits));
        }, id.slot, id.generation, badge};
    if (!pool.value()->observe_refund(self.value(), notifier)) return returned(MYOS_STATUS_BUSY);
    static_cast<void>(pool.value()->close());
    return returned(MYOS_STATUS_OK);
}

} // namespace

auto handle_object(usize operation, Invocation& invocation) noexcept -> Result {
    switch (operation) {
    case MYOS_SYS_MEMORY_SEAL:
        return seal(invocation);
    case MYOS_SYS_MEMORY_POPULATE:
        return populate_memory(invocation);
    case MYOS_SYS_MEMORY_WRITE:
        return write_memory(invocation);
    case MYOS_SYS_RESOURCE_CLOSE:
        return close_pool(invocation);
    case MYOS_SYS_RESOURCE_CLOSE_ASYNC:
        return close_pool_async(invocation);
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
