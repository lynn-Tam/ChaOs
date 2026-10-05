#pragma once

#include <utility>


#include <base/types.hpp>
#include <expected>
#include <libk/inplace_vector.hpp>
#include <libk/noncopyable.hpp>
#include <libk/span.hpp>
#include <mm/mem.hpp>
#include <mm/vspace.hpp>
#include <uapi/ipc.h>

namespace mm {
class VSpace;
}

namespace ipc {

enum class BufferError : u8 {
    Invalid,
    Unavailable,
    NoMemory,
};

// A registered IPC buffer is a stable mapping relation plus resident backing.
// Kernel copies never follow a transient user pointer and remain bounded by
// the compile-time page limit. The VSpace mapping is still the authority truth;
// PageHold only keeps already-authorized backing safe during an operation.
class Buffer final : private libk::noncopyable {
public:
    using Leases = libk::InplaceVector<
        mm::PageHold, MYOS_IPC_BUFFER_MAX_PAGES>;

    class Perm final : private libk::noncopyable {
    public:
        Perm(Perm&&) noexcept = default;
        auto operator=(Perm&&) noexcept -> Perm& = default;

        // A bounded single-page borrow, valid for this Perm lifetime.
        [[nodiscard]] auto bytes(usize offset, usize size) const noexcept -> libk::Span<const byte>;
        [[nodiscard]] auto read(
            usize offset, libk::Span<byte> output) const noexcept -> bool;
        [[nodiscard]] auto write(
            usize offset, libk::Span<const byte> input) noexcept -> bool;

    private:
        friend class Buffer;
        Perm(mm::Pmm& pmm, Leases&& pages, usize size) noexcept
            : pmm_(&pmm), pages_(std::move(pages)), size_(size) {}

        mm::Pmm* pmm_{};
        Leases pages_{};
        usize size_{};
    };

    Buffer() noexcept = default;
    Buffer(Buffer&&) noexcept = default;
    auto operator=(Buffer&&) noexcept -> Buffer& = default;
    ~Buffer() noexcept = default;

    [[nodiscard]] static auto bind(
        mm::Pmm& pmm,
        mm::VSpace& vspace,
        object::ref<>&& memory_ref,
        mm::Mem& memory,
        mm::ObjectRange object,
        mm::VRange virtual_range) noexcept
        -> std::expected<Buffer, BufferError>;

    [[nodiscard]] auto valid() const noexcept -> bool;
    [[nodiscard]] auto size() const noexcept -> usize {
        return object_.size() * mm::page_size;
    }
    [[nodiscard]] auto virtual_range() const noexcept
        -> mm::VRange {
        return virtual_;
    }
    [[nodiscard]] auto read(
        usize offset, libk::Span<byte> output) const noexcept -> bool;
    [[nodiscard]] auto write(
        usize offset, libk::Span<const byte> input) noexcept -> bool;
    [[nodiscard]] auto access() const noexcept
        -> std::expected<Perm, BufferError>;
    void reset() noexcept;

private:
    Buffer(
        mm::Pmm& pmm,
        mm::Mem& memory,
        mm::ObjectRange object,
        mm::View&& view,
        mm::VRange virtual_range) noexcept
        : pmm_(&pmm),
          memory_(&memory),
          object_(object),
          virtual_(virtual_range),
          view_(std::move(view)) {}

    [[nodiscard]] auto lease_pages() const noexcept
        -> std::expected<Leases, mm::MemErr>;

    mm::Pmm* pmm_{};
    mm::Mem* memory_{};
    mm::ObjectRange object_{};
    mm::VRange virtual_{};
    mm::View view_{};
};

} // namespace ipc
