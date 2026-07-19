#include <object/memory_pool.hpp>

#include "kernel/syscall/internal.hpp"

#include <execution/vproc.hpp>
#include <mm/vspace.hpp>
#include <operation/completion.hpp>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

[[nodiscard]] auto page_authorized(
    const cap::Resolved<kernel::mm::MemoryObject>& memory,
    usize page,
    kernel::mm::AccessMask access) noexcept -> bool {
    const cap::EffectiveAuthority effective = memory.authority();
    const auto* const authority = libk::get_if<cap::MemoryAuthority>(
        &effective.data);
    return authority != nullptr
        && authority->range.contains(kernel::mm::ObjectRange{page, 1})
        && authority->access.contains(access)
        && authority->types.contains(kernel::mm::MemoryType::Normal);
}

[[nodiscard]] auto memory_status(kernel::mm::MemoryError error) noexcept
    -> myos_status_t {
    switch (error) {
    case kernel::mm::MemoryError::OutOfMemory:
    case kernel::mm::MemoryError::ResourceExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case kernel::mm::MemoryError::BackingFailed:
        return MYOS_STATUS_BACKING_FAILED;
    case kernel::mm::MemoryError::Busy:
        return MYOS_STATUS_BUSY;
    default:
        return MYOS_STATUS_BAD_ARGS;
    }
}

[[nodiscard]] auto prepare_arm(
    Invocation& invocation,
    cap::Resolved<kernel::mm::MemoryObject>& code,
    cap::Resolved<kernel::mm::MemoryObject>& stack,
    const myos_vproc_arm& descriptor) noexcept
    -> libk::Expected<VprocArm, myos_status_t> {
    const auto rx = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Execute);
    const auto rw = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    const kernel::mm::VirtAddr code_address{descriptor.code_address};
    const kernel::mm::VirtAddr stack_address{descriptor.stack_address};
    const kernel::mm::VirtRange code_range{code_address, kernel::mm::page_size};
    const kernel::mm::VirtRange stack_range{
        stack_address, kernel::mm::page_size};
    if (descriptor.version != MYOS_VPROC_ARM_VERSION
        || descriptor.flags != 0 || descriptor.code_pages != 1
        || descriptor.stack_pages != 1
        || !code_address.valid() || !stack_address.valid()
        || !code_address.is_aligned(kernel::mm::page_size)
        || !stack_address.is_aligned(kernel::mm::page_size)
        || code_range.intersects(stack_range)
        || descriptor.entry < code_address.raw()
        || descriptor.entry >= code_address.raw() + kernel::mm::page_size
        || descriptor.stack_top <= stack_address.raw()
        || descriptor.stack_top > stack_address.raw() + kernel::mm::page_size
        || !page_authorized(code, descriptor.code_page, rx)
        || !page_authorized(stack, descriptor.stack_page, rw)) {
        return libk::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    auto code_ref = code.reference();
    auto stack_ref = stack.reference();
    if (!code_ref || !stack_ref) {
        return libk::unexpected(MYOS_STATUS_BUSY);
    }
    auto code_view = invocation.vspace.bind_view(kernel::mm::UserViewRequest{
        .memory = libk::move(code_ref).value(),
        .object = kernel::mm::ObjectRange{descriptor.code_page, 1},
        .virtual_range = code_range,
        .access = rx,
    });
    if (!code_view) {
        return libk::unexpected(vm_status(code_view.error()));
    }
    auto stack_view = invocation.vspace.bind_view(kernel::mm::UserViewRequest{
        .memory = libk::move(stack_ref).value(),
        .object = kernel::mm::ObjectRange{descriptor.stack_page, 1},
        .virtual_range = stack_range,
        .access = rw,
    });
    if (!stack_view) {
        return libk::unexpected(vm_status(stack_view.error()));
    }
    auto code_page = code->materialize(descriptor.code_page);
    auto stack_page = stack->materialize(descriptor.stack_page);
    if (!code_page || !stack_page) {
        return libk::unexpected(memory_status(
            !code_page ? code_page.error() : stack_page.error()));
    }
    arch::UserStart entry{
        .entry = kernel::mm::VirtAddr{descriptor.entry},
        .stack = kernel::mm::VirtAddr{descriptor.stack_top},
    };
    if (!arch::valid_user_start(entry)) {
        return libk::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    VprocArm arm{};
    arm.code_view = libk::move(code_view).value();
    arm.stack_view = libk::move(stack_view).value();
    arm.code_page = libk::move(code_page).value();
    arm.stack_page = libk::move(stack_page).value();
    arm.entry = entry;
    return libk::expected(libk::move(arm));
}

[[nodiscard]] auto error_status(VprocError error) noexcept -> myos_status_t {
    switch (error) {
    case VprocError::InvalidRuntime:
        return MYOS_STATUS_BAD_ARGS;
    case VprocError::InvalidState:
        return MYOS_STATUS_BUSY;
    case VprocError::TableFull:
        return MYOS_STATUS_NO_MEMORY;
    case VprocError::InvalidKey:
    case VprocError::GenerationExhausted:
        return MYOS_STATUS_NOT_FOUND;
    }
    return MYOS_STATUS_INTERNAL;
}

[[gnu::noinline]] [[nodiscard]] auto commit_arm(
    Invocation& invocation,
    Vproc& vproc,
    const myos_vproc_arm& descriptor) noexcept -> Result {
    auto code = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(descriptor.code_memory),
        cap::Rights::of(cap::Right::Map));
    auto stack = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(descriptor.stack_memory),
        cap::Rights::of(cap::Right::Map));
    if (!code || !stack) {
        return returned(cap_status(!code ? code.error() : stack.error()));
    }
    auto registration = prepare_arm(
        invocation, code.value(), stack.value(), descriptor);
    if (!registration) {
        return returned(registration.error());
    }
    auto armed = vproc.arm(
        code.value(), stack.value(), libk::move(registration).value());
    return armed
        ? returned(MYOS_STATUS_OK)
        : returned(error_status(armed.error()));
}

[[gnu::noinline]] [[nodiscard]] auto arm(
    Invocation& invocation,
    Vproc& vproc) noexcept -> Result {
    auto snapshot = read_snapshot<myos_vproc_arm>(
        invocation,
        handle_of(invocation.trap.arg(0)),
        invocation.trap.arg(1));
    return snapshot
        ? commit_arm(invocation, vproc, snapshot.value())
        : returned(snapshot.error());
}

} // namespace

auto handle_vproc(usize operation, Invocation& invocation) noexcept -> Result {
    Vproc* const vproc = invocation.target.vproc();
    if (vproc == nullptr) {
        return returned(MYOS_STATUS_INVALID_OP);
    }
    switch (operation) {
    case MYOS_SYS_VPROC_ARM:
        return arm(invocation, *vproc);
    case MYOS_SYS_VPROC_RETURN: {
        auto resumed = vproc->resume(invocation.trap, invocation.trap.arg(0));
        return resumed
            ? Result{MYOS_STATUS_OK, 0, Disposition::Resume}
            : returned(error_status(resumed.error()));
    }
    case MYOS_SYS_VPROC_CHECKPOINT:
        return returned(MYOS_STATUS_OK, vproc->pending_sequence());
    case MYOS_SYS_OPERATION_POLL: {
        auto result = vproc->poll_operation(
            operation::Key{invocation.trap.arg(0)});
        return result
            ? returned(result.value().status, result.value().value)
            : returned(error_status(result.error()));
    }
    case MYOS_SYS_OPERATION_CANCEL: {
        auto canceled = vproc->cancel_operation(
            operation::Key{invocation.trap.arg(0)});
        return canceled
            ? returned(MYOS_STATUS_OK)
            : returned(error_status(canceled.error()));
    }
    case MYOS_SYS_OPERATION_FINISH: {
        auto result = vproc->finish_operation(
            operation::Key{invocation.trap.arg(0)});
        return result
            ? returned(result.value().status, result.value().value)
            : returned(error_status(result.error()));
    }
    case MYOS_SYS_VPROC_PARK: {
        auto parked = vproc->request_park(invocation.trap.arg(0));
        return parked
            ? Result{MYOS_STATUS_OK, 0, Disposition::Park}
            : returned(error_status(parked.error()));
    }
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
