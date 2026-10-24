#pragma once

#include <utility>
#include <array>
#include <algorithm>
#include <span>
#include <expected>
#include <start.hpp>
#include <string.h>
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

inline auto cap_info(cap_t cap) -> std::expected<CapInfo, status_t> {
    const auto status = sys::cap_info(cap).status;
    if (status != STATUS_OK) return std::unexpected(status);
    CapInfo info; ::memcpy(&info, reinterpret_cast<const void*>(rt_current().ipc), sizeof(info));
    return info;
}

// Bounded copies through this thread's IPC aperture. No foreign VA lease is
// required; revocation is reported by the native object operation.
inline auto mem_read(cap_t cap, uint64_t offset, std::span<std::byte> output) -> status_t {
    while (!output.empty()) {
        const auto size = std::min(output.size(), size_t{4096} - size_t(offset % 4096));
        const auto status = sys::memory_read(cap, offset, 0, size).status;
        if (status != STATUS_OK) return status;
        ::memmove(output.data(), reinterpret_cast<const void*>(rt_current().ipc), size);
        offset += size; output = output.subspan(size);
    }
    return STATUS_OK;
}
inline auto mem_write(cap_t cap, uint64_t offset, std::span<const std::byte> input) -> status_t {
    while (!input.empty()) {
        const auto size = std::min(input.size(), size_t{4096} - size_t(offset % 4096));
        ::memmove(reinterpret_cast<void*>(rt_current().ipc), input.data(), size);
        const auto status = sys::memory_write(cap, offset, 0, size).status;
        if (status != STATUS_OK) return status;
        offset += size; input = input.subspan(size);
    }
    return STATUS_OK;
}

inline void cap_encode(const CapView& value,
                       std::span<uint8_t, CAP_ATTENUATION_SIZE> output) noexcept {
    const auto put = [&output](size_t offset, uint64_t field, size_t width) noexcept {
        for (size_t byte = 0; byte < width; ++byte) {
            output[offset + byte] = static_cast<uint8_t>(field >> (byte * 8));
        }
    };
    put(CAP_ATTENUATION_VERSION_OFFSET, value.version, 2);
    put(CAP_ATTENUATION_KIND_OFFSET, value.kind, 2);
    put(CAP_ATTENUATION_SIZE_OFFSET, value.size, 4);
    put(CAP_ATTENUATION_RIGHTS_OFFSET, value.rights, 8);
    for (size_t index = 0; index < 6; ++index) {
        put(CAP_ATTENUATION_WORD0_OFFSET + index * 8, value.words[index], 8);
    }
}

// Private descriptor scratch is reused across typed delegations. It must not
// be mapped or exported to a peer that could alter an attenuation descriptor.
class cap_desc {
    cap_owner memory_;
public:
    auto derive(cap_t source, const CapView& view, cap_t quota = 0) -> std::expected<cap_owner, status_t> {
        if (!memory_) {
            const auto created = sys::memory_create(rt_startup().alloc(), 4096, VM_READ | VM_WRITE);
            if (created.status != STATUS_OK) return std::unexpected(created.status);
            memory_ = cap_owner{created.value};
        }
        std::array<uint8_t, CAP_ATTENUATION_SIZE> bytes;
        cap_encode(view, bytes);
        const auto status = mem_write(memory_.selector(), 0, std::as_bytes(std::span{bytes}));
        if (status != STATUS_OK) return std::unexpected(status);
        const auto delegated = sys::cap_typed_delegate(source, rt_startup().cspace, memory_.selector(), 0, quota);
        if (delegated.status != STATUS_OK) return std::unexpected(delegated.status);
        return cap_owner{delegated.value};
    }
};

// Accepted effects and cleanup survive the caller. Waiting is a userspace
// composition over the common Op source, independent of provider type.
inline auto op_wait(cap_t op) noexcept -> sys::SysResult {
    for (;;) {
        const auto result = sys::op_query(op);
        if (result.value2 == OP_DONE || result.status != STATUS_PENDING) return result;
        const auto waited = sys::wait_one(op, OP_DONE, 0, 0, 0, SOURCE_OP);
        if (waited.status != STATUS_OK && waited.status != STATUS_CLOSED) return waited;
    }
}
inline auto op_wait(sys::SysResult submitted) noexcept -> sys::SysResult {
    if (submitted.status != STATUS_PENDING) return submitted;
    cap_owner op{submitted.value};
    return op_wait(op.selector());
}
