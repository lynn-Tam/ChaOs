#pragma once

#include <expected>
#include <utility>
#include <sys/syscall.hpp>
#include <libk/unique_handle.hpp>

namespace sys {

// Owns a slot in this task's CSpace. The child CSpace owns delivered slots;
// managers close a remote slot explicitly, while that CSpace is still alive.
class cap_owner {
    struct drop {
        void operator()(cap_t cap) const noexcept {
            const auto s = cap_close(cap).status;
            if (s != STATUS_OK) sys::exit(s);
        }
    };
    libk::unique_handle<cap_t, drop> cap_{};
public:
    cap_owner() noexcept = default;
    explicit cap_owner(cap_t cap) noexcept : cap_(cap) {}
    explicit operator bool() const noexcept { return bool(cap_); }
    auto selector() const noexcept -> cap_t { return cap_.get(); }
    auto release() noexcept -> cap_t { return cap_.release(); }
    auto close() noexcept -> status_t {
        if (!cap_) return STATUS_OK;
        const auto s = cap_close(cap_.get()).status;
        if (s == STATUS_OK) (void)cap_.release();
        return s;
    }
};

// Owns a VA reservation; repeated attach/detach reuses that reservation.
// clear waits for the kernel's TLB/borrow drain before the VA is reusable.
// create additionally owns the backing object; map owns an imported cap only.
struct mapping {
    cap_owner memory{}, region{};
    uintptr_t address{};
    size_t size{};

    mapping() noexcept = default;
    mapping(const mapping&) = delete;
    auto operator=(const mapping&) -> mapping& = delete;
    mapping(mapping&& other) noexcept { take(other); }
    auto operator=(mapping&& other) noexcept -> mapping& {
        if (this != &other) { require_close(); take(other); }
        return *this;
    }
    ~mapping() noexcept { require_close(); }

    auto open(cap_t vm, uintptr_t va, size_t bytes, word_t perms = VM_READ | VM_WRITE) noexcept -> status_t {
        if (region) return STATUS_BUSY;
        if (!vm || !va || !bytes || (va | bytes) % 4096 || bytes > ~uintptr_t{} - va)
            return STATUS_BAD_ARGS;
        const auto r = vm_slice(vm, va, bytes, perms,
            RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP | RIGHT_DESTROY);
        if (r.value) { region = cap_owner{r.value}; address = va; size = bytes; cleared_ = false; }
        return r.status == STATUS_OK && !region ? STATUS_INVALID_CAP : r.status;
    }
    auto attach(cap_t mem, word_t first, size_t bytes, word_t perms) noexcept -> status_t {
        if (!region || cleared_ || mapped_ || !mem || !bytes || bytes % 4096 || bytes > size)
            return STATUS_BAD_ARGS;
        const auto s = vm_map(region.selector(), mem, address, bytes, first, perms).status;
        if (s == STATUS_OK) mapped_ = bytes;
        return s;
    }
    auto detach() noexcept -> status_t {
        if (!mapped_) return STATUS_OK;
        const auto s = vm_unmap(region.selector(), address, mapped_).status;
        if (s == STATUS_OK) mapped_ = 0;
        return s;
    }
    auto close() noexcept -> status_t {
        if (region) {
            if (!cleared_) {
                const auto s = vm_clear(region.selector()).status;
                if (s != STATUS_OK) return s;
                cleared_ = true;
                mapped_ = 0;
            }
            const auto s = region.close();
            if (s != STATUS_OK) return s;
        }
        if (owns_memory_ && memory) {
            const auto s = object_destroy(memory.selector()).status;
            if (s != STATUS_OK) return s;
            owns_memory_ = false;
        }
        const auto s = memory.close();
        if (s != STATUS_OK) return s;
        address = size = 0;
        return STATUS_OK;
    }
    static auto map(cap_t vm, cap_owner&& mem, uintptr_t va, size_t size, word_t perms)
        noexcept -> std::expected<mapping, status_t> {
        return map_impl(vm, std::move(mem), va, size, perms, false);
    }
    static auto map_private(cap_t vm, cap_owner&& mem, uintptr_t va, size_t size)
        noexcept -> std::expected<mapping, status_t> {
        return map_impl(vm, std::move(mem), va, size, VM_READ | VM_WRITE, false, VM_MAP_PRIVATE);
    }
    static auto create(cap_t pool, cap_t vm, uintptr_t va, size_t size)
        noexcept -> std::expected<mapping, status_t> {
        const auto r = memory_create(pool, size, VM_READ | VM_WRITE);
        if (r.status != STATUS_OK) return std::unexpected(r.status);
        return map_impl(vm, cap_owner{r.value}, va, size, VM_READ | VM_WRITE, true);
    }
private:
    size_t mapped_{};
    bool cleared_{}, owns_memory_{};
    void require_close() noexcept { const auto s = close(); if (s != STATUS_OK) sys::exit(s); }
    void take(mapping& b) noexcept {
        memory = std::move(b.memory); region = std::move(b.region);
        address = std::exchange(b.address, 0); size = std::exchange(b.size, 0);
        mapped_ = std::exchange(b.mapped_, 0); cleared_ = b.cleared_;
        owns_memory_ = std::exchange(b.owns_memory_, false);
    }
    static auto map_impl(cap_t vm, cap_owner&& mem, uintptr_t va, size_t size,
                         word_t perms, bool owned, word_t flags = 0)
        noexcept -> std::expected<mapping, status_t> {
        mapping result;
        result.memory = std::move(mem); result.owns_memory_ = owned;
        auto s = result.open(vm, va, size, perms);
        if (s == STATUS_OK) s = result.attach(result.memory.selector(), 0, size, perms | flags);
        if (s != STATUS_OK) return std::unexpected(s);
        return result;
    }
};
} // namespace sys
