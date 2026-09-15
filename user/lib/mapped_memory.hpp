#pragma once

#include <libk/expected.hpp>
#include <user/lib/capability_syscall.hpp>

namespace myos {

// Owns a task-local region and its capability references. create() additionally
// owns the new anonymous object; map() only owns the imported capability.
// The caller must stop local pointer users before closing or replacing it.
struct MappedMemory final {
    cap::OwnedCap memory{};
    cap::OwnedCap region{};
    uintptr_t address{};
    size_t size{};

    MappedMemory() noexcept = default;
    MappedMemory(const MappedMemory&) = delete;
    auto operator=(const MappedMemory&) -> MappedMemory& = delete;
    MappedMemory(MappedMemory&& other) noexcept { take(other); }
    auto operator=(MappedMemory&& other) noexcept -> MappedMemory& {
        if (this != &other) {
            require_close();
            take(other);
        }
        return *this;
    }
    ~MappedMemory() noexcept { require_close(); }

    // Return follows PTE invalidation and region retirement. A successful close
    // permits immediate reuse of the virtual range.
    [[nodiscard]] auto close() noexcept -> myos_status_t {
        if (region) {
            const auto status = vm_complete(region.selector(), vm_destroy_region(region.selector())).status;
            if (status != MYOS_STATUS_OK) return status;
            region = {};
        }
        if (owns_memory_ && memory) {
            const auto status = object_destroy(memory.selector()).status;
            if (status != MYOS_STATUS_OK) return status;
        }
        memory = {};
        owns_memory_ = false;
        address = 0;
        size = 0;
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto map(myos_cap_t vspace, cap::OwnedCap&& memory,
        uintptr_t address, size_t size, myos_word_t access,
        myos_word_t type = MYOS_VM_NORMAL) noexcept -> libk::Expected<MappedMemory, myos_status_t> {
        return map_impl(vspace, libk::move(memory), address, size, access, type, false);
    }

    [[nodiscard]] static auto create(myos_cap_t pool, myos_cap_t vspace,
        uintptr_t address, size_t size) noexcept -> libk::Expected<MappedMemory, myos_status_t> {
        const auto memory = memory_create(pool, size, MYOS_VM_READ | MYOS_VM_WRITE);
        if (memory.status != MYOS_STATUS_OK) return libk::unexpected(memory.status);
        return map_impl(vspace, cap::OwnedCap{{memory.value, 0}}, address, size,
            MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_NORMAL, true);
    }

private:
    bool owns_memory_{};

    void require_close() noexcept {
        const auto status = close();
        if (status != MYOS_STATUS_OK) cap::SyscallBackend::ownership_fault(status);
    }
    void take(MappedMemory& other) noexcept {
        memory = libk::move(other.memory);
        region = libk::move(other.region);
        address = libk::exchange(other.address, 0);
        size = libk::exchange(other.size, 0);
        owns_memory_ = libk::exchange(other.owns_memory_, false);
    }
    static auto map_impl(myos_cap_t vspace, cap::OwnedCap&& memory,
        uintptr_t address, size_t size, myos_word_t access, myos_word_t type,
        bool owns_memory) noexcept -> libk::Expected<MappedMemory, myos_status_t> {
        MappedMemory result;
        result.memory = libk::move(memory);
        result.owns_memory_ = owns_memory;
        const auto region = vm_create_region(vspace, address, size, access, type,
            MYOS_RIGHT_MAP | MYOS_RIGHT_PROTECT | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
        if (region.status != MYOS_STATUS_OK) return libk::unexpected(region.status);
        result.region = cap::OwnedCap{{region.value, 0}};
        result.address = address;
        result.size = size;
        const auto mapped = vm_complete(region.value,
            vm_map(region.value, result.memory.selector(), address, size, 0, access));
        if (mapped.status != MYOS_STATUS_OK)
            return libk::unexpected(mapped.status);
        return libk::expected(libk::move(result));
    }

};

} // namespace myos
