#pragma once
#include <cap.hpp>
#include <start.hpp>
#include <expected>
#include <algorithm>
#include <memory>
#include <mutex>
#include <sync.hpp>
#include <string.h>

inline auto vm_call(CreateAuth auth, cap_t space, word_t kind, VmReq request = {}) noexcept -> status_t {
    ::memcpy(reinterpret_cast<void*>(rt_current().ipc), &request, sizeof(request));
    return op_wait(sys::vm_submit(auth, space, kind)).status;
}
inline auto vm_call(cap_t space, word_t kind, VmReq request = {}) noexcept -> status_t {
    return vm_call(rt_startup().alloc(), space, kind, request);
}
inline auto io_call(CreateAuth auth, cap_t space, word_t kind, IoReq request) noexcept -> sys::SysResult {
    ::memcpy(reinterpret_cast<void*>(rt_current().ipc), &request, sizeof(request));
    return op_wait(sys::io_submit(auth, space, kind));
}
inline auto io_close(cap_t space) noexcept -> status_t {
    const auto requested = sys::io_space_close(space);
    if (requested.status != STATUS_OK) return requested.status;
    const auto done = sys::wait_one(space, (word_t{1} << IO_SPACE_CLOSED) | (word_t{1} << IO_SPACE_FAILED));
    return done.status != STATUS_OK ? done.status : done.value & (word_t{1} << IO_SPACE_FAILED)
        ? STATUS_BACKING_FAILED : STATUS_OK;
}

struct vm_area;

// One address allocator per VSpace. It outlives its areas; metadata
// is intrusive so the heap can acquire its first mapping without allocating.
struct vm_arena {
    cap_t space{};
    uintptr_t begin{}, end{};
    vm_area* head{};
    rt_mutex mutex;
    vm_arena() = default;
    vm_arena(cap_t space, uintptr_t begin, uintptr_t end) : space(space), begin(begin), end(end) {}
    vm_arena(const vm_arena&) = delete;
    auto operator=(const vm_arena&) -> vm_arena& = delete;
};
auto rt_vm() noexcept -> vm_arena&;

// Owns a local VA lease and only the mapping it actually installed. A sliced
// capability alone is neither a reservation nor authority to clear another map.
struct vm_area {
    cap_owner memory{}, region{};
    uintptr_t address{};
    size_t size{};
    vm_area() noexcept = default;
    vm_area(const vm_area&) = delete;
    auto operator=(const vm_area&) -> vm_area& = delete;
    vm_area(vm_area&& other) noexcept { take(other); }
    auto operator=(vm_area&& other) noexcept -> vm_area& {
        if (this != &other) { require_close(); take(other); }
        return *this;
    }
    ~vm_area() { require_close(); }

    auto open(vm_arena& arena, uintptr_t va, size_t bytes,
              word_t perms = VM_READ | VM_WRITE) noexcept -> status_t {
        if (region) return STATUS_BUSY;
        if (!arena.space || !bytes || (va | bytes) % 4096 || bytes > UINTPTR_MAX - va)
            return STATUS_BAD_ARGS;
        std::lock_guard lock{arena.mutex};
        auto** link = &arena.head;
        if (!va) {
            va = arena.begin;
            while (*link) {
                auto& area = **link;
                if (area.address >= va && bytes <= area.address - va) break;
                va = std::max(va, area.address + area.size);
                link = &area.next_;
            }
            if (va >= arena.end || bytes > arena.end - va) return STATUS_NO_MEMORY;
        } else {
            while (*link && (*link)->address < va) {
                if ((*link)->address + (*link)->size > va) return STATUS_BUSY;
                link = &(*link)->next_;
            }
            if (*link && bytes > (*link)->address - va) return STATUS_BUSY;
        }
        const auto result = sys::vm_slice(arena.space, va, bytes, perms,
            RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP);
        cap_owner slice{result.value};
        if (result.status != STATUS_OK) return result.status;
        if (!slice) return STATUS_INVALID_CAP;
        arena_ = &arena; region = std::move(slice); address = va; size = bytes;
        link_ = link; next_ = *link; *link = this;
        if (next_) next_->link_ = &next_;
        return STATUS_OK;
    }
    auto attach(cap_t mem, word_t first, size_t bytes, word_t perms, size_t offset = 0) noexcept -> status_t {
        if (!region || mapped_ || !mem || !bytes || bytes % 4096 || offset > size || bytes > size - offset || offset % 4096)
            return STATUS_BAD_ARGS;
        const auto status = vm_call(region.selector(), VM_OP_MAP, {mem, address + offset, bytes, first, perms});
        if (status == STATUS_OK) { mapped_ = bytes; mapped_offset_ = offset; }
        return status;
    }
    auto detach() noexcept -> status_t {
        if (!mapped_) return STATUS_OK;
        const auto status = vm_call(region.selector(), VM_OP_UNMAP, {0, address + mapped_offset_, mapped_, 0, 0});
        if (status == STATUS_OK) mapped_ = 0;
        return status;
    }
    auto close() noexcept -> status_t {
        if (const auto status = detach(); status != STATUS_OK) return status;
        if (const auto status = region.close(); status != STATUS_OK) return status;
        if (const auto status = memory.close(); status != STATUS_OK) return status;
        if (link_) {
            std::lock_guard lock{arena_->mutex};
            *link_ = next_;
            if (next_) next_->link_ = link_;
            link_ = nullptr; next_ = nullptr;
        }
        arena_ = nullptr; address = size = 0;
        return STATUS_OK;
    }
    static auto adopt(vm_arena& arena, cap_owner memory, uintptr_t va, size_t bytes) -> std::expected<vm_area, status_t> {
        vm_area area;
        const auto status = area.open(arena, va, bytes);
        if (status != STATUS_OK) return std::unexpected(status);
        area.memory = std::move(memory); area.mapped_ = bytes;
        return area;
    }
    static auto map(vm_arena& arena, cap_owner&& memory, uintptr_t va, size_t size, word_t perms)
        noexcept -> std::expected<vm_area, status_t> {
        vm_area area;
        area.memory = std::move(memory);
        auto status = area.open(arena, va, size, perms & ~VM_MAP_PRIVATE);
        if (status == STATUS_OK) status = area.attach(area.memory.selector(), 0, size, perms);
        if (status != STATUS_OK) return std::unexpected(status);
        return area;
    }
    static auto map_private(vm_arena& arena, cap_owner&& memory, uintptr_t va, size_t size)
        noexcept -> std::expected<vm_area, status_t> {
        return map(arena, std::move(memory), va, size, VM_READ | VM_WRITE | VM_MAP_PRIVATE);
    }
    static auto create(CreateAuth auth, vm_arena& arena, uintptr_t va, size_t size, word_t memory_perms, word_t map_perms)
        noexcept -> std::expected<vm_area, status_t> {
        const auto result = sys::memory_create(auth, size, memory_perms);
        cap_owner memory{result.value};
        if (result.status != STATUS_OK) return std::unexpected(result.status);
        return map(arena, std::move(memory), va, size, map_perms);
    }
    static auto create(CreateAuth auth, vm_arena& arena, uintptr_t va, size_t size, word_t perms = VM_READ | VM_WRITE)
        noexcept -> std::expected<vm_area, status_t> {
        return create(auth, arena, va, size, perms, perms);
    }
private:
    vm_arena* arena_{};
    vm_area** link_{};
    vm_area* next_{};
    size_t mapped_{}, mapped_offset_{};
    void require_close() noexcept { if (const auto status = close(); status != STATUS_OK) sys::exit(status); }
    void take(vm_area& other) noexcept {
        // Moves mutate the intrusive lease chain just like insert/erase.
        auto* arena = other.arena_;
        if (arena) arena->mutex.lock();
        arena_ = std::exchange(other.arena_, nullptr);
        memory = std::move(other.memory); region = std::move(other.region);
        link_ = std::exchange(other.link_, nullptr); next_ = std::exchange(other.next_, nullptr);
        if (link_) *link_ = this;
        if (next_) next_->link_ = &next_;
        address = std::exchange(other.address, 0); size = std::exchange(other.size, 0);
        mapped_ = std::exchange(other.mapped_, 0);
        mapped_offset_ = std::exchange(other.mapped_offset_, 0);
        if (arena) arena->mutex.unlock();
    }
};

// Native, explicitly funded object storage. The object dies before its mapping;
// the allocation's Quota and the provider-owned Scope remain independent.
template<class T> struct vm_delete {
    vm_area storage;
    void operator()(T* object) noexcept { std::destroy_at(object); storage = {}; }
};
template<class T> using vm_ptr = std::unique_ptr<T, vm_delete<T>>;
template<class T, class... Args>
auto vm_new(CreateAuth auth, Args&&... args) noexcept -> std::expected<vm_ptr<T>, status_t> {
    constexpr size_t bytes = (sizeof(T) + (alignof(T) > 4096 ? alignof(T) - 1 : 0) + 4095) & ~size_t{4095};
    auto storage = vm_area::create(auth, rt_vm(), 0, bytes);
    if (!storage) return std::unexpected(storage.error());
    const auto address = (storage->address + alignof(T) - 1) & ~uintptr_t{alignof(T) - 1};
    auto* object = std::construct_at(reinterpret_cast<T*>(address), std::forward<Args>(args)...);
    return vm_ptr<T>{object, vm_delete<T>{std::move(*storage)}};
}
