#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/noncopyable.hpp>
#include <libk/span.hpp>
#include <mm/memory_object.hpp>
#include <mm/user_view.hpp>
#include <uapi/ipc.h>

namespace kernel::mm {
class VSpace;
}

namespace kernel::ipc {

enum class BufferError : u8 {
    Invalid,
    Unavailable,
    NoMemory,
};

// A registered IPC buffer is a stable mapping relation plus resident backing.
// Kernel copies never follow a transient user pointer and remain bounded by
// the compile-time page limit. The VSpace mapping is still the authority truth;
// PageLease only keeps already-authorized backing safe during an operation.
class Buffer final : private libk::noncopyable {
public:
    using Pages = libk::InplaceVector<
        kernel::mm::PageLease, MYOS_IPC_BUFFER_MAX_PAGES>;

    Buffer() noexcept = default;
    Buffer(Buffer&&) noexcept = default;
    auto operator=(Buffer&&) noexcept -> Buffer& = default;
    ~Buffer() noexcept = default;

    [[nodiscard]] static auto bind(
        kernel::mm::Pmm& pmm,
        kernel::mm::VSpace& vspace,
        kernel::object::ObjectRef&& memory_ref,
        kernel::mm::MemoryObject& memory,
        kernel::mm::ObjectRange object,
        kernel::mm::VirtRange virtual_range) noexcept
        -> libk::Expected<Buffer, BufferError>;

    [[nodiscard]] auto valid() const noexcept -> bool;
    [[nodiscard]] auto size() const noexcept -> usize {
        return pages_.size() * kernel::mm::page_size;
    }
    [[nodiscard]] auto virtual_range() const noexcept
        -> kernel::mm::VirtRange {
        return view_.virtual_range();
    }
    [[nodiscard]] auto read(
        usize offset, libk::Span<byte> output) const noexcept -> bool;
    [[nodiscard]] auto write(
        usize offset, libk::Span<const byte> input) noexcept -> bool;
    void reset() noexcept;

private:
    Buffer(
        kernel::mm::Pmm& pmm,
        kernel::mm::UserView&& view,
        Pages&& pages) noexcept
        : pmm_(&pmm), view_(libk::move(view)), pages_(libk::move(pages)) {}

    kernel::mm::Pmm* pmm_{};
    kernel::mm::UserView view_{};
    Pages pages_{};
};

} // namespace kernel::ipc
