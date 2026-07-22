#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/noncopyable.hpp>
#include <sync/lock.hpp>
#include <mm/node_pool.hpp>
#include <mm/permissions.hpp>
#include <mm/pmm.hpp>

namespace kernel::mm {

enum class AliasError : u8 {
    ConflictingType,
    OutOfMemory,
    QuotaExceeded,
};

class PhysicalAliasRegistry;

class AliasLease final : private libk::noncopyable {
public:
    AliasLease() noexcept = default;
    AliasLease(AliasLease&& other) noexcept;
    auto operator=(AliasLease&& other) noexcept -> AliasLease&;
    ~AliasLease() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }
    [[nodiscard]] auto page() const noexcept -> Page { return page_; }
    [[nodiscard]] auto type() const noexcept -> MemoryType { return type_; }

    // Transfers the claim from the prepare transaction to a live PTE. The
    // invalidation path later calls PhysicalAliasRegistry::release().
    void commit() noexcept;
    void reset() noexcept;

private:
    friend class PhysicalAliasRegistry;
    AliasLease(
        PhysicalAliasRegistry& owner,
        Page page,
        MemoryType type) noexcept
        : owner_(&owner), page_(page), type_(type) {}

    PhysicalAliasRegistry* owner_{};
    Page page_{};
    MemoryType type_{MemoryType::Normal};
};

// System-wide truth for concurrently materialized physical cache attributes.
// MemoryObject validates backing policy; this registry arbitrates actual PTE
// aliases across every user VSpace and the dynamic KernelVSpace path.
class PhysicalAliasRegistry final : private libk::noncopyable_nonmovable {
    struct Claim final {
        Claim(Page physical, MemoryType memory_type) noexcept
            : page(physical), type(memory_type) {}

        Page page{};
        MemoryType type{MemoryType::Normal};
        usize refs{1};
        libk::IntrusiveTreeHook hook{};
    };

    struct Compare final {
        [[nodiscard]] constexpr auto operator()(
            const Claim& lhs,
            const Claim& rhs) const noexcept -> bool {
            return lhs.page < rhs.page;
        }
        [[nodiscard]] constexpr auto operator()(
            Page lhs,
            const Claim& rhs) const noexcept -> bool {
            return lhs < rhs.page;
        }
        [[nodiscard]] constexpr auto operator()(
            const Claim& lhs,
            Page rhs) const noexcept -> bool {
            return lhs.page < rhs;
        }
    };

    using Tree = libk::IntrusiveTree<Claim, &Claim::hook, Compare>;

public:
    explicit PhysicalAliasRegistry(Pmm& pmm) noexcept : claims_(pmm) {}
    ~PhysicalAliasRegistry() noexcept;

    [[nodiscard]] auto acquire(Page page, MemoryType type) noexcept
        -> libk::Expected<AliasLease, AliasError>;
    void release(Page page, MemoryType type) noexcept;

    [[nodiscard]] auto type_of(Page page) const noexcept
        -> libk::optional<MemoryType>;
    [[nodiscard]] auto active_pages() const noexcept -> usize;

private:
    friend class AliasLease;

    mutable kernel::sync::SpinLock<kernel::sync::LockClass::PhysicalAlias>
        lock_{};
    NodePool<Claim> claims_;
    Tree tree_{};
};

} // namespace kernel::mm
