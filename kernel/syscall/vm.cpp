#include "kernel/syscall/internal.hpp"

#include <libk/variant.hpp>
#include <mm/memory_object.hpp>
#include <object/object_store.hpp>
#include <uapi/syscall.h>
#include <uapi/vm.h>

namespace kernel::syscall {

auto handle_vm(usize operation, Invocation& invocation) noexcept -> Result {
    arch::TrapContext& trap = invocation.trap;
    cap::CSpace& cspace = invocation.cspace;
    const cap::CapHandle vspace_handle = handle_of(trap.arg(0));
    const cap::Right required = operation == MYOS_SYS_VM_MAP
        ? cap::Right::Map
        : operation == MYOS_SYS_VM_UNMAP
            ? cap::Right::Unmap
            : operation == MYOS_SYS_VM_PROTECT
                ? cap::Right::Protect
                : operation == MYOS_SYS_VM_CREATE_REGION
                    ? cap::Right::CreateRegion
                    : operation == MYOS_SYS_VM_DESTROY_REGION
                        ? cap::Right::Destroy
                        : cap::Right::Reserve;
    if (!vspace_handle) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    auto target = cspace.resolve<kernel::mm::VSpace>(
        vspace_handle, cap::Rights::of(required));
    if (!target) {
        return returned(cap_status(target.error()));
    }
    const cap::EffectiveAuthority effective = target.value().authority();
    const auto* const where =
        libk::get_if<cap::VSpaceAuthority>(&effective.data);
    KASSERT(where != nullptr);
    kernel::mm::VSpace& space = target.value().object();
    const kernel::mm::VmContext vm = vm_context(invocation.cpu);

    if (operation == MYOS_SYS_VM_DESTROY_REGION) {
        auto destroyed = space.destroy_region(vm, where->region);
        return returned(destroyed
            ? operation_status(destroyed.value())
            : vm_status(destroyed.error()));
    }
    if (operation == MYOS_SYS_VM_MAP) {
        const cap::CapHandle memory_handle = handle_of(trap.arg(1));
        const auto range = range_of(trap.arg(2), trap.arg(3));
        const auto access = access_of(trap.arg(5));
        if (!memory_handle || !range || !access) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        auto memory = cspace.resolve<kernel::mm::MemoryObject>(
            memory_handle, cap::Rights::of(cap::Right::Map));
        if (!memory) {
            return returned(cap_status(memory.error()));
        }
        const usize pages = range->size() / kernel::mm::page_size;
        auto mapped = space.map(
            vm,
            *where,
            kernel::mm::MapRequest{
                *range,
                kernel::mm::ObjectRange{trap.arg(4), pages},
                *access},
            memory.value());
        return returned(mapped
            ? operation_status(mapped.value().status)
            : vm_status(mapped.error()));
    }

    const auto range = range_of(trap.arg(1), trap.arg(2));
    if (!range) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    if (operation == MYOS_SYS_VM_UNMAP) {
        auto unmapped = space.unmap(vm, *where, *range);
        return returned(unmapped
            ? operation_status(unmapped.value())
            : vm_status(unmapped.error()));
    }
    if (operation == MYOS_SYS_VM_PROTECT) {
        const auto access = access_of(trap.arg(3));
        if (!access) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        auto protected_range = space.protect(vm, *where, *range, *access);
        return returned(protected_range
            ? operation_status(protected_range.value())
            : vm_status(protected_range.error()));
    }
    if (operation == MYOS_SYS_VM_CREATE_REGION) {
        const auto access = access_of(trap.arg(3));
        const auto types = types_of(trap.arg(4));
        const auto rights = rights_of(trap.arg(5));
        if (!access || !types || !rights) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        auto created = space.create_region(
            target.value(),
            cspace,
            *range,
            kernel::mm::RegionPolicy{*access, *types},
            *rights);
        return returned(
            created ? MYOS_STATUS_OK : vm_status(created.error()),
            created ? created.value().capability.raw() : 0);
    }
    if (operation == MYOS_SYS_VM_RESERVE || operation == MYOS_SYS_VM_GUARD) {
        auto reserved = operation == MYOS_SYS_VM_RESERVE
            ? space.reserve(where->region, *range)
            : space.guard(where->region, *range);
        return returned(
            reserved ? MYOS_STATUS_OK : vm_status(reserved.error()));
    }
    return returned(MYOS_STATUS_INVALID_OP);
}

} // namespace kernel::syscall
