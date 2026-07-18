#include "kernel/syscall/internal.hpp"

#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <libk/limits.hpp>

namespace kernel::syscall {

auto cap_status(cap::CSpaceError error) noexcept -> myos_status_t {
    switch (error) {
    case cap::CSpaceError::InvalidHandle:
    case cap::CSpaceError::WrongKind:
        return MYOS_STATUS_INVALID_CAP;
    case cap::CSpaceError::Denied:
        return MYOS_STATUS_BAD_RIGHTS;
    case cap::CSpaceError::Amplification:
        return MYOS_STATUS_DENIED;
    case cap::CSpaceError::OutOfMemory:
    case cap::CSpaceError::SlotQuota:
    case cap::CSpaceError::PageQuota:
    case cap::CSpaceError::GenerationExhausted:
    case cap::CSpaceError::ResourceExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case cap::CSpaceError::InvalidState:
    case cap::CSpaceError::Contended:
    case cap::CSpaceError::GrantUnavailable:
        return MYOS_STATUS_BUSY;
    }
    return MYOS_STATUS_INTERNAL;
}

auto vm_status(kernel::mm::VSpaceError error) noexcept -> myos_status_t {
    switch (error) {
    case kernel::mm::VSpaceError::InvalidAuthority:
    case kernel::mm::VSpaceError::InvalidAccess:
        return MYOS_STATUS_BAD_RIGHTS;
    case kernel::mm::VSpaceError::InvalidRegion:
    case kernel::mm::VSpaceError::InvalidMapping:
    case kernel::mm::VSpaceError::NotMapped:
        return MYOS_STATUS_NOT_FOUND;
    case kernel::mm::VSpaceError::InvalidRange:
    case kernel::mm::VSpaceError::Overlap:
    case kernel::mm::VSpaceError::UnsupportedMemoryType:
    case kernel::mm::VSpaceError::AliasConflict:
        return MYOS_STATUS_BAD_ARGS;
    case kernel::mm::VSpaceError::OutOfMemory:
    case kernel::mm::VSpaceError::QuotaExceeded:
    case kernel::mm::VSpaceError::GenerationExhausted:
    case kernel::mm::VSpaceError::ResourceExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case kernel::mm::VSpaceError::InvalidState:
    case kernel::mm::VSpaceError::Busy:
    case kernel::mm::VSpaceError::GrantUnavailable:
        return MYOS_STATUS_BUSY;
    case kernel::mm::VSpaceError::ShootdownUnavailable:
        return MYOS_STATUS_RETRY;
    case kernel::mm::VSpaceError::BackingFailed:
        return MYOS_STATUS_BACKING_FAILED;
    case kernel::mm::VSpaceError::TranslationCorrupt:
        KASSERT(false);
        __builtin_unreachable();
    }
    return MYOS_STATUS_INTERNAL;
}

auto operation_status(kernel::mm::VmStatus status) noexcept -> myos_status_t {
    return status == kernel::mm::VmStatus::Complete
        ? MYOS_STATUS_OK
        : MYOS_STATUS_PENDING;
}

auto handle_of(usize raw) noexcept -> cap::CapHandle {
    return cap::CapHandle::from_raw(static_cast<u64>(raw));
}

auto rights_of(usize raw) noexcept -> libk::optional<cap::Rights> {
    return cap::Rights::from_raw(static_cast<u64>(raw));
}

auto access_of(usize raw) noexcept -> libk::optional<kernel::mm::AccessMask> {
    if (raw > libk::numeric_limits<u8>::max()) {
        return libk::nullopt;
    }
    const auto access = kernel::mm::AccessMask::from_raw(static_cast<u8>(raw));
    return kernel::mm::valid_access(access)
        ? libk::optional<kernel::mm::AccessMask>{access}
        : libk::nullopt;
}

auto types_of(usize raw) noexcept -> libk::optional<kernel::mm::MemoryTypes> {
    if (raw > libk::numeric_limits<u8>::max()) {
        return libk::nullopt;
    }
    const auto types = kernel::mm::MemoryTypes::from_raw(static_cast<u8>(raw));
    return kernel::mm::valid_memory_types(types)
        ? libk::optional<kernel::mm::MemoryTypes>{types}
        : libk::nullopt;
}

auto range_of(usize base, usize size) noexcept
    -> libk::optional<kernel::mm::VirtRange> {
    const kernel::mm::VirtRange range{kernel::mm::VirtAddr{base}, size};
    return range.valid() && !range.empty()
        && (base & (kernel::mm::page_size - 1)) == 0
        && (size & (kernel::mm::page_size - 1)) == 0
        ? libk::optional<kernel::mm::VirtRange>{range}
        : libk::nullopt;
}

auto vm_context(CpuLocal& cpu) noexcept -> kernel::mm::VmContext {
    KASSERT(cpu.descriptor != nullptr);
    return kernel::mm::VmContext{
        .cpus = cpu.runtime().owner_registry,
        .local = cpu.descriptor->logical_id(),
    };
}

} // namespace kernel::syscall
