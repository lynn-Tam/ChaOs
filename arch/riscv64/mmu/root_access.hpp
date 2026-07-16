#pragma once

#include <arch/page_table.hpp>
#include <libk/utility.hpp>

namespace arch {

// Selected-backend access to public root ownership storage. This type is not
// part of the generic kernel contract and may differ in another backend.
struct RootAccess final {
    [[nodiscard]] static constexpr auto value(RootToken token) noexcept
        -> usize {
        return token.value_;
    }
    [[nodiscard]] static auto make_kernel(
        kernel::mm::Page root,
        kernel::mm::OwnedPageGroup&& tables) noexcept -> KernelRoot {
        return KernelRoot{root, libk::move(tables)};
    }
    [[nodiscard]] static auto make_user(
        kernel::mm::Page root,
        kernel::mm::OwnedPageGroup&& tables) noexcept -> UserRoot {
        return UserRoot{root, libk::move(tables)};
    }
    [[nodiscard]] static auto page(KernelRoot& root) noexcept -> kernel::mm::Page {
        return root.root_page_;
    }
    [[nodiscard]] static auto page(const KernelRoot& root) noexcept -> kernel::mm::Page {
        return root.root_page_;
    }
    [[nodiscard]] static auto tables(KernelRoot& root) noexcept
        -> kernel::mm::OwnedPageGroup& {
        return root.tables_;
    }
    [[nodiscard]] static auto tables(const KernelRoot& root) noexcept
        -> const kernel::mm::OwnedPageGroup& {
        return root.tables_;
    }
    [[nodiscard]] static auto page(UserRoot& root) noexcept -> kernel::mm::Page {
        return root.root_page_;
    }
    [[nodiscard]] static auto tables(UserRoot& root) noexcept
        -> kernel::mm::OwnedPageGroup& {
        return root.tables_;
    }
};

} // namespace arch
