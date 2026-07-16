#pragma once

#include "sv39.hpp"

#include <arch/page_table.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <mm/permissions.hpp>
#include <mm/pmm.hpp>

namespace arch::riscv64 {

enum class EditError : u8 {
    BadAddress,
    BadPhysicalAddress,
    AllocationFailed,
    AlreadyMapped,
    NotMapped,
    CorruptTree,
};

struct Leaf final {
    kernel::mm::Page page{};
    PtePerm permissions{PtePerm::supervisor_ro()};
};

class DetachedTables final {
public:
    DetachedTables() noexcept = default;
    DetachedTables(const DetachedTables&) = delete;
    auto operator=(const DetachedTables&) -> DetachedTables& = delete;
    DetachedTables(DetachedTables&&) noexcept = default;
    auto operator=(DetachedTables&&) noexcept -> DetachedTables& = default;

    [[nodiscard]] auto empty() const noexcept -> bool {
        return pages_.empty();
    }
    [[nodiscard]] auto size() const noexcept -> usize {
        return pages_.size();
    }
    [[nodiscard]] auto take() noexcept -> libk::optional<kernel::mm::OwnedPage> {
        if (pages_.empty()) {
            return libk::nullopt;
        }
        kernel::mm::OwnedPage page{libk::move(pages_[pages_.size() - 1])};
        [[maybe_unused]] const bool popped = pages_.try_pop_back();
        return libk::optional<kernel::mm::OwnedPage>{libk::move(page)};
    }

private:
    friend class Editor;
    libk::InplaceVector<kernel::mm::OwnedPage, 2> pages_{};
};

struct Unmapped final {
    Leaf leaf;
    DetachedTables tables;
};

class Editor final : private libk::noncopyable {
public:
    // Architecture-owned upper bound for table resources consumed by one
    // ordered mapping transaction. Existing tables may leave pages unused;
    // the caller retains those pages for rollback.
    class Plan final {
    public:
        [[nodiscard]] auto include(kernel::mm::VPage page) noexcept -> bool;
        [[nodiscard]] auto table_pages() const noexcept -> usize {
            return table_pages_;
        }

    private:
        usize previous_vpn_{};
        usize previous_level2_{};
        usize previous_level1_{};
        usize table_pages_{};
        bool empty_{true};
    };

    Editor(Editor&&) noexcept = default;
    auto operator=(Editor&&) -> Editor& = delete;

    [[nodiscard]] static auto kernel(arch::KernelRoot& root) noexcept
        -> Editor;
    [[nodiscard]] static auto user(arch::UserRoot& root) noexcept
        -> Editor;
    [[nodiscard]] static auto user_permissions(
        kernel::mm::AccessMask access,
        kernel::mm::MemoryType type) noexcept -> libk::optional<PtePerm>;

    [[nodiscard]] auto map(
        kernel::mm::VPage virtual_page,
        kernel::mm::Page physical_page,
        PtePerm permissions) noexcept -> libk::Expected<void, EditError>;

    // Commit path: every table page that may be needed was allocated before
    // translation serialization. The editor consumes only the required
    // prefix; unused reserve pages remain with the caller for rollback.
    [[nodiscard]] auto map(
        kernel::mm::VPage virtual_page,
        kernel::mm::Page physical_page,
        PtePerm permissions,
        kernel::mm::OwnedPageGroup& prepared) noexcept
        -> libk::Expected<void, EditError>;

    [[nodiscard]] auto unmap(kernel::mm::VPage page) noexcept
        -> libk::Expected<Unmapped, EditError>;

    [[nodiscard]] auto protect(
        kernel::mm::VPage page,
        PtePerm permissions) noexcept -> libk::Expected<Leaf, EditError>;

    [[nodiscard]] auto query(kernel::mm::VPage page) const noexcept
        -> libk::Expected<Leaf, EditError>;

private:
    enum class Domain : u8 { User, Kernel };
    struct NewTable final {
        kernel::mm::Page page;
        Pte parent;
    };

    Editor(
        kernel::mm::Page root,
        kernel::mm::OwnedPageGroup& tables,
        Domain domain) noexcept
        : root_(root), tables_(&tables), domain_(domain) {}

    [[nodiscard]] auto accepts(kernel::mm::VPage page) const noexcept -> bool;
    [[nodiscard]] auto take_table(kernel::mm::OwnedPageGroup& prepared) noexcept
        -> libk::Expected<NewTable, EditError>;

    kernel::mm::Page root_{};
    kernel::mm::OwnedPageGroup* tables_{};
    Domain domain_{Domain::User};
};

} // namespace arch::riscv64
