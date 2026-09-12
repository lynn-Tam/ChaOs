#pragma once

#include <libk/expected.hpp>
#include <user/lib/capability_syscall.hpp>

namespace myos {

// A task-local mapping and its capability references. The task's VSpace and
// ResourcePool own teardown; dropping this record does not unmap live users.
struct MappedMemory final {
    cap::OwnedCap memory{};
    cap::OwnedCap region{};
    uintptr_t address{};
    size_t size{};

    [[nodiscard]] static auto map(myos_cap_t vspace, cap::OwnedCap&& memory,
        uintptr_t address, size_t size, myos_word_t access,
        myos_word_t type = MYOS_VM_NORMAL) noexcept -> libk::Expected<MappedMemory, myos_status_t> {
        const auto region = vm_create_region(vspace, address, size, access, type,
            MYOS_RIGHT_MAP | MYOS_RIGHT_PROTECT | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
        if (region.status != MYOS_STATUS_OK) return libk::unexpected(region.status);
        MappedMemory result{libk::move(memory), cap::OwnedCap{{region.value, 0}}, address, size};
        const auto mapped = vm_map(region.value, result.memory.selector(), address, size, 0, access);
        if (mapped.status != MYOS_STATUS_OK && mapped.status != MYOS_STATUS_PENDING)
            return libk::unexpected(mapped.status);
        return libk::expected(libk::move(result));
    }

    [[nodiscard]] static auto create(myos_cap_t pool, myos_cap_t vspace,
        uintptr_t address, size_t size) noexcept -> libk::Expected<MappedMemory, myos_status_t> {
        const auto memory = memory_create(pool, size, MYOS_VM_READ | MYOS_VM_WRITE);
        if (memory.status != MYOS_STATUS_OK) return libk::unexpected(memory.status);
        return map(vspace, cap::OwnedCap{{memory.value, 0}}, address, size, MYOS_VM_READ | MYOS_VM_WRITE);
    }
};

} // namespace myos
