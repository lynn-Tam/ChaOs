#pragma once

#include <utility>


#include <optional>
#include <sys/queue.hpp>
#include <sys/handle.hpp>
#include <servers/runtime/service.hpp>

namespace block {

// One execution owns the split virtqueue. Slots own their descriptor chain
// and DMA data until used-ring completion; the caller copies returned data
// before submitting another request in that slot. IOSpace owns device drain.
class Device final : private libk::noncopyable_nonmovable {
public:
    static constexpr size_t Depth = sys::io::QueueDepth;
    static constexpr size_t BlockSize = 512;
    static constexpr size_t MaxTransfer = 4096;
    struct Completion final {
        sys::io::Ticket ticket{};
        status_t status{};
        size_t size{};
        const uint8_t* data{};
    };

    [[nodiscard]] auto open(cap_t pool, cap_t vspace,
        cap_t device, cap_t events) noexcept -> status_t {
        using namespace sys;
        const auto created = io_space_create(pool);
        if (created.status != STATUS_OK) return created.status;
        space_ = cap::OwnedCap{{created.value, 0}};
        auto arena = MappedMemory::create(pool, vspace, ArenaAddress, ArenaSize);
        if (!arena) return arena.error();
        arena_ = std::move(*arena);
        const auto watched = io_space_watch(created.value, events, service::EventsBadge);
        if (watched.status != STATUS_OK) return watched.status;
        const auto bound = io_space_bind(created.value, device, arena_.memory.selector(),
            0, ArenaSize / 4096, Iova);
        if (bound.status != STATUS_OK) return bound.status;
        for (;;) {
            const auto state = io_space_state(created.value);
            if (state.status != STATUS_OK) return state.status;
            if (state.value == IO_SPACE_ACTIVE) break;
            if (state.value != IO_SPACE_OPENING) return STATUS_BACKING_FAILED;
            yield();
        }
        auto result = io_space_info(created.value);
        if (result.status != STATUS_OK) return result.status;
        IoInfo info{};
        service::copy(&info, reinterpret_cast<const void*>(service::IpcAddress), sizeof(info));
        if (info.version != IO_INFO_VERSION || info.reserved != 0
            || info.configuration[0] != 0x1042'1af4) return STATUS_BAD_ARGS;
        for (size_t index = 0; index < 6; ++index) {
            if (info.bar_sizes[index] == 0) continue;
            if (info.bar_sizes[index] > BarStride) return STATUS_BAD_ARGS;
            const auto bar = io_space_bar(created.value, index);
            if (bar.status != STATUS_OK) return bar.status;
            const size_t bytes = (info.bar_sizes[index] + 4095) & ~size_t{4095};
            auto mapped = MappedMemory::map(vspace, cap::OwnedCap{{bar.value, 0}},
                BarsAddress + index * BarStride, bytes, VM_READ | VM_WRITE);
            if (!mapped) return mapped.error();
            bars_[index] = std::move(*mapped);
        }
        const auto interrupt = io_space_irq(created.value);
        if (interrupt.status != STATUS_OK) return interrupt.status;
        interrupt_ = cap::OwnedCap{{interrupt.value, 0}};
        result = irq_bind(interrupt.value, events, service::EventsBadge);
        if (result.status != STATUS_OK) return result.status;
        return configure(info) ? STATUS_OK : STATUS_BACKING_FAILED;
    }

    [[nodiscard]] auto capacity() const noexcept -> uint64_t { return sectors_ * BlockSize; }
    [[nodiscard]] auto active() const noexcept -> size_t { return active_; }

    // After close starts, BAR/IRQ views may be revoked. Do not submit,
    // observe or acknowledge again; only the IOSpace state remains usable.
    [[nodiscard]] auto close() noexcept -> status_t {
        const auto closed = sys::io_space_close(space_.selector());
        if (closed.status != STATUS_OK) return closed.status;
        for (;;) {
            const auto state = sys::io_space_state(space_.selector());
            if (state.status != STATUS_OK) return state.status;
            if (state.value == IO_SPACE_CLOSED) return STATUS_OK;
            if (state.value == IO_SPACE_FAILED
                || state.value == IO_SPACE_FAULTED) return STATUS_BACKING_FAILED;
            sys::yield();
        }
    }

    [[nodiscard]] auto submit(sys::io::Ticket ticket, sys::io::Operation operation,
        uint64_t offset, size_t size, const uint8_t* source = nullptr) noexcept
        -> status_t {
        if (ticket.slot >= Depth || slots_[ticket.slot].active) return STATUS_BUSY;
        if (operation == sys::io::Operation::Flush) {
            if (!flush_supported_) return STATUS_INVALID_OP;
            if (offset != 0 || size != 0 || source != nullptr) return STATUS_BAD_ARGS;
        } else if (operation == sys::io::Operation::Identify) {
            if (offset != 0 || size != 20 || source != nullptr) return STATUS_BAD_ARGS;
        } else {
            if (operation != sys::io::Operation::Read && operation != sys::io::Operation::Write)
                return STATUS_INVALID_OP;
            if (operation == sys::io::Operation::Write && (read_only_ || source == nullptr))
                return STATUS_DENIED;
            if (size == 0 || size > MaxTransfer || size % BlockSize != 0 || offset % BlockSize != 0
                || offset > capacity() || size > capacity() - offset) return STATUS_BAD_ARGS;
        }
        const size_t slot = ticket.slot;
        const uintptr_t header = ArenaAddress + Headers + slot * 32;
        const uint32_t type = operation == sys::io::Operation::Read ? 0
            : operation == sys::io::Operation::Write ? 1
            : operation == sys::io::Operation::Flush ? 4 : 8;
        dma_write<uint32_t>(header, type);
        dma_write<uint32_t>(header + 4, 0);
        dma_write<uint64_t>(header + 8, operation == sys::io::Operation::Read
            || operation == sys::io::Operation::Write ? offset / BlockSize : 0);
        dma_write<uint8_t>(header + 16, 0xff);
        const uint16_t head = static_cast<uint16_t>(slot * 3);
        descriptor(head, Iova + Headers + slot * 32, 16, 1, head + 1);
        if (operation == sys::io::Operation::Flush) {
            descriptor(head + 1, Iova + Headers + slot * 32 + 16, 1, 2, 0);
        } else {
            if (operation == sys::io::Operation::Write)
                sys::service::copy(reinterpret_cast<void*>(ArenaAddress + Data + slot * MaxTransfer), source, size);
            descriptor(head + 1, Iova + Data + slot * MaxTransfer, size,
                operation == sys::io::Operation::Write ? 1 : 3, head + 2);
            descriptor(head + 2, Iova + Headers + slot * 32 + 16, 1, 2, 0);
        }
        dma_write<uint16_t>(ArenaAddress + Available + 4 + (available_ % QueueSize) * 2, head);
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
        dma_write<uint16_t>(ArenaAddress + Available + 2, available_);
        write<uint16_t>(notify_, 0);
        published_ = available_;
    }

    [[nodiscard]] auto take(Completion& completion) noexcept -> status_t {
        const uint16_t published = dma_read<uint16_t>(ArenaAddress + Used + 2);
        const uint16_t count = static_cast<uint16_t>(published - used_);
        if (count == 0) return STATUS_WOULD_BLOCK;
        if (count > active_) return STATUS_BACKING_FAILED;
        asm volatile("fence r, rw" ::: "memory");
        const uintptr_t entry = ArenaAddress + Used + 4 + (used_ % QueueSize) * 8;
        const uint32_t head = dma_read<uint32_t>(entry);
        const uint32_t written = dma_read<uint32_t>(entry + 4);
        if (head % 3 != 0 || head / 3 >= Depth) return STATUS_BACKING_FAILED;
        const size_t index = head / 3;
        const auto& slot = slots_[index];
        if (!slot.active) return STATUS_BACKING_FAILED;
        const uint8_t status = dma_read<uint8_t>(ArenaAddress + Headers + index * 32 + 16);
        const bool returned_data = slot.operation == sys::io::Operation::Read
            || slot.operation == sys::io::Operation::Identify;
        const uint32_t expected = returned_data ? static_cast<uint32_t>(slot.size + 1) : 1;
        if (status > 2 || (status == 0 && written != expected)) return STATUS_BACKING_FAILED;
        completion = {slot.ticket, status == 0 ? STATUS_OK
            : status == 2 ? STATUS_INVALID_OP : STATUS_BACKING_FAILED,
            status == 0 ? slot.size : 0,
            returned_data
                ? reinterpret_cast<const uint8_t*>(ArenaAddress + Data + index * MaxTransfer) : nullptr};
        slots_[index] = {};
        --active_;
        ++used_;
        return STATUS_OK;
    }

    [[nodiscard]] auto acknowledge() noexcept -> status_t {
        const auto state = sys::io_space_state(space_.selector());
        if (state.status != STATUS_OK) return state.status;
        if (state.value != IO_SPACE_ACTIVE) return STATUS_BACKING_FAILED;
        const auto delivered = sys::irq_observe(interrupt_.selector());
        if (delivered.status == STATUS_BUSY) return STATUS_OK;
        if (delivered.status != STATUS_OK) return delivered.status;
        (void)read<uint8_t>(isr_); // clear the device's level before unmasking PLIC
        const auto result = sys::irq_ack(interrupt_.selector(), delivered.value2, delivered.value);
        return result.status;
    }

private:
    static constexpr uintptr_t ArenaAddress = 0x5000'0000;
    static constexpr uintptr_t BarsAddress = 0x6000'0000;
    static constexpr size_t BarStride = 0x10'0000;
    static constexpr uint64_t Iova = 0x1000;
    static constexpr uint16_t QueueSize = 128;
    static constexpr size_t Available = QueueSize * 16;
    static constexpr size_t Used = 4096;
    static constexpr size_t Headers = 8192;
    static constexpr size_t Data = 12288;
    static constexpr size_t ArenaSize = Data + Depth * MaxTransfer;
    static_assert(Depth * 3 <= QueueSize && Available + QueueSize * 2 + 6 <= Used);

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
        const uintptr_t target = ArenaAddress + index * 16;
        dma_write<uint64_t>(target, address);
        dma_write<uint32_t>(target + 8, size);
        dma_write<uint16_t>(target + 12, flags);
        dma_write<uint16_t>(target + 14, next);
    }

    auto configure(const IoInfo& info) noexcept -> bool {
        uintptr_t common{};
        uintptr_t device{};
        size_t notify_size{};
        uint32_t multiplier{};
        uint8_t cap = info.configuration[0x34 / 4];
        for (size_t count = 0; cap != 0 && count < 48; ++count) {
            if (cap < 0x40 || cap > 0xfc || cap % 4 != 0) return false;
            const uint32_t header = info.configuration[cap / 4];
            if ((header & 255) == 9) {
                const uint8_t type = header >> 24;
                if (type >= 1 && type <= 4) {
                    if (cap > 0xf0) return false;
                    const size_t bar = info.configuration[cap / 4 + 1] & 255;
                    const size_t offset = info.configuration[cap / 4 + 2];
                    const size_t size = info.configuration[cap / 4 + 3];
                    if (((header >> 16) & 255) < 16 || bar >= 6
                        || offset > info.bar_sizes[bar] || size > info.bar_sizes[bar] - offset)
                        return false;
                    const uintptr_t address = bars_[bar].address + offset;
                    if (type == 1) {
                        if (common != 0 || size < 56) return false;
                        common = address;
                    } else if (type == 2) {
                        if (notify_ != 0 || cap > 0xec || ((header >> 16) & 255) < 20) return false;
                        notify_ = address;
                        notify_size = size;
                        multiplier = info.configuration[cap / 4 + 4];
                    } else if (type == 3) {
                        if (isr_ != 0 || size < 1) return false;
                        isr_ = address;
                    } else {
                        if (device != 0 || size < 8) return false;
                        device = address;
                    }
                }
            }
            cap = header >> 8;
        }
        if (cap != 0 || common == 0 || notify_ == 0 || isr_ == 0 || device == 0
            || read<uint8_t>(common + 20) != 0) return false;
        write<uint8_t>(common + 20, 1);
        write<uint8_t>(common + 20, 3);
        write<uint32_t>(common, 1);
        if ((read<uint32_t>(common + 4) & 3) != 3) return false;
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
        if ((read<uint8_t>(common + 20) & 8) == 0) return false;
        bool stable{};
        for (size_t count = 0; count < 8; ++count) {
            const auto generation = read<uint8_t>(common + 21);
            sectors_ = read<uint64_t>(device);
            if (generation == read<uint8_t>(common + 21)) { stable = true; break; }
        }
        if (!stable || sectors_ == 0 || sectors_ > UINT64_MAX / BlockSize) return false;
        write<uint16_t>(common + 16, 0xffff); // disable MSI-X configuration vector
        write<uint16_t>(common + 22, 0);
        if (read<uint16_t>(common + 24) < QueueSize || read<uint16_t>(common + 28) != 0) return false;
        write<uint16_t>(common + 24, QueueSize);
        write<uint16_t>(common + 26, 0xffff);
        write<uint64_t>(common + 32, Iova);
        write<uint64_t>(common + 40, Iova + Available);
        write<uint64_t>(common + 48, Iova + Used);
        const size_t offset = size_t{read<uint16_t>(common + 30)} * multiplier;
        if (notify_size < 2 || offset > notify_size - 2) return false;
        notify_ += offset;
        write<uint16_t>(common + 28, 1);
        write<uint8_t>(common + 20, 15);
        return true;
    }

    struct Slot final {
        sys::io::Ticket ticket{};
        size_t size{};
        sys::io::Operation operation{sys::io::Operation::Read};
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
    sys::cap::OwnedCap space_{};
    sys::cap::OwnedCap interrupt_{};
    sys::MappedMemory arena_{};
    sys::MappedMemory bars_[6]{};
};

} // namespace block
