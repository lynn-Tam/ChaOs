#include <optional>
#include <boot/link.hpp>

#include <libk/assert.hpp>
#include <base/types.hpp>

extern "C" {
extern char kernel_img_start[];
extern char kernel_img_end[];
}

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
    -> mm::Pages {
    libk_assert(end > first);
    const auto range = mm::Pages::from_aligned_bytes(
        mm::Phys{first}, end - first);
    libk_assert(range);
    return *range;
}

} // namespace

auto kernel_begin() noexcept -> mm::Virt {
    return mm::Virt{virtual_address(kernel_img_start)};
}

auto kernel_end() noexcept -> mm::Virt {
    return mm::Virt{virtual_address(kernel_img_end)};
}

auto kernel_phys() noexcept -> mm::Phys {
    return mm::Phys{LINKER_PHYSICAL(kernel_image_first_pfn)};
}

auto boot_pages() noexcept -> mm::Pages {
    return page_range(
        LINKER_PHYSICAL(boot_entry_first_pfn),
        LINKER_PHYSICAL(boot_entry_end_pfn));
}

auto secondary_pages() noexcept -> mm::Pages {
    return page_range(
        LINKER_PHYSICAL(secondary_entry_first_pfn),
        LINKER_PHYSICAL(secondary_entry_end_pfn));
}

auto transition_pages() noexcept -> mm::Pages {
    return page_range(
        LINKER_PHYSICAL(boot_reclaim_first_pfn),
        LINKER_PHYSICAL(boot_reclaim_end_pfn));
}

auto kernel_pages() noexcept -> mm::Pages {
    return page_range(
        LINKER_PHYSICAL(kernel_image_first_pfn),
        LINKER_PHYSICAL(kernel_image_end_pfn));
}

auto kernel_phys(mm::Virt address) noexcept
    -> std::optional<mm::Phys> {
    const usize first = kernel_begin().raw();
    const usize end = kernel_end().raw();
    if (address.raw() < first || address.raw() >= end) {
        return std::nullopt;
    }
    return kernel_phys().checked_add(address.raw() - first);
}

#undef LINKER_PHYSICAL
#undef LINKER_PFN

