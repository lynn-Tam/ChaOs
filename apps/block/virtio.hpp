#pragma once
#include <io.hpp>
#include <libk/noncopyable.hpp>
#include <vm.hpp>
#include <string.h>

#include <utility>


#include <optional>
#include <ubus/ubus.hpp>
#include <cap.hpp>
#include <ipc.hpp>

#include <libk/pci.hpp>
#include <bit>

enum class blk_op { Read = 1, Write, Flush, Identify };

inline void blk_fence() noexcept { asm volatile("fence iorw, iorw" ::: "memory"); }

// One execution owns the split virtqueue. Slots own their descriptor chain
// and DMA data until used-ring completion; the caller copies returned data
// before submitting another request in that slot. IOSpace owns device drain.
class blk_dev final : private libk::noncopyable_nonmovable {
public:
    static constexpr size_t Depth = io_depth;
    static constexpr size_t BlockSize = 512;
    static constexpr size_t MaxTransfer = io_limit;
    struct Completion final {
        ubus_ticket ticket{};
        status_t status{};
        size_t size{};
        const uint8_t* data{};
    };

    [[nodiscard]] auto open(const _env& info, std::span<const ubus_msg> setup) noexcept -> status_t {
        const auto resource = [&](std::string_view name) -> std::pair<cap_t, ubus_value> {
            for (const auto& message : setup) if (const auto value = message.data[name]) return {message.capability(value), value};
            return {};
        };
        const auto space = _cap_duplicate(resource("dma").first, info.cspace,
            RIGHT_MAP | RIGHT_UNMAP | RIGHT_INSPECT | RIGHT_OBSERVE | RIGHT_CLOSE);
        if (space.status != STATUS_OK) return space.status;
        space_ = _cap_owner{space.value};
        const auto watched = space_watch_.open(&_events(), space.value, 0, 0, WATCH_CHANGED);
        if (watched != STATUS_OK) return watched;
        events_ = &_events();
        for (size_t i = 0; i < std::size(regs_); ++i) {
            const auto name = i == 6 ? std::string{"pci.config"} : "bar" + std::to_string(i);
            const auto [cap, value] = resource(name);
            const auto buffer = value.as<ubus_buffer>();
            if (!buffer && i < 6) continue;
            if (!buffer || buffer->offset || !buffer->size || buffer->size > SIZE_MAX - 4095) return STATUS_BAD_ARGS;
            const auto reg = _cap_duplicate(cap, info.cspace, RIGHT_MAP);
            if (reg.status != STATUS_OK) return reg.status;
            sizes_[i] = buffer->size;
            auto mapped = _vm_area::map(_vm(), _cap_owner{reg.value}, 0,
                (buffer->size + 4095) & ~size_t{4095}, i == 6 ? VM_READ : VM_READ | VM_WRITE);
            if (!mapped) return mapped.error();
            regs_[i] = std::move(*mapped);
        }
        const auto interrupt = _cap_duplicate(resource("irq").first, info.cspace, RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK);
        if (interrupt.status != STATUS_OK) return interrupt.status;
        interrupt_ = _cap_owner{interrupt.value};
        const auto bound = _irq_set(interrupt.value, true);
        if (bound.status != STATUS_OK) return bound.status;
        const auto interrupt_watch = irq_watch_.open(events_, interrupt.value, SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_READ);
        if (interrupt_watch != STATUS_OK) return interrupt_watch;
        return configure(info);
    }

    [[nodiscard]] auto capacity() const noexcept -> uint64_t { return sectors_ * BlockSize; }
    [[nodiscard]] auto active() const noexcept -> size_t { return active_; }
    [[nodiscard]] auto depth() const noexcept -> size_t { return depth_; }

    // After close starts, BAR/IRQ views may be revoked. Do not submit,
    // observe or acknowledge again; only the IOSpace state remains usable.
    [[nodiscard]] auto close() noexcept -> status_t {
        return _io_close_wait(space_.selector());
    }

    [[nodiscard]] auto submit(ubus_ticket ticket, blk_op operation,
        uint64_t offset, size_t size, const uint8_t* source = nullptr) noexcept
        -> status_t {
        if (ticket.slot >= depth_ || slots_[ticket.slot].active) return STATUS_BUSY;
        if (operation == blk_op::Flush) {
            if (!flush_supported_) return STATUS_INVALID_OP;
            if (offset != 0 || size != 0 || source != nullptr) return STATUS_BAD_ARGS;
        } else if (operation == blk_op::Identify) {
            if (offset != 0 || size != 20 || source != nullptr) return STATUS_BAD_ARGS;
        } else {
            if (operation != blk_op::Read && operation != blk_op::Write)
                return STATUS_INVALID_OP;
            if (operation == blk_op::Write && (read_only_ || source == nullptr))
                return STATUS_DENIED;
            if (size == 0 || size > MaxTransfer || size % BlockSize != 0 || offset % BlockSize != 0
                || offset > capacity() || size > capacity() - offset) return STATUS_BAD_ARGS;
        }
        const size_t slot = ticket.slot;
        const uintptr_t header = arena_.address + headers_ + slot * 32;
        const uint32_t type = operation == blk_op::Read ? 0
            : operation == blk_op::Write ? 1
            : operation == blk_op::Flush ? 4 : 8;
        dma_write<uint32_t>(header, type);
        dma_write<uint32_t>(header + 4, 0);
        dma_write<uint64_t>(header + 8, operation == blk_op::Read
            || operation == blk_op::Write ? offset / BlockSize : 0);
        dma_write<uint8_t>(header + 16, 0xff);
        const uint16_t head = static_cast<uint16_t>(slot * 3);
        descriptor(head, iova_ + headers_ + slot * 32, 16, 1, head + 1);
        if (operation == blk_op::Flush) {
            descriptor(head + 1, iova_ + headers_ + slot * 32 + 16, 1, 2, 0);
        } else {
            if (operation == blk_op::Write)
                ::memcpy(reinterpret_cast<void*>(arena_.address + data_ + slot * MaxTransfer), source, size);
            descriptor(head + 1, iova_ + data_ + slot * MaxTransfer, size,
                operation == blk_op::Write ? 1 : 3, head + 2);
            descriptor(head + 2, iova_ + headers_ + slot * 32 + 16, 1, 2, 0);
        }
        dma_write<uint16_t>(arena_.address + available_offset_ + 4 + (available_ % queue_size_) * 2, head);
        ++available_;
        slots_[slot] = {ticket, size, operation, true};
        ++active_;
        return STATUS_OK;
    }

    void publish() noexcept {
        if (published_ == available_) return;
        // Publish the complete descriptor batch before making its index and
        // doorbell visible to the device. No per-descriptor MMIO is needed.
        asm volatile("fence rw, rw" ::: "memory");
        dma_write<uint16_t>(arena_.address + available_offset_ + 2, available_);
        write<uint16_t>(notify_, 0);
        published_ = available_;
    }

    [[nodiscard]] auto take(Completion& completion) noexcept -> status_t {
        const uint16_t published = dma_read<uint16_t>(arena_.address + used_offset_ + 2);
        const uint16_t count = static_cast<uint16_t>(published - used_);
        if (count == 0) return STATUS_WOULD_BLOCK;
        if (count > active_) return STATUS_BACKING_FAILED;
        asm volatile("fence r, rw" ::: "memory");
        const uintptr_t entry = arena_.address + used_offset_ + 4 + (used_ % queue_size_) * 8;
        const uint32_t head = dma_read<uint32_t>(entry);
        const uint32_t written = dma_read<uint32_t>(entry + 4);
        if (head % 3 != 0 || head / 3 >= Depth) return STATUS_BACKING_FAILED;
        const size_t index = head / 3;
        const auto& slot = slots_[index];
        if (!slot.active) return STATUS_BACKING_FAILED;
        const uint8_t status = dma_read<uint8_t>(arena_.address + headers_ + index * 32 + 16);
        const bool returned_data = slot.operation == blk_op::Read
            || slot.operation == blk_op::Identify;
        const uint32_t expected = returned_data ? static_cast<uint32_t>(slot.size + 1) : 1;
        if (status > 2 || (status == 0 && written != expected)) return STATUS_BACKING_FAILED;
        completion = {slot.ticket, status == 0 ? STATUS_OK
            : status == 2 ? STATUS_INVALID_OP : STATUS_BACKING_FAILED,
            status == 0 ? slot.size : 0,
            returned_data
                ? reinterpret_cast<const uint8_t*>(arena_.address + data_ + index * MaxTransfer) : nullptr};
        slots_[index] = {};
        --active_;
        ++used_;
        return STATUS_OK;
    }

    [[nodiscard]] auto acknowledge() noexcept -> status_t {
        const auto state = _io_space_state(space_.selector());
        if (state.status != STATUS_OK) return state.status;
        if (state.value != IO_SPACE_READY) return STATUS_BACKING_FAILED;
        const auto fact = _source_info(space_.selector());
        if (fact.status != STATUS_OK) return fact.status;
        if (const auto status = space_watch_.arm(fact.value2).status; status != STATUS_OK) return status;
        const auto delivered = _irq_observe(interrupt_.selector());
        if (delivered.status == STATUS_BUSY) return irq_watch_.arm().status;
        if (delivered.status != STATUS_OK) return delivered.status;
        (void)read<uint8_t>(isr_); // clear the device's level before unmasking PLIC
        const auto result = _irq_ack(interrupt_.selector(), delivered.value2, delivered.value);
        return result.status == STATUS_OK ? irq_watch_.arm().status : result.status;
    }

private:
    _ev_port* events_{};
    _ev_watch irq_watch_, space_watch_;
    size_t depth_{}, available_offset_{}, used_offset_{}, headers_{}, data_{};
    uint16_t queue_size_{};
    uint64_t iova_{};
    std::array<size_t, 7> sizes_{};

    template<typename T> static auto dma_read(uintptr_t address) noexcept -> T {
        return *reinterpret_cast<volatile const T*>(address);
    }
    template<typename T> static void dma_write(uintptr_t address, T value) noexcept {
        *reinterpret_cast<volatile T*>(address) = value;
    }
    template<typename T> static auto read(uintptr_t address) noexcept -> T {
        asm volatile("fence iorw, iorw" ::: "memory");
        const T value = dma_read<T>(address);
        asm volatile("fence iorw, iorw" ::: "memory");
        return value;
    }
    template<typename T> static void write(uintptr_t address, T value) noexcept {
        asm volatile("fence iorw, iorw" ::: "memory");
        dma_write(address, value);
        asm volatile("fence iorw, iorw" ::: "memory");
    }
    void descriptor(uint16_t index, uint64_t address, uint32_t size,
        uint16_t flags, uint16_t next) noexcept {
        const uintptr_t target = arena_.address + index * 16;
        dma_write<uint64_t>(target, address);
        dma_write<uint32_t>(target + 8, size);
        dma_write<uint16_t>(target + 12, flags);
        dma_write<uint16_t>(target + 14, next);
    }

    auto configure(const _env& info) noexcept -> status_t {
        std::array<std::pair<uintptr_t, size_t>, 6> bars;
        for (size_t i = 0; i < bars.size(); ++i) bars[i] = {regs_[i].address, sizes_[i]};
        const pci::cfg<blk_fence> cfg{regs_[6].address};
        const auto transport = cfg.virtio(bars);
        if (cfg.read<uint32_t>(0) != 0x1042'1af4 || !transport) return STATUS_BACKING_FAILED;
        const auto common = transport->regs[0].first, device = transport->regs[3].first;
        notify_ = transport->regs[1].first;
        isr_ = transport->regs[2].first;
        const auto notify_size = transport->regs[1].second;
        const auto multiplier = transport->multiplier;
        if (read<uint8_t>(common + 20) != 0) return STATUS_BACKING_FAILED;
        write<uint8_t>(common + 20, 1);
        write<uint8_t>(common + 20, 3);
        write<uint32_t>(common, 1);
        if ((read<uint32_t>(common + 4) & 3) != 3) return STATUS_BACKING_FAILED;
        write<uint32_t>(common, 0);
        const uint32_t offered = read<uint32_t>(common + 4);
        const uint32_t low = offered & ((1U << 5) | (1U << 9));
        read_only_ = (offered & (1U << 5)) != 0;
        flush_supported_ = (offered & (1U << 9)) != 0;
        write<uint32_t>(common + 8, 0);
        write<uint32_t>(common + 12, low);
        write<uint32_t>(common + 8, 1);
        write<uint32_t>(common + 12, 3); // VERSION_1 and ACCESS_PLATFORM
        write<uint8_t>(common + 20, 11);
        if ((read<uint8_t>(common + 20) & 8) == 0) return STATUS_BACKING_FAILED;
        bool stable{};
        for (size_t count = 0; count < 8; ++count) {
            const auto generation = read<uint8_t>(common + 21);
            sectors_ = read<uint64_t>(device);
            if (generation == read<uint8_t>(common + 21)) { stable = true; break; }
        }
        if (!stable || sectors_ == 0 || sectors_ > UINT64_MAX / BlockSize) return STATUS_BACKING_FAILED;
        write<uint16_t>(common + 16, 0xffff); // disable MSI-X configuration vector
        write<uint16_t>(common + 22, 0);
        if (read<uint16_t>(common + 28) != 0) return STATUS_BACKING_FAILED;
        queue_size_ = std::bit_floor(std::min<uint16_t>(read<uint16_t>(common + 24), std::bit_ceil(uint16_t{Depth * 3})));
        depth_ = std::min<size_t>(Depth, queue_size_ / 3);
        if (!depth_) return STATUS_BACKING_FAILED;
        available_offset_ = queue_size_ * 16;
        used_offset_ = (available_offset_ + 6 + queue_size_ * 2 + 4095) & ~size_t{4095};
        headers_ = (used_offset_ + 6 + queue_size_ * 8 + 4095) & ~size_t{4095};
        data_ = (headers_ + depth_ * 32 + 4095) & ~size_t{4095};
        const auto bytes = data_ + depth_ * MaxTransfer;
        auto arena = _vm_area::create(info.alloc(), _vm(), 0, bytes);
        if (!arena) return arena.error();
        arena_ = std::move(*arena);
        const auto mapping = _io_wait(info.alloc(), space_.selector(), IO_OP_MAP,
            {arena_.memory.selector(), 0, bytes / 4096, 0, VM_READ | VM_WRITE});
        if (mapping.status != STATUS_OK) return mapping.status;
        iova_ = mapping.value;
        write<uint16_t>(common + 24, queue_size_);
        write<uint16_t>(common + 26, 0xffff);
        write<uint64_t>(common + 32, iova_);
        write<uint64_t>(common + 40, iova_ + available_offset_);
        write<uint64_t>(common + 48, iova_ + used_offset_);
        const size_t offset = size_t{read<uint16_t>(common + 30)} * multiplier;
        if (notify_size < 2 || offset > notify_size - 2) return STATUS_BACKING_FAILED;
        notify_ += offset;
        write<uint16_t>(common + 28, 1);
        write<uint8_t>(common + 20, 15);
        return STATUS_OK;
    }

    struct Slot final {
        ubus_ticket ticket{};
        size_t size{};
        blk_op operation{blk_op::Read};
        bool active{};
    };
    Slot slots_[Depth]{};
    size_t active_{};
    uint16_t available_{};
    uint16_t published_{};
    uint16_t used_{};
    uint64_t sectors_{};
    bool read_only_{};
    bool flush_supported_{};
    uintptr_t notify_{};
    uintptr_t isr_{};
    _cap_owner space_{};
    _cap_owner interrupt_{};
    _vm_area arena_{};
    _vm_area regs_[7]{};
};
