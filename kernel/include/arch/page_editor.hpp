#pragma once

#include <arch/backend/page_editor.hpp>

namespace arch {

using PageEditor = backend::PageEditor;
using PageEditError = backend::PageEditError;
using PageLeaf = backend::PageLeaf;
using DetachedTables = backend::DetachedTables;
using UnmappedPage = backend::UnmappedPage;
using PagePerm = backend::PagePerm;

[[nodiscard]] constexpr auto kernel_data_permissions() noexcept
    -> PagePerm {
    return backend::kernel_data_permissions();
}

} // namespace arch
