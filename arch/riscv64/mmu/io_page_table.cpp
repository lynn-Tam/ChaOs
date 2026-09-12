#include <arch/io_page_table.hpp>

#include "sv39_builder.hpp"
#include <libk/utility.hpp>

namespace arch {

IoRoot::IoRoot(kernel::mm::Page root, kernel::mm::OwnedPageGroup&& tables) noexcept
    : root_(root), tables_(libk::move(tables)) {}

IoRoot::IoRoot(IoRoot&& other) noexcept
    : root_(libk::exchange(other.root_, kernel::mm::Page{})),
      tables_(libk::move(other.tables_)) {}

auto IoRoot::create(kernel::mm::Pmm& pmm, usize first,
    libk::Span<const kernel::mm::Page> pages, bool writable) noexcept
    -> libk::Expected<IoRoot, IoRootError> {
    usize index{};
    auto next = [&]() noexcept { return pages[index++]; };
    return create(pmm, first, pages.size(), PageSource::bind(next), writable);
}

auto IoRoot::required_pages(usize first, usize count) noexcept
    -> libk::Expected<usize, IoRootError> {
    constexpr usize Limit = usize{1} << 38;
    constexpr usize PageSize = kernel::mm::page_size;
    // Use the positive Sv39 range. Keeping address zero unmapped catches a
    // null DMA descriptor without conflating IOVAs with kernel virtual memory.
    if (first == 0 || first >= Limit || first % PageSize != 0
        || count == 0 || count > (Limit - first) / PageSize)
        return libk::unexpected(IoRootError::InvalidRange);
    const usize last = first + (count - 1) * PageSize;
    // One root, one middle table per 1 GiB, one leaf table per 2 MiB.
    return libk::expected(usize{1}
        + (last >> 30) - (first >> 30) + 1
        + (last >> 21) - (first >> 21) + 1);
}

auto IoRoot::create(kernel::mm::Pmm& pmm, usize first, usize count,
    PageSource pages, bool writable) noexcept
    -> libk::Expected<IoRoot, IoRootError> {
    const auto required = required_pages(first, count);
    if (!required || !pages)
        return libk::unexpected(IoRootError::InvalidRange);
    auto result = riscv64::Sv39Builder::create(pmm);
    if (!result) return libk::unexpected(IoRootError::InsufficientMemory);
    auto builder = libk::move(result).value();
    usize address = first;
    for (usize index = 0; index < count; ++index) {
        const auto page = pages();
        // Non-PASID PCI transactions are unprivileged IOMMU accesses. U must
        // be set; R/W never implies permission to execute through this tree.
        const auto mapped = builder.map_page(
            *kernel::mm::VPage::from_base(kernel::mm::VirtAddr{address}), page,
            writable ? riscv64::PtePerm::user_rw() : riscv64::PtePerm::user_ro());
        if (!mapped) {
            return libk::unexpected(mapped.error() == riscv64::MappingError::AllocFailed
                ? IoRootError::InsufficientMemory : IoRootError::InvalidRange);
        }
        address += kernel::mm::page_size;
    }
    return libk::expected(libk::move(builder).finalize_io());
}

} // namespace arch
