#include <core/kernel_image.hpp>

#include <core/debug.hpp>

extern "C" {
extern char kernel_img_start[];
extern char kernel_img_end[];
}

namespace kernel::image {
namespace {

[[nodiscard]] auto virtual_address(const char* symbol) noexcept -> usize {
    return reinterpret_cast<usize>(symbol);
}

// Low physical linker symbols are outside RISC-V's medany PC-relative reach
// from the high kernel image. The linker exports page-frame values so this
// selected-architecture boundary can materialize them without duplicating a
// physical address or truncating an absolute relocation.
#define LINKER_PFN(symbol)                                                  \
    []() noexcept -> usize {                                               \
        usize value;                                                       \
        asm volatile(                                                      \
            "lui %0, %%hi(" #symbol ")\n"                                 \
            "addi %0, %0, %%lo(" #symbol ")\n"                           \
            : "=r"(value));                                               \
        return value;                                                      \
    }()

#define LINKER_PHYSICAL(symbol) (LINKER_PFN(symbol) * mm::page_size)

[[nodiscard]] auto page_range(usize first, usize end) noexcept
    -> mm::PageRange {
    KASSERT(end > first);
    const auto range = mm::PageRange::from_aligned_bytes(
        mm::PhysAddr{first}, end - first);
    KASSERT(range);
    return *range;
}

} // namespace

auto virtual_begin() noexcept -> mm::VirtAddr {
    return mm::VirtAddr{virtual_address(kernel_img_start)};
}

auto virtual_end() noexcept -> mm::VirtAddr {
    return mm::VirtAddr{virtual_address(kernel_img_end)};
}

auto physical_begin() noexcept -> mm::PhysAddr {
    return mm::PhysAddr{LINKER_PHYSICAL(kernel_image_first_pfn)};
}

auto boot_entry() noexcept -> mm::PageRange {
    return page_range(
        LINKER_PHYSICAL(boot_entry_first_pfn),
        LINKER_PHYSICAL(boot_entry_end_pfn));
}

auto secondary_entry() noexcept -> mm::PageRange {
    return page_range(
        LINKER_PHYSICAL(secondary_entry_first_pfn),
        LINKER_PHYSICAL(secondary_entry_end_pfn));
}

auto transition() noexcept -> mm::PageRange {
    return page_range(
        LINKER_PHYSICAL(boot_reclaim_first_pfn),
        LINKER_PHYSICAL(boot_reclaim_end_pfn));
}

auto physical_image() noexcept -> mm::PageRange {
    return page_range(
        LINKER_PHYSICAL(kernel_image_first_pfn),
        LINKER_PHYSICAL(kernel_image_end_pfn));
}

auto linked_physical(mm::VirtAddr address) noexcept
    -> libk::optional<mm::PhysAddr> {
    const usize first = virtual_begin().raw();
    const usize end = virtual_end().raw();
    if (address.raw() < first || address.raw() >= end) {
        return libk::nullopt;
    }
    return physical_begin().checked_add(address.raw() - first);
}

#undef LINKER_PHYSICAL
#undef LINKER_PFN

} // namespace kernel::image
