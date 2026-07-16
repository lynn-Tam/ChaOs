#pragma once

#include "arch/riscv64/mmu/sv39_editor.hpp"

namespace arch::backend {

using PageEditor = riscv64::Editor;
using PageEditError = riscv64::EditError;
using PageLeaf = riscv64::Leaf;
using DetachedTables = riscv64::DetachedTables;
using UnmappedPage = riscv64::Unmapped;
using PagePerm = riscv64::PtePerm;

[[nodiscard]] constexpr auto kernel_data_permissions() noexcept
    -> PagePerm {
    return PagePerm::supervisor_rw();
}

} // namespace arch::backend
