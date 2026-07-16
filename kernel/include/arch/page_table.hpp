#pragma once

#include <libk/expected.hpp>
#include <libk/limits.hpp>
#include <mm/pmm.hpp>

namespace arch {

struct RootAccess;

enum class RootError : u8 {
    InsufficientMemory,
    UnrepresentableAddress,
    TranslationModeUnavailable,
};

// A CPU-consumable projection of one translation root.  It neither owns nor
// extends the lifetime of the root from which it was minted.
class RootToken final {
public:
    constexpr RootToken() noexcept = default;
    RootToken(const RootToken&) noexcept = default;
    auto operator=(const RootToken&) noexcept -> RootToken& = default;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value_ != 0;
    }

    [[nodiscard]] friend constexpr auto operator==(
        RootToken lhs, RootToken rhs) noexcept -> bool {
        return lhs.value_ == rhs.value_;
    }

private:
    friend class KernelRoot;
    friend class UserRoot;
    friend struct RootAccess;
    friend void activate_root(RootToken token) noexcept;
    friend auto root_active(RootToken token) noexcept -> bool;

    explicit constexpr RootToken(usize value) noexcept : value_(value) {}
    usize value_{};
};

// Architecture-owned resources behind the supervisor root.  KernelVSpace is
// the semantic owner; user roots later borrow its shared supervisor branches
// without acquiring authority to release them.
class KernelRoot final {
public:
    KernelRoot(const KernelRoot&) = delete;
    auto operator=(const KernelRoot&) -> KernelRoot& = delete;

    KernelRoot(KernelRoot&& other) noexcept;
    auto operator=(KernelRoot&&) -> KernelRoot& = delete;
    ~KernelRoot() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto token() const noexcept -> RootToken;

private:
    friend struct RootAccess;
    friend class UserRoot;

    KernelRoot(kernel::mm::Page root_page, kernel::mm::OwnedPageGroup&& tables) noexcept;

    [[nodiscard]] static constexpr auto empty_root() noexcept -> kernel::mm::Page {
        return kernel::mm::Page{kernel::mm::Pfn{libk::numeric_limits<usize>::max()}};
    }

    kernel::mm::Page root_page_{};
    kernel::mm::OwnedPageGroup tables_;
};

// A user root owns its root and all low-half table pages.  Its supervisor root
// entries are borrowed pointers to KernelRoot-owned branches and are never
// released by this owner.
class UserRoot final {
public:
    UserRoot(const UserRoot&) = delete;
    auto operator=(const UserRoot&) -> UserRoot& = delete;

    UserRoot(UserRoot&& other) noexcept;
    auto operator=(UserRoot&&) -> UserRoot& = delete;
    ~UserRoot() noexcept = default;

    [[nodiscard]] static auto create(
        const KernelRoot& shared,
        kernel::mm::Pmm& pmm) noexcept -> libk::Expected<UserRoot, RootError>;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto token() const noexcept -> RootToken;

private:
    friend struct RootAccess;

    UserRoot(kernel::mm::Page root_page, kernel::mm::OwnedPageGroup&& tables) noexcept;

    [[nodiscard]] static constexpr auto empty_root() noexcept -> kernel::mm::Page {
        return kernel::mm::Page{kernel::mm::Pfn{libk::numeric_limits<usize>::max()}};
    }

    kernel::mm::Page root_page_{};
    kernel::mm::OwnedPageGroup tables_;
};

using RootResult = libk::Expected<KernelRoot, RootError>;

[[nodiscard]] auto build_kernel_root(kernel::mm::Pmm& pmm) noexcept -> RootResult;

// Installs a fully built root and performs the backend-required local fence.
// The caller owns the activation/leave serialization.
void activate_root(RootToken token) noexcept;
[[nodiscard]] auto root_active(RootToken token) noexcept -> bool;
void flush_tlb_all() noexcept;

static_assert(sizeof(KernelRoot) <= 48);
static_assert(sizeof(UserRoot) <= 48);
static_assert(sizeof(RootToken) == sizeof(usize));

} // namespace arch
