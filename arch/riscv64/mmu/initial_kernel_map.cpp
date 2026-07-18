// Final kernel translation policy.  The semantic owners are KernelVSpace and
// DirectMap; this file only materializes their initial RISC-V projection.

#include "initial_kernel_map.hpp"

#include <core/debug.hpp>
#include <libk/optional.hpp>
#include <mm/addr.hpp>
#include <mm/pmm.hpp>
#include <core/kernel_image.hpp>

#include "range_map.hpp"
#include "sv39_builder.hpp"

extern "C" {
extern char kernel_img_start[];
extern char kernel_text_start[];
extern char kernel_text_end[];
extern char kernel_rodata_start[];
extern char kernel_rodata_end[];
extern char kernel_data_start[];
extern char kernel_data_end[];
extern char kernel_bss_start[];
extern char kernel_bss_end[];
extern char kernel_bootstack_start[];
extern char kernel_bootstack_end[];
extern char kernel_img_end[];
}

namespace arch::riscv64 {
namespace {

[[nodiscard]] auto address_of(const char* symbol) noexcept -> uintptr_t {
    return reinterpret_cast<uintptr_t>(symbol);
}

[[nodiscard]] auto physical_for(uintptr_t virtual_address) noexcept
    -> libk::optional<kernel::mm::PhysAddr> {
    return kernel::image::linked_physical(
        kernel::mm::VirtAddr{virtual_address});
}

[[nodiscard]] auto map_at(
    Sv39Builder& builder,
    kernel::mm::VirtAddr virtual_base,
    kernel::mm::PageRange physical,
    PtePerm permissions) noexcept -> Sv39Builder::MapResult {
    const auto first = kernel::mm::VPage::from_base(virtual_base);
    if (!first) {
        return libk::unexpected(MappingError::BadVAddr);
    }
    return map_range(builder, *first, physical, permissions);
}

[[nodiscard]] auto map_and_verify(
    Sv39Builder& builder,
    kernel::mm::VirtAddr virtual_base,
    kernel::mm::PageRange physical,
    PtePerm permissions) noexcept -> Sv39Builder::MapResult {
    auto mapped = map_at(builder, virtual_base, physical, permissions);
    if (!mapped) {
        return mapped;
    }
    const auto first = kernel::mm::VPage::from_base(virtual_base);
    KASSERT(first);
    KASSERT(maps_range(builder, *first, physical, permissions));
    return libk::expected();
}

[[nodiscard]] auto convert_mapping_error(MappingError error) noexcept
    -> InitialKernelMapResult {
    switch (error) {
    case MappingError::AllocFailed:
        return libk::unexpected(InitialKernelMapError::InsufficientMemory);
    case MappingError::BadPAddr:
    case MappingError::BadVAddr:
        return libk::unexpected(InitialKernelMapError::UnrepresentableAddress);
    case MappingError::MappingConflict:
        KASSERT(false);
    }
    __builtin_unreachable();
}

struct LinkedSection final {
    const char* first;
    const char* end;
    PtePerm permissions;
    bool required;
};

[[nodiscard]] auto map_linked_section(
    Sv39Builder& builder,
    const LinkedSection& section) noexcept -> InitialKernelMapResult {
    const uintptr_t first = address_of(section.first);
    const uintptr_t end = address_of(section.end);
    KASSERT(first <= end);
    KASSERT(first % kernel::mm::page_size == 0);
    KASSERT(end % kernel::mm::page_size == 0);

    if (first == end) {
        KASSERT(!section.required);
        return libk::expected();
    }
    const auto physical = physical_for(first);
    if (!physical) {
        return libk::unexpected(InitialKernelMapError::UnrepresentableAddress);
    }
    const auto range = kernel::mm::PageRange::from_aligned_bytes(*physical, end - first);
    if (!range) {
        return libk::unexpected(InitialKernelMapError::UnrepresentableAddress);
    }
    const auto mapped = map_and_verify(
        builder, kernel::mm::VirtAddr{first}, *range, section.permissions);
    if (!mapped) {
        return convert_mapping_error(mapped.error());
    }
    return libk::expected();
}

auto validate_linker_layout() noexcept -> void {
    KASSERT(address_of(kernel_img_start)
        == kernel::image::virtual_begin().raw());
    KASSERT(address_of(kernel_img_start) == address_of(kernel_text_start));
    KASSERT(address_of(kernel_text_end) == address_of(kernel_rodata_start));
    KASSERT(address_of(kernel_rodata_end) == address_of(kernel_data_start));
    KASSERT(address_of(kernel_data_end) == address_of(kernel_bss_start));
    KASSERT(address_of(kernel_bss_end) == address_of(kernel_bootstack_start));
    KASSERT(address_of(kernel_bootstack_end) == address_of(kernel_img_end));
}

} // namespace

auto map_initial_kernel(Sv39Builder& builder, kernel::mm::Pmm& pmm) noexcept
    -> InitialKernelMapResult {
    validate_linker_layout();

    const LinkedSection linked_sections[] = {
        {kernel_text_start, kernel_text_end, PtePerm::supervisor_rx(), true},
        {kernel_rodata_start, kernel_rodata_end, PtePerm::supervisor_ro(), false},
        {kernel_data_start, kernel_data_end, PtePerm::supervisor_rw(), false},
        {kernel_bss_start, kernel_bss_end, PtePerm::supervisor_rw(), false},
        {kernel_bootstack_start, kernel_bootstack_end,
         PtePerm::supervisor_rw(), true},
    };

    for (const LinkedSection& section : linked_sections) {
        auto mapped = map_linked_section(builder, section);
        if (!mapped) {
            return mapped;
        }
    }

    const kernel::mm::DirectMap& direct = pmm.direct_map();
    for (usize index = 0; index < direct.range_count(); ++index) {
        const kernel::mm::PageRange range = direct.range(index);
        const usize bytes = range.page_count() * kernel::mm::page_size;
        const auto virtual_base = direct.map(range.first().base(), bytes);
        KASSERT(virtual_base);
        const auto mapped = map_and_verify(
            builder, virtual_base.value(), range, PtePerm::supervisor_rw());
        if (!mapped) {
            return convert_mapping_error(mapped.error());
        }
    }

    // Secondary harts enter with translation disabled.  This final-root alias
    // is the narrow bridge from their physical trampoline to the high entry;
    // D2 removes it after the last secondary acknowledges activation.
    const auto boot_entry = kernel::image::boot_entry();
    const auto secondary = kernel::image::secondary_entry();
    auto mapped = map_and_verify(
        builder,
        kernel::mm::VirtAddr{boot_entry.first().base().raw()},
        boot_entry,
        PtePerm::supervisor_rx());
    if (!mapped) {
        return convert_mapping_error(mapped.error());
    }
    mapped = map_and_verify(
        builder,
        kernel::mm::VirtAddr{secondary.first().base().raw()},
        secondary,
        PtePerm::supervisor_rx());
    if (!mapped) {
        return convert_mapping_error(mapped.error());
    }

    return libk::expected();
}

} // namespace arch::riscv64
