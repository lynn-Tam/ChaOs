#pragma once

#include "sv39.hpp"

#include <core/debug.hpp>
#include <libk/noncopyable.hpp>
#include <libk/new.hpp>
#include <mm/pmm.hpp>

namespace arch::riscv64 {

class TableRef final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] static auto initialize(byte* storage) noexcept -> TableRef {
        KASSERT(storage != nullptr);
        auto* const table = ::new (storage)
            TablePage{TablePage::ConstructionKey{}};
        return TableRef{*table};
    }

    [[nodiscard]] static auto open(
        kernel::mm::OwnedPageGroup& owner,
        kernel::mm::Page page) noexcept -> TableRef {
        return TableRef{*reinterpret_cast<TablePage*>(owner.bytes(page))};
    }

    [[nodiscard]] auto entry(usize index) noexcept -> Pte& {
        KASSERT(index < ptes_per_pg);
        return table_.entries_[index];
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        for (const Pte entry : table_.entries_) {
            if (entry.valid()) {
                return false;
            }
        }
        return true;
    }

private:
    explicit TableRef(TablePage& table) noexcept : table_(table) {}
    TablePage& table_;
};

class TableView final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] static auto open(
        const kernel::mm::OwnedPageGroup& owner,
        kernel::mm::Page page) noexcept -> TableView {
        return TableView{
            *reinterpret_cast<const TablePage*>(owner.bytes(page))};
    }

    [[nodiscard]] auto entry(usize index) const noexcept -> const Pte& {
        KASSERT(index < ptes_per_pg);
        return table_.entries_[index];
    }

private:
    explicit TableView(const TablePage& table) noexcept : table_(table) {}
    const TablePage& table_;
};

} // namespace arch::riscv64
