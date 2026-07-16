#include "sv39_editor.hpp"

#include "root_access.hpp"
#include "arch/riscv64/cpu/csr.hpp"
#include "sv39_table.hpp"

#include <arch/address_layout.hpp>
#include <core/debug.hpp>
#include <libk/utility.hpp>

namespace arch {

UserRoot::UserRoot(
    kernel::mm::Page root_page,
    kernel::mm::OwnedPageGroup&& tables) noexcept
    : root_page_(root_page), tables_(libk::move(tables)) {
    KASSERT(root_page_.valid());
    KASSERT(tables_);
}

UserRoot::UserRoot(UserRoot&& other) noexcept
    : root_page_(libk::exchange(other.root_page_, empty_root())),
      tables_(libk::move(other.tables_)) {}

UserRoot::operator bool() const noexcept {
    return root_page_.valid() && static_cast<bool>(tables_);
}

auto UserRoot::token() const noexcept -> RootToken {
    KASSERT(*this);
    const auto value =
        riscv64::Satp::try_make_sv39(root_page_.frame().raw());
    KASSERT(value);
    return RootToken{*value};
}

auto UserRoot::create(
    const KernelRoot& shared,
    kernel::mm::Pmm& pmm) noexcept -> libk::Expected<UserRoot, RootError> {
    KASSERT(shared);
    auto tables = pmm.make_page_group();
    kernel::mm::Page root{};
    {
        auto extension = tables.extend();
        auto allocation = extension.allocate_page();
        if (!allocation) {
            return libk::unexpected(RootError::InsufficientMemory);
        }
        root = allocation.value();
        auto destination = riscv64::TableRef::initialize(
            extension.bytes(root));
        const auto source = riscv64::TableView::open(
            RootAccess::tables(shared), RootAccess::page(shared));
        for (usize index = 256; index < riscv64::ptes_per_pg; ++index) {
            const riscv64::Pte entry = source.entry(index);
            KASSERT(entry.is_non_leaf());
            destination.entry(index) = entry;
        }
        extension.commit();
    }
    return libk::expected(RootAccess::make_user(root, libk::move(tables)));
}

} // namespace arch

namespace arch::riscv64 {
namespace {

[[nodiscard]] auto leaf_from(Pte entry) noexcept
    -> libk::Expected<Leaf, EditError> {
    const auto page = entry.leaf_page();
    const auto permissions = entry.permissions();
    if (!page || !permissions) {
        return libk::unexpected(EditError::CorruptTree);
    }
    return libk::expected(Leaf{*page, *permissions});
}

} // namespace

auto Editor::Plan::include(kernel::mm::VPage virtual_page) noexcept -> bool {
    const auto page = Sv39VPage::from(virtual_page);
    if (!page) {
        return false;
    }
    const usize vpn = virtual_page.number().raw();
    if (!empty_ && vpn <= previous_vpn_) {
        return false;
    }
    const usize level2 = vpn >> 18;
    const usize level1 = vpn >> 9;
    if (empty_ || level2 != previous_level2_) {
        ++table_pages_;
    }
    if (empty_ || level1 != previous_level1_) {
        ++table_pages_;
    }
    previous_vpn_ = vpn;
    previous_level2_ = level2;
    previous_level1_ = level1;
    empty_ = false;
    return true;
}

auto Editor::user_permissions(
    kernel::mm::AccessMask access,
    kernel::mm::MemoryType type) noexcept -> libk::optional<PtePerm> {
    if (type != kernel::mm::MemoryType::Normal || !kernel::mm::valid_access(access)
        || (access.contains(kernel::mm::Access::Write)
            && access.contains(kernel::mm::Access::Execute))) {
        return libk::nullopt;
    }
    if (access.contains(kernel::mm::Access::Write)) {
        return PtePerm::user_rw();
    }
    if (access.contains(kernel::mm::Access::Read)) {
        return access.contains(kernel::mm::Access::Execute)
            ? libk::optional<PtePerm>{PtePerm::user_rx()}
            : libk::optional<PtePerm>{PtePerm::user_ro()};
    }
    return PtePerm::user_x();
}

auto Editor::kernel(arch::KernelRoot& root) noexcept -> Editor {
    KASSERT(root);
    return Editor{
        RootAccess::page(root), RootAccess::tables(root), Domain::Kernel};
}

auto Editor::user(arch::UserRoot& root) noexcept -> Editor {
    KASSERT(root);
    return Editor{
        RootAccess::page(root), RootAccess::tables(root), Domain::User};
}

auto Editor::accepts(kernel::mm::VPage page) const noexcept -> bool {
    const kernel::mm::VirtAddr address = page.base();
    return domain_ == Domain::Kernel
        ? layout::is_kernel(address)
        : layout::is_user(address);
}

auto Editor::map(
    kernel::mm::VPage virtual_page,
    kernel::mm::Page physical_page,
    PtePerm permissions) noexcept -> libk::Expected<void, EditError> {
    kernel::mm::OwnedPageGroup prepared = tables_->owner().make_page_group();
    {
        auto extension = prepared.extend();
        for (usize index = 0; index < 2; ++index) {
            if (!extension.allocate_page()) {
                return libk::unexpected(EditError::AllocationFailed);
            }
        }
        extension.commit();
    }
    return map(virtual_page, physical_page, permissions, prepared);
}

auto Editor::take_table(kernel::mm::OwnedPageGroup& prepared) noexcept
    -> libk::Expected<NewTable, EditError> {
    auto taken = prepared.take();
    if (!taken) {
        return libk::unexpected(EditError::AllocationFailed);
    }
    kernel::mm::OwnedPage owned = libk::move(*taken);
    const kernel::mm::Page page = owned.page();
    const auto parent = Pte::non_leaf(page);
    if (!parent) {
        return libk::unexpected(EditError::BadPhysicalAddress);
    }
    static_cast<void>(TableRef::initialize(owned.bytes()));
    if (!tables_->attach(libk::move(owned))) {
        return libk::unexpected(EditError::CorruptTree);
    }
    return libk::expected(NewTable{page, *parent});
}

auto Editor::map(
    kernel::mm::VPage virtual_page,
    kernel::mm::Page physical_page,
    PtePerm permissions,
    kernel::mm::OwnedPageGroup& prepared) noexcept
    -> libk::Expected<void, EditError> {
    const auto page = Sv39VPage::from(virtual_page);
    const auto leaf = Pte::leaf_4k(physical_page, permissions);
    if (!page || !accepts(virtual_page)) {
        return libk::unexpected(EditError::BadAddress);
    }
    if (!leaf) {
        return libk::unexpected(EditError::BadPhysicalAddress);
    }

    auto root = TableRef::open(*tables_, root_);
    Pte& level2 = root.entry(page->level2_index());
    if (!level2.valid()) {
        auto level1 = take_table(prepared);
        if (!level1) {
            return libk::unexpected(level1.error());
        }
        auto level0 = take_table(prepared);
        if (!level0) {
            return libk::unexpected(level0.error());
        }
        auto l1 = TableRef::open(*tables_, level1.value().page);
        auto l0 = TableRef::open(*tables_, level0.value().page);
        l0.entry(page->level0_index()) = *leaf;
        l1.entry(page->level1_index()) = level0.value().parent;
        level2 = level1.value().parent;
        return libk::expected();
    }
    const auto level1_page = level2.next_table_page();
    if (!level1_page) {
        return libk::unexpected(EditError::CorruptTree);
    }
    auto level1 = TableRef::open(*tables_, *level1_page);
    Pte& level1_entry = level1.entry(page->level1_index());
    if (!level1_entry.valid()) {
        auto level0 = take_table(prepared);
        if (!level0) {
            return libk::unexpected(level0.error());
        }
        auto l0 = TableRef::open(*tables_, level0.value().page);
        l0.entry(page->level0_index()) = *leaf;
        level1_entry = level0.value().parent;
        return libk::expected();
    }
    const auto level0_page = level1_entry.next_table_page();
    if (!level0_page) {
        return libk::unexpected(EditError::CorruptTree);
    }
    auto level0 = TableRef::open(*tables_, *level0_page);
    Pte& entry = level0.entry(page->level0_index());
    if (entry.valid()) {
        return libk::unexpected(EditError::AlreadyMapped);
    }
    entry = *leaf;
    return libk::expected();
}

auto Editor::query(kernel::mm::VPage virtual_page) const noexcept
    -> libk::Expected<Leaf, EditError> {
    const auto page = Sv39VPage::from(virtual_page);
    if (!page || !accepts(virtual_page)) {
        return libk::unexpected(EditError::BadAddress);
    }
    const auto root = TableView::open(*tables_, root_);
    const auto level1_page =
        root.entry(page->level2_index()).next_table_page();
    if (!level1_page) {
        return libk::unexpected(EditError::NotMapped);
    }
    const auto level1 = TableView::open(*tables_, *level1_page);
    const auto level0_page =
        level1.entry(page->level1_index()).next_table_page();
    if (!level0_page) {
        return libk::unexpected(EditError::NotMapped);
    }
    const auto level0 = TableView::open(*tables_, *level0_page);
    const Pte entry = level0.entry(page->level0_index());
    if (!entry.valid()) {
        return libk::unexpected(EditError::NotMapped);
    }
    return leaf_from(entry);
}

auto Editor::protect(
    kernel::mm::VPage virtual_page,
    PtePerm permissions) noexcept -> libk::Expected<Leaf, EditError> {
    const auto current = query(virtual_page);
    if (!current) {
        return libk::unexpected(current.error());
    }
    const auto page = Sv39VPage::from(virtual_page);
    KASSERT(page);
    auto root = TableRef::open(*tables_, root_);
    const auto level1_page =
        root.entry(page->level2_index()).next_table_page();
    KASSERT(level1_page);
    auto level1 = TableRef::open(*tables_, *level1_page);
    const auto level0_page =
        level1.entry(page->level1_index()).next_table_page();
    KASSERT(level0_page);
    auto level0 = TableRef::open(*tables_, *level0_page);
    const auto replacement = Pte::leaf_4k(current.value().page, permissions);
    if (!replacement) {
        return libk::unexpected(EditError::BadPhysicalAddress);
    }
    level0.entry(page->level0_index()) = *replacement;
    return current;
}

auto Editor::unmap(kernel::mm::VPage virtual_page) noexcept
    -> libk::Expected<Unmapped, EditError> {
    const auto current = query(virtual_page);
    if (!current) {
        return libk::unexpected(current.error());
    }
    const auto page = Sv39VPage::from(virtual_page);
    KASSERT(page);
    auto root = TableRef::open(*tables_, root_);
    Pte& level2_entry = root.entry(page->level2_index());
    const kernel::mm::Page level1_page = *level2_entry.next_table_page();
    auto level1 = TableRef::open(*tables_, level1_page);
    Pte& level1_entry = level1.entry(page->level1_index());
    const kernel::mm::Page level0_page = *level1_entry.next_table_page();
    auto level0 = TableRef::open(*tables_, level0_page);
    level0.entry(page->level0_index()) = Pte{};

    DetachedTables detached{};
    if (level0.empty()) {
        level1_entry = Pte{};
        auto owner = tables_->detach(level0_page);
        KASSERT(owner);
        KASSERT(detached.pages_.try_push_back(libk::move(*owner)));
    }
    if (domain_ == Domain::User && level1.empty()) {
        level2_entry = Pte{};
        auto owner = tables_->detach(level1_page);
        KASSERT(owner);
        KASSERT(detached.pages_.try_push_back(libk::move(*owner)));
    }
    return libk::expected(Unmapped{
        current.value(), libk::move(detached)});
}

} // namespace arch::riscv64
