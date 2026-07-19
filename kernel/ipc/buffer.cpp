#include <ipc/buffer.hpp>

#include <libk/checked_arithmetic.hpp>
#include <libk/mem.h>
#include <libk/utility.hpp>
#include <mm/vspace.hpp>

namespace kernel::ipc {

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
    Pages pages{};
    for (usize index = 0; index < *count; ++index) {
        auto page = memory.materialize(object.first + index);
        if (!page) {
            return libk::unexpected(page.error()
                    == kernel::mm::MemoryError::OutOfMemory
                ? BufferError::NoMemory : BufferError::Unavailable);
        }
        KASSERT(pages.try_push_back(libk::move(page).value()));
    }
    return libk::expected(Buffer{
        pmm, libk::move(view).value(), libk::move(pages)});
}

auto Buffer::valid() const noexcept -> bool {
    return pmm_ != nullptr && view_.valid() && !pages_.empty();
}

auto Buffer::read(
    usize offset, libk::Span<byte> output) const noexcept -> bool {
    const auto end = libk::checked_add(offset, output.size());
    if (!valid() || !end || *end > size()) {
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

auto Buffer::write(
    usize offset, libk::Span<const byte> input) noexcept -> bool {
    const auto end = libk::checked_add(offset, input.size());
    if (!valid() || !end || *end > size()) {
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

void Buffer::reset() noexcept {
    pages_.clear();
    view_.reset();
    pmm_ = nullptr;
}

} // namespace kernel::ipc
