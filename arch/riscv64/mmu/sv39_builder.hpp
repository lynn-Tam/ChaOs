#pragma once
#include <stddef.h>
#include <stdint.h>

#include <libk/expected.hpp>

#include <arch/page_table.hpp>
#include <mm/addr.hpp>
#include <mm/pmm.hpp>

#include "sv39.hpp"

namespace arch::riscv64 {
enum class MappingError : uint8_t {
    BadVAddr,
    BadPAddr,
    AllocFailed,
    MappingConflict,
};

class Sv39Builder final {
  public:
    using CreateResult = libk::Expected<Sv39Builder, MappingError>;
    using MapResult = libk::Expected<void, MappingError>;

    [[nodiscard]] static auto create(kernel::mm::Pmm& pmm) noexcept -> CreateResult;

    Sv39Builder(const Sv39Builder&) = delete;
    auto operator=(const Sv39Builder&) -> Sv39Builder& = delete;

    Sv39Builder(Sv39Builder&& other) noexcept;
    auto operator=(Sv39Builder&&) -> Sv39Builder& = delete;

    ~Sv39Builder() noexcept = default;

    [[nodiscard]] auto root_page() const noexcept -> kernel::mm::Page;

    [[nodiscard]] auto map_page(kernel::mm::VPage virtual_page, kernel::mm::Page physical_page,
                                PtePerm permissions) noexcept -> MapResult;

    // Publishes one owned level-1 branch without creating a leaf.  The kernel
    // builder uses this to freeze all supervisor root slots before any user
    // root borrows them.
    [[nodiscard]] auto ensure_root_branch(kernel::mm::VPage page) noexcept -> MapResult;

    // Walks the detached builder's current tables without mutation so the
    // complete identity and permission policy can be checked before root
    // publication. It is not an active-table or hardware-translation API.
    [[nodiscard]] auto mapping_at(kernel::mm::VPage virtual_page) const noexcept -> libk::optional<Pte>;

    [[nodiscard]] auto finalize() && noexcept -> arch::KernelRoot;

  private:
    Sv39Builder(kernel::mm::Page root_page, kernel::mm::OwnedPageGroup&& page_tables) noexcept;

    kernel::mm::Page root_page_{};
    kernel::mm::OwnedPageGroup page_tables_;
};

static_assert(sizeof(Sv39Builder) <= 48);
} // namespace arch::riscv64
