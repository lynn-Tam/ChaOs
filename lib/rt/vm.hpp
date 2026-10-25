#pragma once
#include <ipc.hpp>
#include <start.hpp>
#include <expected>
#include <algorithm>
#include <memory>
#include <mutex>
#include <sync.hpp>
#include <string.h>

inline auto _vm_wait(CreateAuth auth, cap_t space, word_t kind, VmReq request = {}) noexcept -> status_t {
    ::memcpy(reinterpret_cast<void*>(_ipc()), &request, sizeof(request));
    const auto submitted = _vm_submit(auth, space, kind);
    if (submitted.status != STATUS_PENDING) return submitted.status;
    _cap_owner op{submitted.value};
    return _op_wait(op.selector()).status;
}
inline auto _vm_wait(cap_t space, word_t kind, VmReq request = {}) noexcept -> status_t {
    return _vm_wait(_startup().alloc(), space, kind, request);
}
inline auto _io_wait(CreateAuth auth, cap_t space, word_t kind, IoReq request) noexcept -> _sys_result {
    ::memcpy(reinterpret_cast<void*>(_ipc()), &request, sizeof(request));
    const auto submitted = _io_submit(auth, space, kind);
    if (submitted.status != STATUS_PENDING) return submitted;
    _cap_owner op{submitted.value};
    return _op_wait(op.selector());
}
inline auto _io_close_wait(cap_t space) noexcept -> status_t {
    const auto requested = _io_space_close(space);
    if (requested.status != STATUS_OK) return requested.status;
    const auto done = _wait_one(space, (word_t{1} << IO_SPACE_CLOSED) | (word_t{1} << IO_SPACE_FAILED));
    return done.status != STATUS_OK ? done.status : done.value & (word_t{1} << IO_SPACE_FAILED)
        ? STATUS_BACKING_FAILED : STATUS_OK;
}

// Bounded copies through this thread's IPC aperture. No foreign VA lease is
// required; revocation is reported by the native object operation.
struct _mem_result { size_t bytes{}; status_t status{}; };
inline auto _mem_read(cap_t cap, uint64_t offset, std::span<std::byte> output) -> _mem_result {
    size_t copied{};
    while (!output.empty()) {
        const auto size = std::min(output.size(), size_t{4096} - size_t(offset % 4096));
        const auto status = _memory_read(cap, offset, 0, size).status;
        if (status != STATUS_OK) return {copied, status};
        ::memmove(output.data(), reinterpret_cast<const void*>(_ipc()), size);
        copied += size; offset += size; output = output.subspan(size);
    }
    return {copied, STATUS_OK};
}
inline auto _mem_write(cap_t cap, uint64_t offset, std::span<const std::byte> input) -> _mem_result {
    size_t copied{};
    while (!input.empty()) {
        const auto size = std::min(input.size(), size_t{4096} - size_t(offset % 4096));
        ::memmove(reinterpret_cast<void*>(_ipc()), input.data(), size);
        const auto status = _memory_write(cap, offset, 0, size).status;
        if (status != STATUS_OK) return {copied, status};
        copied += size; offset += size; input = input.subspan(size);
    }
    return {copied, STATUS_OK};
}


// Borrowed VSpace and policy for automatic placement. The kernel owns layout.
struct _vm_space {
    cap_t space{};
    uintptr_t begin{}, end{UINTPTR_MAX};
};
auto _vm() noexcept -> _vm_space&;

// Owns a kernel address claim. Closing waits for its translations to retire;
// it cannot clear records installed by a later claim at the same address.
struct _vm_area {
    _cap_owner memory{}, region{};
    uintptr_t address{};
    size_t size{};
    _vm_area() noexcept = default;
    _vm_area(const _vm_area&) = delete;
    auto operator=(const _vm_area&) -> _vm_area& = delete;
    _vm_area(_vm_area&& other) noexcept { take(other); }
    auto operator=(_vm_area&& other) noexcept -> _vm_area& {
        if (this != &other) { require_close(); take(other); }
        return *this;
    }
    ~_vm_area() { require_close(); }
    auto open(_vm_space& space, uintptr_t va, size_t bytes,
              word_t perms = VM_READ | VM_WRITE) noexcept -> status_t {
        if (region) return STATUS_BUSY;
        const VmAlloc req{VM_ALLOC_VERSION, sizeof(VmAlloc), va, bytes, 4096, space.begin, space.end, perms};
        ::memcpy(reinterpret_cast<void*>(_ipc()), &req, sizeof(req));
        const auto result = _vm_reserve(space.space);
        if (result.status != STATUS_OK) return result.status;
        region = _cap_owner{result.value}; address = result.value2; size = bytes;
        return STATUS_OK;
    }
    auto guard(size_t bytes) noexcept -> status_t { return _vm_guard(region.selector(), address, bytes).status; }
    auto attach(cap_t mem, word_t first, size_t bytes, word_t perms, size_t offset = 0) noexcept -> status_t {
        if (!region || mapped_ || !mem || !bytes || bytes % 4096 || offset > size || bytes > size - offset || offset % 4096)
            return STATUS_BAD_ARGS;
        const auto status = _vm_wait(region.selector(), VM_OP_MAP, {mem, address + offset, bytes, first, perms});
        if (status == STATUS_OK) { mapped_ = bytes; mapped_offset_ = offset; }
        return status;
    }
    auto detach() noexcept -> status_t {
        if (!mapped_) return STATUS_OK;
        const auto status = _vm_wait(region.selector(), VM_OP_UNMAP, {0, address + mapped_offset_, mapped_, 0, 0});
        if (status == STATUS_OK) mapped_ = 0;
        return status;
    }
    auto close() noexcept -> status_t {
        if (region) {
            const auto status = _vm_wait(region.selector(), VM_OP_CLEAR);
            if (status != STATUS_OK) return status;
            if (const auto status = region.close(); status != STATUS_OK) return status;
        }
        if (const auto status = memory.close(); status != STATUS_OK) return status;
        address = size = mapped_ = mapped_offset_ = 0;
        return STATUS_OK;
    }
private:
    friend void _thread_init();
    // Only a validated startup handoff transfers an existing claim to the CRT.
    static auto bootstrap(_cap_owner region, _cap_owner memory, uintptr_t va, size_t bytes, size_t guard = 0) -> _vm_area {
        _vm_area area;
        area.region = std::move(region); area.memory = std::move(memory);
        area.address = va; area.size = bytes; area.mapped_ = bytes - guard; area.mapped_offset_ = guard;
        return area;
    }
public:
    static auto map(_vm_space& space, cap_t memory, uintptr_t va, size_t size, word_t perms)
        noexcept -> std::expected<_vm_area, status_t> {
        _vm_area area;
        auto status = area.open(space, va, size, perms & ~VM_MAP_PRIVATE);
        if (status == STATUS_OK) status = area.attach(memory, 0, size, perms);
        if (status != STATUS_OK) return std::unexpected(status);
        return area;
    }
    static auto map(_vm_space& space, _cap_owner memory, uintptr_t va, size_t size, word_t perms)
        noexcept -> std::expected<_vm_area, status_t> {
        auto area = map(space, memory.selector(), va, size, perms);
        if (area) area->memory = std::move(memory);
        return area;
    }
    static auto map_private(_vm_space& space, _cap_owner memory, uintptr_t va, size_t size)
        noexcept -> std::expected<_vm_area, status_t> {
        return map(space, std::move(memory), va, size, VM_READ | VM_WRITE | VM_MAP_PRIVATE);
    }
    static auto create(CreateAuth auth, _vm_space& space, uintptr_t va, size_t size, word_t memory_perms, word_t map_perms)
        noexcept -> std::expected<_vm_area, status_t> {
        const auto result = _memory_create(auth, size, memory_perms);
        _cap_owner memory{result.value};
        auto status = result.status;
        if (status == STATUS_PENDING) status = _op_wait(memory.selector()).status;
        if (status != STATUS_OK) return std::unexpected(status);
        return map(space, std::move(memory), va, size, map_perms);
    }
    static auto create(CreateAuth auth, _vm_space& space, uintptr_t va, size_t size, word_t perms = VM_READ | VM_WRITE)
        noexcept -> std::expected<_vm_area, status_t> {
        return create(auth, space, va, size, perms, perms);
    }
private:
    size_t mapped_{}, mapped_offset_{};
    void require_close() noexcept { if (const auto status = close(); status != STATUS_OK) _thread_exit(status); }
    void take(_vm_area& other) noexcept {
        memory = std::move(other.memory); region = std::move(other.region);
        address = std::exchange(other.address, 0); size = std::exchange(other.size, 0);
        mapped_ = std::exchange(other.mapped_, 0); mapped_offset_ = std::exchange(other.mapped_offset_, 0);
    }
};

// Native, explicitly funded object storage. The object dies before its mapping;
// the allocation's Quota and the provider-owned Scope remain independent.
template<class T> struct _vm_delete {
    _vm_area storage;
    void operator()(T* object) noexcept { std::destroy_at(object); storage = {}; }
};
template<class T> using _vm_ptr = std::unique_ptr<T, _vm_delete<T>>;
template<class T, class... Args>
auto _vm_new(CreateAuth auth, Args&&... args) noexcept -> std::expected<_vm_ptr<T>, status_t> {
    constexpr size_t bytes = (sizeof(T) + (alignof(T) > 4096 ? alignof(T) - 1 : 0) + 4095) & ~size_t{4095};
    auto storage = _vm_area::create(auth, _vm(), 0, bytes);
    if (!storage) return std::unexpected(storage.error());
    const auto address = (storage->address + alignof(T) - 1) & ~uintptr_t{alignof(T) - 1};
    auto* object = std::construct_at(reinterpret_cast<T*>(address), std::forward<Args>(args)...);
    return _vm_ptr<T>{object, _vm_delete<T>{std::move(*storage)}};
}
