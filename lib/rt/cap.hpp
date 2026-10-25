#pragma once

#include <utility>
#include <array>
#include <algorithm>
#include <span>
#include <expected>
#include <type_traits>
#include <start.hpp>
#include <string.h>
#include <sys/syscall.hpp>
#include <libk/unique_handle.hpp>


// Owns a slot in this task's CSpace. The child CSpace owns delivered slots;
// managers close a remote slot explicitly, while that CSpace is still alive.
class _cap_owner {
    struct drop {
        void operator()(cap_t cap) const noexcept {
            const auto s = _cap_close(cap).status;
            if (s != STATUS_OK) _thread_exit(s);
        }
    };
    libk::unique_handle<cap_t, drop> cap_{};
public:
    _cap_owner() noexcept = default;
    explicit _cap_owner(cap_t cap) noexcept : cap_(cap) {}
    explicit operator bool() const noexcept { return bool(cap_); }
    auto selector() const noexcept -> cap_t { return cap_.get(); }
    auto release() noexcept -> cap_t { return cap_.release(); }
    auto close() noexcept -> status_t {
        if (!cap_) return STATUS_OK;
        const auto s = _cap_close(cap_.get()).status;
        if (s == STATUS_OK) (void)cap_.release();
        return s;
    }
};

template<class T> requires (std::is_trivially_copyable_v<T> && sizeof(T) <= 4096)
inline auto _cap_copy(_sys_result result) -> std::expected<T, status_t> {
    if (result.status != STATUS_OK) return std::unexpected(result.status);
    T value; ::memcpy(&value, reinterpret_cast<const void*>(_ipc()), sizeof(value));
    return value;
}

inline auto _cap_mem(word_t rights, word_t first, word_t pages, word_t perms) noexcept -> CapView {
    return {.version=CAP_ATTENUATION_VERSION_CURRENT, .kind=ObjMem, .size=CAP_ATTENUATION_SIZE,
        .rights=rights, .words={first, pages, perms}};
}

inline void _cap_encode(const CapView& value,
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
class _cap_desc {
    _cap_owner memory_;
public:
    auto derive(cap_t source, const CapView& view, cap_t quota = 0) -> std::expected<_cap_owner, status_t> {
        if (!memory_) {
            const auto created = _memory_create(_startup().alloc(), 4096, VM_READ | VM_WRITE);
            if (created.status != STATUS_OK) return std::unexpected(created.status);
            memory_ = _cap_owner{created.value};
        }
        std::array<uint8_t, CAP_ATTENUATION_SIZE> bytes;
        _cap_encode(view, bytes);
        ::memcpy(reinterpret_cast<void*>(_ipc()), bytes.data(), bytes.size());
        const auto status = _memory_write(memory_.selector(), 0, 0, bytes.size()).status;
        if (status != STATUS_OK) return std::unexpected(status);
        const auto delegated = _cap_typed_delegate(source, _startup().cspace, memory_.selector(), 0, quota);
        if (delegated.status != STATUS_OK) return std::unexpected(delegated.status);
        return _cap_owner{delegated.value};
    }
};
