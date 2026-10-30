#pragma once
#include <ubus/ubus.hpp>
#include <vm.hpp>
#include <libk/pci.hpp>
#include <bit>

// Coherent DMA only. Ordering does not perform cache maintenance.
inline void vi_fence() noexcept {
#if defined(__riscv)
    asm volatile("fence iorw, iorw" ::: "memory");
#elif defined(__x86_64__)
    asm volatile("mfence" ::: "memory");
#else
#error Unsupported DMA architecture
#endif
}
template<class T> inline auto vi_load(uintptr_t p) noexcept -> T { return *reinterpret_cast<volatile const T*>(p); }
template<class T> inline void vi_store(uintptr_t p, T v) noexcept { *reinterpret_cast<volatile T*>(p) = v; }
template<class T> inline auto vi_read(uintptr_t p) noexcept -> T { vi_fence(); auto v = vi_load<T>(p); vi_fence(); return v; }
template<class T> inline void vi_write(uintptr_t p, T v) noexcept { vi_fence(); vi_store(p, v); vi_fence(); }

struct vi_dma {
    _vm_area memory;
    uint64_t address{};
};

class vi_dev {
    std::array<_vm_area, 7> regs_;
    std::array<size_t, 7> sizes_{};
    _cap_owner space_, irq_;
    _ev_watch irq_watch_, space_watch_;
    uintptr_t common_{}, notify_{}, isr_{};
    size_t notify_size_{};
    uint32_t multiplier_{};
public:
    uintptr_t config{};
    size_t config_size{};
    uint32_t features{};
    uint64_t irq_count{};
    auto open(std::span<const ubus_msg> setup, uint16_t id, uint32_t wanted = 0) -> status_t {
        const auto resource = [&](std::string_view name) -> std::pair<cap_t, ubus_value> {
            for (const auto& m : setup) if (auto v = m.data[name]) return {m.capability(v), v};
            return {};
        };
        const auto copy = [&](std::string_view name, word_t rights, _cap_owner& target) {
            auto c = _cap_duplicate(resource(name).first, _startup().cspace, rights);
            if (c.status == STATUS_OK) target = _cap_owner{c.value};
            return c.status;
        };
        auto status = copy("dma", RIGHT_MAP | RIGHT_UNMAP | RIGHT_INSPECT | RIGHT_OBSERVE | RIGHT_CLOSE, space_);
        if (status != STATUS_OK) return status;
        status = space_watch_.open(&_events(), space_.selector(), 0, 0, WATCH_CHANGED);
        if (status != STATUS_OK) return status;
        for (size_t n = 0; n < regs_.size(); ++n) {
            const auto [cap, value] = resource(n == 6 ? "pci.config" : "bar" + std::to_string(n));
            const auto b = value.as<ubus_buffer>();
            if (!b && n < 6) continue;
            if (!b || b->offset || !b->size || b->size > SIZE_MAX - 4095) return STATUS_BAD_ARGS;
            const auto c = _cap_duplicate(cap, _startup().cspace, RIGHT_MAP);
            if (c.status != STATUS_OK) return c.status;
            auto m = _vm_area::map(_vm(), _cap_owner{c.value}, 0, (b->size + 4095) & ~size_t{4095}, n == 6 ? VM_READ : VM_READ | VM_WRITE);
            if (!m) return m.error();
            regs_[n] = std::move(*m); sizes_[n] = b->size;
        }
        status = copy("irq", RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK, irq_);
        if (status == STATUS_OK) status = _irq_set(irq_.selector(), true).status;
        if (status == STATUS_OK) status = irq_watch_.open(&_events(), irq_.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_READ);
        if (status != STATUS_OK) return status;
        std::array<std::pair<uintptr_t, size_t>, 6> bars;
        for (size_t n = 0; n < bars.size(); ++n) bars[n] = {regs_[n].address, sizes_[n]};
        const pci::cfg<vi_fence> pci{regs_[6].address};
        const auto t = pci.virtio(bars);
        if (pci.read<uint32_t>(0) != (uint32_t{id} << 16 | 0x1af4) || !t) return STATUS_BACKING_FAILED;
        common_ = t->regs[0].first; notify_ = t->regs[1].first; notify_size_ = t->regs[1].second;
        isr_ = t->regs[2].first; config = t->regs[3].first; config_size = t->regs[3].second; multiplier_ = t->multiplier;
        if (vi_read<uint8_t>(common_ + 20)) return STATUS_BACKING_FAILED;
        vi_write<uint8_t>(common_ + 20, 1); vi_write<uint8_t>(common_ + 20, 3);
        vi_write<uint32_t>(common_, 1);
        if ((vi_read<uint32_t>(common_ + 4) & 3) != 3) return STATUS_BACKING_FAILED;
        vi_write<uint32_t>(common_, 0); features = vi_read<uint32_t>(common_ + 4) & wanted;
        vi_write<uint32_t>(common_ + 8, 0); vi_write<uint32_t>(common_ + 12, features);
        vi_write<uint32_t>(common_ + 8, 1); vi_write<uint32_t>(common_ + 12, 3);
        vi_write<uint8_t>(common_ + 20, 11);
        if (!(vi_read<uint8_t>(common_ + 20) & 8)) return STATUS_BACKING_FAILED;
        vi_write<uint16_t>(common_ + 16, 0xffff);
        return STATUS_OK;
    }
    auto ready() -> status_t {
        vi_write<uint8_t>(common_ + 20, 15);
        return vi_read<uint8_t>(common_ + 20) == 15 ? STATUS_OK : STATUS_BACKING_FAILED;
    }
    auto generation() const -> uint8_t { return vi_read<uint8_t>(common_ + 21); }
    auto queue_size(uint16_t index, uint16_t limit) -> uint16_t {
        vi_write<uint16_t>(common_ + 22, index);
        if (vi_read<uint16_t>(common_ + 28)) return 0;
        return std::bit_floor(std::min(vi_read<uint16_t>(common_ + 24), limit));
    }
    auto queue(uint16_t index, uint16_t size, uint64_t desc, uint64_t avail, uint64_t used) -> std::expected<uintptr_t, status_t> {
        vi_write<uint16_t>(common_ + 22, index);
        vi_write<uint16_t>(common_ + 24, size); vi_write<uint16_t>(common_ + 26, 0xffff);
        vi_write<uint64_t>(common_ + 32, desc); vi_write<uint64_t>(common_ + 40, avail); vi_write<uint64_t>(common_ + 48, used);
        const uint64_t offset = uint64_t{vi_read<uint16_t>(common_ + 30)} * multiplier_;
        if (notify_size_ < 2 || offset > notify_size_ - 2) return std::unexpected(STATUS_BACKING_FAILED);
        vi_write<uint16_t>(common_ + 28, 1);
        return notify_ + offset;
    }
    auto map(cap_t mem, uint64_t first, uint64_t pages, word_t perms) -> _sys_result {
        return _io_wait(_startup().alloc(), space_.selector(), IO_OP_MAP, {mem, first, pages, 0, perms});
    }
    auto unmap(uint64_t address, uint64_t pages) -> status_t {
        return _io_wait(_startup().alloc(), space_.selector(), IO_OP_UNMAP, {0, 0, pages, address, 0}).status;
    }
    auto allocate(size_t bytes, cap_t scope = 0) -> std::expected<vi_dma, status_t> {
        if (!bytes || bytes > SIZE_MAX - 4095) return std::unexpected(STATUS_BAD_ARGS);
        bytes = (bytes + 4095) & ~size_t{4095};
        auto resident = _vm(); resident.pager = 0;
        auto auth = _startup().alloc(); if (scope) auth.scope = scope;
        auto area = _vm_area::create(auth, resident, 0, bytes);
        if (!area) return std::unexpected(area.error());
        auto dma = map(area->memory.selector(), 0, bytes / 4096, VM_READ | VM_WRITE);
        if (dma.status != STATUS_OK) return std::unexpected(dma.status);
        return vi_dma{std::move(*area), dma.value};
    }
    // Call only after device references have ended. Failure retains storage.
    auto release(vi_dma& dma) -> status_t {
        if (!dma.address) return STATUS_OK;
        auto s = unmap(dma.address, dma.memory.size / 4096);
        if (s == STATUS_OK) { dma.address = 0; s = dma.memory.close(); }
        return s;
    }
    auto close() -> status_t { return _io_close_wait(space_.selector()); }
    auto acknowledge() -> status_t {
        const auto s = _io_space_state(space_.selector());
        if (s.status != STATUS_OK || s.value != IO_SPACE_READY) return s.status != STATUS_OK ? s.status : STATUS_BACKING_FAILED;
        const auto f = _source_info(space_.selector());
        if (f.status != STATUS_OK) return f.status;
        auto status = space_watch_.arm(f.value2).status; if (status != STATUS_OK) return status;
        const auto irq = _irq_observe(irq_.selector());
        if (irq.status == STATUS_BUSY) return irq_watch_.arm().status;
        if (irq.status != STATUS_OK) return irq.status;
        (void)vi_read<uint8_t>(isr_);
        status = _irq_ack(irq_.selector(), irq.value2, irq.value).status;
        if (status == STATUS_OK) ++irq_count;
        return status == STATUS_OK ? irq_watch_.arm().status : status;
    }
};

// One event loop owns a queue. A cookie's storage survives until completion;
// only used heads free descriptors. No device-specific request layout lives here.
class vi_queue {
    struct slot { uint64_t cookie{}; uint32_t writable{}; uint16_t count{}, next{}; };
    std::array<slot, 256> slots_{};
    vi_dma arena_;
    uint16_t size_{}, free_{}, head_{}, available_{}, published_{}, used_{}, index_{};
    size_t active_{}, avail_{}, used_offset_{};
    uintptr_t notify_{};
public:
    struct part { uint64_t address; uint32_t size; bool write; };
    struct completion { uint64_t cookie{}; uint32_t size{}; };
    auto active() const -> size_t { return active_; }
    auto size() const -> uint16_t { return size_; }
    auto open(vi_dev& dev, uint16_t index, uint16_t limit) -> status_t {
        size_ = dev.queue_size(index, std::min<uint16_t>(limit, slots_.size()));
        if (!size_) return STATUS_BACKING_FAILED;
        index_ = index; free_ = size_;
        for (uint16_t n = 0; n < size_; ++n) slots_[n].next = n + 1;
        avail_ = size_ * 16;
        used_offset_ = (avail_ + 6 + size_ * 2 + 4095) & ~size_t{4095};
        auto arena = dev.allocate(used_offset_ + 6 + size_ * 8);
        if (!arena) return arena.error();
        arena_ = std::move(*arena);
        const auto notify = dev.queue(index, size_, arena_.address, arena_.address + avail_, arena_.address + used_offset_);
        if (!notify) return notify.error();
        notify_ = *notify; return STATUS_OK;
    }
    auto submit(uint64_t cookie, std::span<const part> parts) -> status_t {
        if (parts.empty() || parts.size() > free_) return STATUS_BUSY;
        uint64_t writable{};
        for (auto p : parts) { if (!p.size || p.address > UINT64_MAX - p.size) return STATUS_BAD_ARGS; if (p.write) writable += p.size; }
        if (writable > UINT32_MAX) return STATUS_BAD_ARGS;
        const auto first = head_; auto n = head_;
        for (size_t i = 0; i < parts.size(); ++i) {
            const auto next = slots_[n].next;
            const auto p = arena_.memory.address + n * 16;
            vi_store<uint64_t>(p, parts[i].address); vi_store<uint32_t>(p + 8, parts[i].size);
            vi_store<uint16_t>(p + 12, (parts[i].write ? 2 : 0) | (i + 1 < parts.size() ? 1 : 0));
            vi_store<uint16_t>(p + 14, next); n = next;
        }
        head_ = n; free_ -= parts.size();
        slots_[first].cookie = cookie; slots_[first].count = parts.size(); slots_[first].writable = writable;
        vi_store<uint16_t>(arena_.memory.address + avail_ + 4 + (available_ % size_) * 2, first);
        ++available_; ++active_; return STATUS_OK;
    }
    void publish() {
        if (published_ == available_) return;
        vi_fence(); vi_store<uint16_t>(arena_.memory.address + avail_ + 2, available_);
        vi_write<uint16_t>(notify_, index_); published_ = available_;
    }
    auto take(completion& out) -> status_t {
        const auto count = uint16_t(vi_load<uint16_t>(arena_.memory.address + used_offset_ + 2) - used_);
        if (!count) return STATUS_WOULD_BLOCK;
        if (count > active_) return STATUS_BACKING_FAILED;
        vi_fence();
        const auto p = arena_.memory.address + used_offset_ + 4 + (used_ % size_) * 8;
        const auto first = vi_load<uint32_t>(p), written = vi_load<uint32_t>(p + 4);
        if (first >= size_ || !slots_[first].count || written > slots_[first].writable) return STATUS_BACKING_FAILED;
        auto& s = slots_[first]; out = {s.cookie, written};
        auto last = uint16_t(first);
        for (uint16_t n = 1; n < s.count; ++n) last = slots_[last].next;
        slots_[last].next = head_; head_ = first; free_ += s.count; s.count = 0;
        ++used_; --active_; return STATUS_OK;
    }
};
