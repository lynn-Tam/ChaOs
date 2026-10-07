#pragma once

#include <array>
#include <expected>
#include <utility>
#include <libk/inplace_vector.hpp>
#include <libk/span.hpp>
#include <mm/pmm.hpp>
#include <mm/types.hpp>
#include <pte.hpp>
#include <uapi/arch/riscv64/address_space.h>

namespace mm {
inline constexpr usize UserBegin = RISCV64_LOW_GUARD_END;
inline constexpr usize UserEnd = arch::PtUserEnd;
inline constexpr usize DirectBegin = arch::PtKernelBegin;
inline constexpr usize DirectSize = 128ULL * 1024 * 1024 * 1024;
inline constexpr usize DirectEnd = DirectBegin + DirectSize;
inline constexpr usize DynamicBegin = DirectEnd;
constexpr bool is_user(Virt a) noexcept { return a.raw() >= UserBegin && a.raw() < UserEnd; }
static_assert(UserBegin < UserEnd && DirectEnd > DirectBegin);
using arch::PtPerm;
enum class PtErr : u8 { BadAddr, BadPhys, NoMemory, Exists, Missing, Corrupt };
struct PtLeaf {
    mm::Page page;
    PtPerm perms;
};
struct PageUsage {
    bool accessed{}, dirty{};
};
struct PtRemoved {
    PageUsage usage;
    // Ownership transfers to the caller's retirement batch, after shootdown.
    libk::InplaceVector<mm::OwnedPage, arch::PtLevels - 1> tables;
};

// Caller serializes tree changes. Hardware may concurrently update leaf A/D.
// User roots borrow the kernel's permanent upper branches; DMA roots borrow none.
class PageTable {
  public:
    enum class Kind : u8 { Kernel, User, Io };
    PageTable(const PageTable &) = delete;
    auto operator=(const PageTable &) -> PageTable & = delete;
    PageTable(PageTable &&) noexcept;
    auto operator=(PageTable &&) -> PageTable & = delete;
    static auto create(mm::Pmm &, Kind, const PageTable *kernel = nullptr) noexcept
        -> std::expected<PageTable, PtErr>;
    auto kind() const noexcept -> Kind { return kind_; }
    explicit operator bool() const noexcept { return bool(tables_); }
    auto page() const noexcept -> mm::Page {
        libk_assert(*this);
        return root_;
    }
    auto cpu_root() const noexcept -> usize;
    auto page_count() const noexcept -> usize { return tables_.page_count(); }

    // Exact missing-table count for an ordered set under stable serialization.
    class Count {
      public:
        bool include(mm::VPage) noexcept;
        auto pages() const noexcept -> usize { return pages_; }

      private:
        friend class PageTable;
        explicit Count(const PageTable &table) noexcept : table_(&table) {}
        const PageTable *table_;
        usize previous_{~usize{}}, pages_{};
    };
    auto count() const noexcept -> Count { return Count{*this}; }
    static auto user_perms(mm::Perms) noexcept -> std::optional<PtPerm>;
    // No allocation in commit: consumes only missing tables from the reserve.
    auto map(mm::VPage, mm::Page, PtPerm, mm::PageGroup &) noexcept -> std::expected<void, PtErr>;
    auto query(mm::VPage) const noexcept -> std::expected<PtLeaf, PtErr>;
    auto usage(mm::VPage) const noexcept -> std::expected<PageUsage, PtErr>;
    auto clear_usage(mm::VPage) noexcept -> std::expected<PageUsage, PtErr>;
    auto protect(mm::VPage, PtPerm) noexcept -> std::expected<PageUsage, PtErr>;
    auto replace(mm::VPage, mm::Page, PtPerm) noexcept -> std::expected<PageUsage, PtErr>;
    auto unmap(mm::VPage) noexcept -> std::expected<PtRemoved, PtErr>;

    // A fresh range: caller keeps it unpublished and serializes tree changes.
    // Failure removes this invocation's leaves and returns all new tables.
    template<class Source>
    auto map(mm::VPage first, usize n, Source next, PtPerm perms) noexcept
        -> std::expected<void, PtErr> {
        if (!n || !first.checked_add(n - 1)) return std::unexpected(PtErr::BadAddr);
        auto plan = count();
        for (usize i = 0; i < n; ++i)
            if (!plan.include(*first.checked_add(i))) return std::unexpected(PtErr::BadAddr);
        auto reserve = tables_.owner().group();
        if (!reserve.grow(plan.pages())) return std::unexpected(PtErr::NoMemory);
        for (usize i = 0; i < n; ++i) {
            auto added = map(*first.checked_add(i), next(), perms, reserve);
            if (!added) {
                while (i) { auto removed = unmap(*first.checked_add(--i)); libk_assert(removed); }
                return added;
            }
        }
        return {};
    }
    auto map(mm::Virt va, mm::Pages pages, PtPerm perms) noexcept -> std::expected<void, PtErr> {
        auto first = mm::VPage::from_base(va);
        if (!first || !pages.valid()) return std::unexpected(PtErr::BadAddr);
        usize i{};
        return map(*first, pages.page_count(), [&] { return *pages.base().checked_add(i++); }, perms);
    }

    static auto dma_pages(usize first, usize count) noexcept -> std::expected<usize, PtErr>;
    template <class Source>
    static auto dma(mm::Pmm &pmm, usize first, usize n, Source next, bool writable) noexcept
        -> std::expected<PageTable, PtErr> {
        auto pages = dma_pages(first, n);
        if (!pages) return std::unexpected(pages.error());
        auto root = create(pmm, Kind::Io);
        if (!root) return root;
        auto va = mm::VPage::from_base(mm::Virt{first});
        auto added = root->map(*va, n, std::move(next), writable ? PtPerm::UserRw : PtPerm::UserRo);
        if (!added) return std::unexpected(added.error());
        return root;
    }
    static auto dma(mm::Pmm &pmm, usize first, libk::Span<const mm::Page> pages, bool writable) noexcept
        -> std::expected<PageTable, PtErr> {
        usize i{};
        return dma(pmm, first, pages.size(), [&]() noexcept { return pages[i++]; }, writable);
    }

  private:
    PageTable(mm::Page root, mm::PageGroup &&tables, Kind kind) noexcept
        : root_(root), tables_(std::move(tables)), kind_(kind) {}
    struct Walk {
        std::array<mm::Page, arch::PtLevels> pages{};
        std::array<arch::Pte *, arch::PtLevels> entries{};
        int missing{-1};
    };
    auto walk(mm::VPage) const noexcept -> std::expected<Walk, PtErr>;
    auto leaf(mm::VPage) const noexcept -> std::expected<arch::Pte *, PtErr>;
    auto table(mm::Page) const noexcept -> arch::Pte *;
    auto encode(mm::Page, PtPerm) const noexcept -> std::expected<arch::Pte, PtErr>;
    mm::Page root_;
    mm::PageGroup tables_;
    Kind kind_;
};


} // namespace mm
