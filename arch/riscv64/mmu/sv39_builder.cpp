#include "sv39_builder.hpp"

#include "root_access.hpp"
#include "sv39_table.hpp"

#include "arch/riscv64/cpu/csr.hpp"

#include <libk/utility.hpp>
#include <core/debug.hpp>

namespace arch::riscv64 {

namespace {
[[nodiscard]] constexpr auto empty_root() noexcept -> kernel::mm::Page {
    return kernel::mm::Page{kernel::mm::Pfn{libk::numeric_limits<uintptr_t>::max()}};
}
} // namespace

namespace {
struct Mapping {
    Sv39VPage virtual_page;
    Pte leaf_entry;

    [[nodiscard]] static auto from(kernel::mm::VPage virtual_page, kernel::mm::Page physical_page,
                                   PtePerm permissions) noexcept
        -> libk::Expected<Mapping, MappingError> {
        const auto sv39_page = Sv39VPage::from(virtual_page);
        if (!sv39_page) {
            return libk::unexpected(MappingError::BadVAddr);
        }
        const auto leaf_entry = Pte::leaf_4k(physical_page, permissions);
        if (!leaf_entry) {
            return libk::unexpected(MappingError::BadPAddr);
        }
        return libk::expected(Mapping{
            *sv39_page,
            *leaf_entry,
        });
    }
};

struct NewTable {
    kernel::mm::Page page;
    Pte parent_entry;
};

[[nodiscard]] auto alloc_table(kernel::mm::OwnedPageGroupExtension& extension) noexcept
    -> libk::Expected<NewTable, MappingError> {
    auto allocation = extension.allocate_page();

    if (!allocation) {
        return libk::unexpected(MappingError::AllocFailed);
    }

    const kernel::mm::Page page = allocation.value();
    const auto parent_entry = Pte::non_leaf(page);
    if (!parent_entry) {
        return libk::unexpected(MappingError::BadPAddr);
    }

    static_cast<void>(TableRef::initialize(extension.bytes(page)));
    return libk::expected(NewTable{
        page,
        *parent_entry,
    });
}
} // namespace

Sv39Builder::Sv39Builder(kernel::mm::Page root_page, kernel::mm::OwnedPageGroup&& page_tables) noexcept
    : root_page_(root_page), page_tables_(libk::move(page_tables)) {}

Sv39Builder::Sv39Builder(Sv39Builder&& other) noexcept
    : root_page_(libk::exchange(other.root_page_, empty_root())),
      page_tables_(libk::move(other.page_tables_)) {}

auto Sv39Builder::create(kernel::mm::Pmm& pmm) noexcept -> CreateResult {
    auto page_tables = pmm.make_page_group();
    kernel::mm::Page root_page{};

    {
        auto extension = page_tables.extend();
        auto allocation = alloc_table(extension);
        if (!allocation) {
            return libk::unexpected(allocation.error());
        }

        root_page = allocation.value().page;
        extension.commit();
    }

    // PTE and SATP currently both carry a 44-bit PPN, but they are distinct
    // hardware contracts.  Validate the root against SATP here rather than
    // relying on those widths remaining equal.
    if (!Satp::try_make_sv39(root_page.frame().raw())) {
        return libk::unexpected(MappingError::BadPAddr);
    }

    return libk::expected(Sv39Builder{
        root_page,
        libk::move(page_tables),
    });
}

auto Sv39Builder::root_page() const noexcept -> kernel::mm::Page {
    KASSERT(root_page_.valid());
    KASSERT(page_tables_);
    return root_page_;
}

auto Sv39Builder::ensure_root_branch(kernel::mm::VPage virtual_page) noexcept
    -> MapResult {
    const auto page = Sv39VPage::from(virtual_page);
    if (!page) {
        return libk::unexpected(MappingError::BadVAddr);
    }
    auto root = TableRef::open(page_tables_, root_page());
    Pte& entry = root.entry(page->level2_index());
    if (entry.valid()) {
        return entry.is_non_leaf()
            ? libk::expected()
            : libk::Expected<void, MappingError>{
                libk::unexpected(MappingError::MappingConflict)};
    }
    auto extension = page_tables_.extend();
    auto table = alloc_table(extension);
    if (!table) {
        return libk::unexpected(table.error());
    }
    entry = table.value().parent_entry;
    extension.commit();
    return libk::expected();
}

auto Sv39Builder::map_page(kernel::mm::VPage virtual_page, kernel::mm::Page physical_page,
                           PtePerm permissions) noexcept -> MapResult {
    const auto preparation = Mapping::from(virtual_page, physical_page, permissions);
    if (!preparation) {
        return libk::unexpected(preparation.error());
    }
    const Mapping mapping = preparation.value();
    auto root = TableRef::open(page_tables_, root_page());
    auto& level2_entry = root.entry(mapping.virtual_page.level2_index());

    if (!level2_entry.valid()) {
        auto extension = page_tables_.extend();

        auto level1_allocation = alloc_table(extension);
        if (!level1_allocation) {
            return libk::unexpected(level1_allocation.error());
        }

        auto level0_allocation = alloc_table(extension);
        if (!level0_allocation) {
            return libk::unexpected(level0_allocation.error());
        }

        const NewTable level1 = level1_allocation.value();
        const NewTable level0 = level0_allocation.value();

        auto level1_table = TableRef::open(page_tables_, level1.page);
        auto level0_table = TableRef::open(page_tables_, level0.page);

        level0_table.entry(mapping.virtual_page.level0_index()) = mapping.leaf_entry;
        level1_table.entry(mapping.virtual_page.level1_index()) = level0.parent_entry;

        level2_entry = level1.parent_entry;
        extension.commit();
        return libk::expected();
    } // ! level2_entry valid

    const auto level1_page = level2_entry.next_table_page();
    if (!level1_page) {
        return libk::unexpected(MappingError::MappingConflict);
    }

    auto level1_table = TableRef::open(page_tables_, *level1_page);
    auto& level1_entry = level1_table.entry(mapping.virtual_page.level1_index());

    if (!level1_entry.valid()) {
        auto extension = page_tables_.extend();
        auto level0_allocation = alloc_table(extension);
        if (!level0_allocation) {
            return libk::unexpected(level0_allocation.error());
        }

        const NewTable level0 = level0_allocation.value();
        auto level0_table = TableRef::open(page_tables_, level0.page);

        level0_table.entry(mapping.virtual_page.level0_index()) = mapping.leaf_entry;

        level1_entry = level0.parent_entry;
        extension.commit();
        return libk::expected();
    } // !level1_entry.valid

    const auto level0_page = level1_entry.next_table_page();
    if (!level0_page) {
        return libk::unexpected(MappingError::MappingConflict);
    }

    auto level0_table = TableRef::open(page_tables_, *level0_page);
    auto& leaf_entry = level0_table.entry(mapping.virtual_page.level0_index());

    if (leaf_entry.valid()) {
        return libk::unexpected(MappingError::MappingConflict);
    }

    leaf_entry = mapping.leaf_entry;
    return libk::expected();
}

auto Sv39Builder::mapping_at(kernel::mm::VPage virtual_page) const noexcept -> libk::optional<Pte> {
    const auto page = Sv39VPage::from(virtual_page);
    if (!page) {
        return libk::nullopt;
    }

    auto root = TableView::open(page_tables_, root_page_);
    const auto level1_page = root.entry(page->level2_index()).next_table_page();
    if (!level1_page) {
        return libk::nullopt;
    }

    auto level1_table = TableView::open(page_tables_, *level1_page);
    const auto level0_page = level1_table.entry(page->level1_index()).next_table_page();
    if (!level0_page) {
        return libk::nullopt;
    }

    auto level0_table = TableView::open(page_tables_, *level0_page);
    const Pte leaf = level0_table.entry(page->level0_index());
    return leaf.is_leaf() ? libk::optional<Pte>{leaf} : libk::nullopt;
}

auto Sv39Builder::finalize() && noexcept -> arch::KernelRoot {
    return arch::RootAccess::make_kernel(
        libk::exchange(root_page_, empty_root()),
        libk::move(page_tables_));
}

} // namespace arch::riscv64
