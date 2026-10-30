#pragma once
#include <io.hpp>
#include <virtio.hpp>

enum class blk_op { Read = 1, Write, Flush, Identify };

class blk_dev {
    vi_dev dev_;
    vi_queue queue_;
    vi_dma arena_;
    struct slot {
        ubus_ticket ticket{};
        size_t size{};
        blk_op op{};
        bool active{}, direct{};
    };
    std::array<slot, io_depth> slots_{};
    uint64_t sectors_{};
    size_t depth_{};
public:
    static constexpr size_t Depth = io_depth, BlockSize = 512, MaxTransfer = io_limit;
    struct Completion { ubus_ticket ticket{}; status_t status{}; size_t size{}; const uint8_t* data{}; };
    auto open(const _env&, std::span<const ubus_msg> setup) -> status_t {
        auto s = dev_.open(setup, 0x1042, (1U << 5) | (1U << 9));
        if (s != STATUS_OK) return s;
        if (dev_.config_size < 8) return STATUS_BACKING_FAILED;
        bool stable{};
        for (unsigned n = 0; n < 8; ++n) {
            auto gen = dev_.generation(); sectors_ = vi_read<uint64_t>(dev_.config);
            if (gen == dev_.generation()) { stable = true; break; }
        }
        if (!stable || !sectors_ || sectors_ > UINT64_MAX / BlockSize) return STATUS_BACKING_FAILED;
        s = queue_.open(dev_, 0, std::bit_ceil(uint16_t{Depth * 3}));
        if (s != STATUS_OK) return s;
        depth_ = std::min<size_t>(Depth, queue_.size() / 3);
        if (!depth_) return STATUS_BACKING_FAILED;
        auto arena = dev_.allocate(4096 + depth_ * MaxTransfer);
        if (!arena) return arena.error();
        arena_ = std::move(*arena);
        return dev_.ready();
    }
    auto capacity() const -> uint64_t { return sectors_ * BlockSize; }
    auto depth() const -> size_t { return depth_; }
    auto active() const -> size_t { return queue_.active(); }
    auto map(cap_t m, uint64_t first, uint64_t count, word_t perms) -> _sys_result { return dev_.map(m, first, count, perms); }
    auto unmap(uint64_t a, uint64_t count) -> status_t { return dev_.unmap(a, count); }
    auto close() -> status_t { return dev_.close(); }
    auto acknowledge() -> status_t { return dev_.acknowledge(); }
    void publish() { queue_.publish(); }
    auto submit(ubus_ticket ticket, blk_op op, uint64_t offset, size_t size, const uint8_t* source = nullptr, uint64_t direct = 0) -> status_t {
        if (ticket.slot >= depth_ || slots_[ticket.slot].active) return STATUS_BUSY;
        if (op == blk_op::Flush) {
            if (!(dev_.features & (1U << 9))) return STATUS_INVALID_OP;
            if (offset || size || source) return STATUS_BAD_ARGS;
        } else if (op == blk_op::Identify) {
            if (offset || size != 20 || source) return STATUS_BAD_ARGS;
        } else {
            if (op != blk_op::Read && op != blk_op::Write) return STATUS_INVALID_OP;
            if (op == blk_op::Write && ((dev_.features & (1U << 5)) || (!source && !direct))) return STATUS_DENIED;
            if (!size || size > MaxTransfer || (offset | size) % BlockSize || offset > capacity() || size > capacity() - offset) return STATUS_BAD_ARGS;
        }
        const auto n = ticket.slot;
        const auto p = arena_.memory.address + n * 32;
        const auto header = arena_.address + n * 32;
        const auto data = arena_.address + 4096 + n * MaxTransfer;
        const auto type = op == blk_op::Read ? 0U : op == blk_op::Write ? 1U : op == blk_op::Flush ? 4U : 8U;
        vi_store<uint32_t>(p, type); vi_store<uint32_t>(p + 4, 0);
        vi_store<uint64_t>(p + 8, op == blk_op::Read || op == blk_op::Write ? offset / BlockSize : 0);
        vi_store<uint8_t>(p + 16, 0xff);
        if (op == blk_op::Write && !direct) ::memcpy(reinterpret_cast<void*>(arena_.memory.address + 4096 + n * MaxTransfer), source, size);
        std::array<vi_queue::part, 3> parts{{{header, 16, false}, {direct ? direct : data, uint32_t(size), op != blk_op::Write}, {header + 16, 1, true}}};
        if (op == blk_op::Flush) parts[1] = parts[2];
        const auto s = queue_.submit(n, std::span{parts}.first(op == blk_op::Flush ? 2 : 3));
        if (s == STATUS_OK) slots_[n] = {ticket, size, op, true, direct != 0};
        return s;
    }
    auto take(Completion& out) -> status_t {
        vi_queue::completion c;
        const auto s = queue_.take(c); if (s != STATUS_OK) return s;
        if (c.cookie >= depth_) return STATUS_BACKING_FAILED;
        auto& slot = slots_[c.cookie];
        if (!slot.active) return STATUS_BACKING_FAILED;
        const auto status = vi_load<uint8_t>(arena_.memory.address + c.cookie * 32 + 16);
        const bool data = slot.op == blk_op::Read || slot.op == blk_op::Identify;
        if (status > 2 || (!status && c.size != (data ? slot.size + 1 : 1))) return STATUS_BACKING_FAILED;
        out = {slot.ticket, status == 0 ? STATUS_OK : status == 2 ? STATUS_INVALID_OP : STATUS_BACKING_FAILED,
            status == 0 ? slot.size : 0, data && !slot.direct ? reinterpret_cast<const uint8_t*>(arena_.memory.address + 4096 + c.cookie * MaxTransfer) : nullptr};
        slot = {}; return STATUS_OK;
    }
};
