#pragma once

#include <expected>
#include <span>
#include <utility>
#include <sys/syscall.hpp>
#include <libk/unique_handle.hpp>


// Owns a slot in this task's CSpace. The child CSpace owns delivered slots;
// managers close a remote slot explicitly, while that CSpace is still alive.
class cap_owner {
    struct drop {
        void operator()(cap_t cap) const noexcept {
            const auto s = sys::cap_close(cap).status;
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
        const auto s = sys::cap_close(cap_.get()).status;
        if (s == STATUS_OK) (void)cap_.release();
        return s;
    }
};

// Owns a VA reservation; repeated attach/detach reuses that reservation.
// clear waits for the kernel's TLB/borrow drain before the VA is reusable.
// Memory follows its live caps, mappings and in-flight kernel references.
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
    ~vm_area() noexcept { require_close(); }

    auto open(cap_t vm, uintptr_t va, size_t bytes, word_t perms = VM_READ | VM_WRITE) noexcept -> status_t {
        if (region) return STATUS_BUSY;
        if (!vm || !bytes || (va | bytes) % 4096 || bytes > ~uintptr_t{} - va)
            return STATUS_BAD_ARGS;
        vm_area** link = nullptr;
        if (!va) {
            // The runtime owns this VA arena; fixed boot/device windows live below it.
            va = 0x100000000;
            link = &head_;
            while (*link) {
                const auto& m = **link;
                if (bytes <= m.address - va) break;
                va = m.address + m.size;
                link = &(*link)->next_;
            }
            if (va >= 0x2000000000 || bytes > 0x2000000000 - va) return STATUS_NO_MEMORY;
        }
        const auto r = sys::vm_slice(vm, va, bytes, perms,
            RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP | RIGHT_DESTROY);
        if (r.value) {
            region = cap_owner{r.value}; address = va; size = bytes; cleared_ = false;
            if (link) {
                next_ = *link; link_ = link; *link = this;
                if (next_) next_->link_ = &next_;
            }
        }
        return r.status == STATUS_OK && !region ? STATUS_INVALID_CAP : r.status;
    }
    auto attach(cap_t mem, word_t first, size_t bytes, word_t perms) noexcept -> status_t {
        if (!region || cleared_ || mapped_ || !mem || !bytes || bytes % 4096 || bytes > size)
            return STATUS_BAD_ARGS;
        const auto s = sys::vm_map(region.selector(), mem, address, bytes, first, perms).status;
        if (s == STATUS_OK) mapped_ = bytes;
        return s;
    }
    auto detach() noexcept -> status_t {
        if (!mapped_) return STATUS_OK;
        const auto s = sys::vm_unmap(region.selector(), address, mapped_).status;
        if (s == STATUS_OK) mapped_ = 0;
        return s;
    }
    auto close() noexcept -> status_t {
        if (region) {
            if (!cleared_) {
                const auto s = sys::vm_clear(region.selector()).status;
                if (s != STATUS_OK) return s;
                cleared_ = true;
                mapped_ = 0;
            }
            const auto s = region.close();
            if (s != STATUS_OK) return s;
        }
        const auto s = memory.close();
        if (s != STATUS_OK) return s;
        if (link_) {
            *link_ = next_;
            if (next_) next_->link_ = link_;
            link_ = nullptr; next_ = nullptr;
        }
        address = size = 0;
        return STATUS_OK;
    }
    static auto map(cap_t vm, cap_owner&& mem, uintptr_t va, size_t size, word_t perms)
        noexcept -> std::expected<vm_area, status_t> {
        return map_impl(vm, std::move(mem), va, size, perms);
    }
    static auto map_private(cap_t vm, cap_owner&& mem, uintptr_t va, size_t size)
        noexcept -> std::expected<vm_area, status_t> {
        return map_impl(vm, std::move(mem), va, size, VM_READ | VM_WRITE, VM_MAP_PRIVATE);
    }
    static auto create(cap_t pool, cap_t vm, uintptr_t va, size_t size)
        noexcept -> std::expected<vm_area, status_t> {
        const auto r = sys::memory_create(pool, size, VM_READ | VM_WRITE);
        if (r.status != STATUS_OK) return std::unexpected(r.status);
        return map_impl(vm, cap_owner{r.value}, va, size, VM_READ | VM_WRITE);
    }
private:
    inline static vm_area* head_{};
    vm_area** link_{};
    vm_area* next_{};
    size_t mapped_{};
    bool cleared_{};
    void require_close() noexcept { const auto s = close(); if (s != STATUS_OK) sys::exit(s); }
    void take(vm_area& b) noexcept {
        memory = std::move(b.memory); region = std::move(b.region);
        link_ = std::exchange(b.link_, nullptr); next_ = std::exchange(b.next_, nullptr);
        if (link_) *link_ = this;
        if (next_) next_->link_ = &next_;
        address = std::exchange(b.address, 0); size = std::exchange(b.size, 0);
        mapped_ = std::exchange(b.mapped_, 0); cleared_ = b.cleared_;
    }
    static auto map_impl(cap_t vm, cap_owner&& mem, uintptr_t va, size_t size,
                         word_t perms, word_t flags = 0)
        noexcept -> std::expected<vm_area, status_t> {
        vm_area result;
        result.memory = std::move(mem);
        auto s = result.open(vm, va, size, perms);
        if (s == STATUS_OK) s = result.attach(result.memory.selector(), 0, size, perms | flags);
        if (s != STATUS_OK) return std::unexpected(s);
        return result;
    }
};
