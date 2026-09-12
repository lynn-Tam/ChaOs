#pragma once

#include <libk/expected.hpp>
#include <libk/delegate.hpp>
#include <libk/span.hpp>
#include <mm/pmm.hpp>

namespace arch::riscv64 { class Sv39Builder; }

namespace arch {

enum class IoRootError : u8 { InvalidRange, InsufficientMemory };

// An immutable I/O translation tree. It owns table pages only; IOSpace retains
// every mapped MemoryObject PageLease until device drain and IOMMU invalidation
// complete. It never borrows CPU supervisor mappings or mints a CPU RootToken.
class IoRoot final {
public:
    using PageSource = libk::delegate<kernel::mm::Page() noexcept>;
    IoRoot(const IoRoot&) = delete;
    auto operator=(const IoRoot&) -> IoRoot& = delete;
    IoRoot(IoRoot&& other) noexcept;
    auto operator=(IoRoot&&) -> IoRoot& = delete;
    ~IoRoot() noexcept = default;

    [[nodiscard]] static auto create(
        kernel::mm::Pmm& pmm, usize first,
        libk::Span<const kernel::mm::Page> pages, bool writable) noexcept
        -> libk::Expected<IoRoot, IoRootError>;
    // Sequential, allocation-free borrow; called exactly count times on
    // success and never retained by the tree. The caller owns backing pins.
    [[nodiscard]] static auto create(
        kernel::mm::Pmm& pmm, usize first, usize count,
        PageSource pages, bool writable) noexcept
        -> libk::Expected<IoRoot, IoRootError>;
    [[nodiscard]] static auto required_pages(usize first, usize count) noexcept
        -> libk::Expected<usize, IoRootError>;
    [[nodiscard]] auto page() const noexcept -> kernel::mm::Page { return root_; }
    [[nodiscard]] auto page_count() const noexcept -> usize {
        return tables_.page_count();
    }

private:
    friend class riscv64::Sv39Builder;
    IoRoot(kernel::mm::Page root, kernel::mm::OwnedPageGroup&& tables) noexcept;
    kernel::mm::Page root_{};
    kernel::mm::OwnedPageGroup tables_{};
};

} // namespace arch
