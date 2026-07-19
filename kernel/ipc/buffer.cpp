#include <ipc/buffer.hpp>

#include <libk/checked_arithmetic.hpp>
#include <libk/mem.h>
#include <libk/utility.hpp>
#include <mm/vspace.hpp>

namespace kernel::ipc {

auto Buffer::Access::read(
    usize offset, libk::Span<byte> output) const noexcept -> bool {
    const auto end = libk::checked_add(offset, output.size());
    if (pmm_ == nullptr || !end || *end > size_) {
        return false;
    }
    usize copied{};
    while (copied != output.size()) {
        const usize position = offset + copied;
        const usize page_index = position / kernel::mm::page_size;
        const usize in_page = position % kernel::mm::page_size;
        const usize available = kernel::mm::page_size - in_page;
        const usize remaining = output.size() - copied;
        const usize count = available < remaining ? available : remaining;
        const byte* const bytes = pmm_->bytes(pages_[page_index].page().page);
        if (bytes == nullptr) {
            return false;
        }
        memcpy(output.data() + copied, bytes + in_page, count);
        copied += count;
    }
    return true;
}

auto Buffer::Access::write(
    usize offset, libk::Span<const byte> input) noexcept -> bool {
    const auto end = libk::checked_add(offset, input.size());
    if (pmm_ == nullptr || !end || *end > size_) {
        return false;
    }
    usize copied{};
    while (copied != input.size()) {
        const usize position = offset + copied;
        const usize page_index = position / kernel::mm::page_size;
        const usize in_page = position % kernel::mm::page_size;
        const usize available = kernel::mm::page_size - in_page;
        const usize remaining = input.size() - copied;
        const usize count = available < remaining ? available : remaining;
        byte* const bytes = pmm_->bytes(pages_[page_index].page().page);
        if (bytes == nullptr) {
            return false;
        }
        memcpy(bytes + in_page, input.data() + copied, count);
        copied += count;
    }
    return true;
}

auto Buffer::bind(
    kernel::mm::Pmm& pmm,
    kernel::mm::VSpace& vspace,
    kernel::object::ObjectRef&& memory_ref,
    kernel::mm::MemoryObject& memory,
    kernel::mm::ObjectRange object,
    kernel::mm::VirtRange virtual_range) noexcept
    -> libk::Expected<Buffer, BufferError> {
    const auto count = virtual_range.page_count();
    if (!memory_ref || !count || *count == 0
        || *count > MYOS_IPC_BUFFER_MAX_PAGES
        || object.page_count != *count) {
        return libk::unexpected(BufferError::Invalid);
    }
    const auto rw = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    auto view = vspace.bind_view(kernel::mm::UserViewRequest{
        .memory = libk::move(memory_ref),
        .object = object,
        .virtual_range = virtual_range,
        .access = rw,
    });
    if (!view) {
        return libk::unexpected(BufferError::Unavailable);
    }
    Leases leases{};
    for (usize index = 0; index < *count; ++index) {
        auto page = memory.materialize(object.first + index);
        if (!page) {
            return libk::unexpected(page.error()
                    == kernel::mm::MemoryError::OutOfMemory
                ? BufferError::NoMemory : BufferError::Unavailable);
        }
        KASSERT(leases.try_push_back(libk::move(page).value()));
    }
    // Binding materializes the complete bounded range once, but residency is
    // borrowed only by individual copy operations. Keeping these leases in the
    // Buffer would make an invalid view retain MemoryObject backing forever.
    leases.clear();
    return libk::expected(Buffer{
        pmm, memory, object, libk::move(view).value(), virtual_range});
}

auto Buffer::valid() const noexcept -> bool {
    return pmm_ != nullptr && memory_ != nullptr && object_.page_count != 0
        && view_.valid();
}

auto Buffer::lease_pages() const noexcept
    -> libk::Expected<Leases, kernel::mm::MemoryError> {
    if (pmm_ == nullptr || memory_ == nullptr || object_.page_count == 0
        || object_.page_count > MYOS_IPC_BUFFER_MAX_PAGES) {
        return libk::unexpected(kernel::mm::MemoryError::InvalidState);
    }
    Leases leases{};
    for (usize index = 0; index < object_.page_count; ++index) {
        auto page = memory_->materialize(object_.first + index);
        if (!page) {
            return libk::unexpected(page.error());
        }
        KASSERT(leases.try_push_back(libk::move(page).value()));
    }
    // This is the operation-admission linearization point. An invalidation
    // that wins before it prevents the copy; one that wins after it may revoke
    // the view while these leases safely carry the already-admitted operation.
    if (!view_.valid()) {
        return libk::unexpected(kernel::mm::MemoryError::InvalidState);
    }
    return libk::expected(libk::move(leases));
}

auto Buffer::access() const noexcept
    -> libk::Expected<Access, BufferError> {
    auto leased = lease_pages();
    if (!leased) {
        return libk::unexpected(leased.error()
                == kernel::mm::MemoryError::OutOfMemory
            ? BufferError::NoMemory : BufferError::Unavailable);
    }
    return libk::expected(Access{*pmm_, libk::move(leased).value(), size()});
}

auto Buffer::read(
    usize offset, libk::Span<byte> output) const noexcept -> bool {
    const auto end = libk::checked_add(offset, output.size());
    if (!end || *end > size()) {
        return false;
    }
    auto admitted = access();
    if (!admitted) {
        return false;
    }
    return admitted.value().read(offset, output);
}

auto Buffer::write(
    usize offset, libk::Span<const byte> input) noexcept -> bool {
    const auto end = libk::checked_add(offset, input.size());
    if (!end || *end > size()) {
        return false;
    }
    auto admitted = access();
    if (!admitted) {
        return false;
    }
    return admitted.value().write(offset, input);
}

void Buffer::reset() noexcept {
    view_.reset();
    virtual_ = {};
    object_ = {};
    memory_ = nullptr;
    pmm_ = nullptr;
}

} // namespace kernel::ipc
