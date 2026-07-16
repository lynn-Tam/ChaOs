#include <arch/page_table.hpp>

#include "arch/riscv64/cpu/csr.hpp"

#include <core/debug.hpp>
#include <arch/address_layout.hpp>
#include <libk/utility.hpp>
#include <mm/pmm.hpp>

#include "initial_kernel_map.hpp"
#include "sv39_builder.hpp"

namespace arch {
namespace {

[[nodiscard]] auto convert_initial_map_error(
    riscv64::InitialKernelMapError error) noexcept -> RootResult {
    switch (error) {
    case riscv64::InitialKernelMapError::InsufficientMemory:
        return libk::unexpected(RootError::InsufficientMemory);
    case riscv64::InitialKernelMapError::UnrepresentableAddress:
        return libk::unexpected(RootError::UnrepresentableAddress);
    }
    __builtin_unreachable();
}

[[nodiscard]] auto convert_builder_error(
    riscv64::MappingError error) noexcept -> RootResult {
    switch (error) {
    case riscv64::MappingError::AllocFailed:
        return libk::unexpected(RootError::InsufficientMemory);
    case riscv64::MappingError::BadPAddr:
    case riscv64::MappingError::BadVAddr:
        return libk::unexpected(RootError::UnrepresentableAddress);
    case riscv64::MappingError::MappingConflict:
        KASSERT(false);
    }
    __builtin_unreachable();
}

} // namespace

KernelRoot::KernelRoot(
    kernel::mm::Page root_page,
    kernel::mm::OwnedPageGroup&& tables) noexcept
    : root_page_(root_page), tables_(libk::move(tables)) {
    KASSERT(root_page_.valid());
    KASSERT(tables_);
}

KernelRoot::KernelRoot(KernelRoot&& other) noexcept
    : root_page_(libk::exchange(other.root_page_, empty_root())),
      tables_(libk::move(other.tables_)) {}

KernelRoot::operator bool() const noexcept {
    return root_page_.valid() && static_cast<bool>(tables_);
}

auto KernelRoot::token() const noexcept -> RootToken {
    KASSERT(*this);
    const auto value =
        riscv64::Satp::try_make_sv39(root_page_.frame().raw());
    KASSERT(value);
    return RootToken{*value};
}

auto build_kernel_root(kernel::mm::Pmm& pmm) noexcept -> RootResult {
    KASSERT(!riscv64::Sstatus::is_interrupts_enabled());

    auto builder_result = riscv64::Sv39Builder::create(pmm);
    if (!builder_result) {
        return convert_builder_error(builder_result.error());
    }
    auto builder = libk::move(builder_result).value();

    auto mapped = riscv64::map_initial_kernel(builder, pmm);
    if (!mapped) {
        return convert_initial_map_error(mapped.error());
    }

    // User roots borrow these exact supervisor branches.  Root entries never
    // need propagation after a user root is published; later kernel mappings
    // mutate the shared level-1/level-0 descendants in place.
    constexpr usize root_span = usize{1} << 30;
    for (usize index = 256; index < 512; ++index) {
        const auto page = kernel::mm::VPage::from_base(kernel::mm::VirtAddr{
            layout::direct_base + (index - 256) * root_span});
        KASSERT(page);
        const auto ensured = builder.ensure_root_branch(*page);
        if (!ensured) {
            return convert_builder_error(ensured.error());
        }
    }
    return libk::expected(libk::move(builder).finalize());
}

void activate_root(RootToken token) noexcept {
    KASSERT(!riscv64::Sstatus::is_interrupts_enabled());
    KASSERT(riscv64::Satp::mode(token.value_)
        == riscv64::Satp::MODE_SV39);
    riscv64::Satp::write(token.value_);
    flush_tlb_all();
    KASSERT(riscv64::Satp::read() == token.value_);
}

auto root_active(RootToken token) noexcept -> bool {
    return riscv64::Satp::read() == token.value_;
}

void flush_tlb_all() noexcept {
    asm volatile("sfence.vma x0, x0" ::: "memory");
}

} // namespace arch
