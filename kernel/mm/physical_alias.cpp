#include <mm/physical_alias.hpp>

#include <core/debug.hpp>
#include <libk/limits.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

AliasLease::AliasLease(AliasLease&& other) noexcept
    : owner_(libk::exchange(other.owner_, nullptr)),
      page_(other.page_),
      type_(other.type_) {}

auto AliasLease::operator=(AliasLease&& other) noexcept -> AliasLease& {
    if (this != &other) {
        reset();
        owner_ = libk::exchange(other.owner_, nullptr);
        page_ = other.page_;
        type_ = other.type_;
    }
    return *this;
}

AliasLease::~AliasLease() noexcept {
    reset();
}

void AliasLease::commit() noexcept {
    KASSERT(owner_ != nullptr);
    owner_ = nullptr;
    page_ = {};
}

void AliasLease::reset() noexcept {
    PhysicalAliasRegistry* const owner = libk::exchange(owner_, nullptr);
    const Page page = page_;
    const MemoryType type = type_;
    page_ = {};
    if (owner != nullptr) {
        owner->release(page, type);
    }
}

PhysicalAliasRegistry::~PhysicalAliasRegistry() noexcept {
    KASSERT(tree_.empty());
    KASSERT(claims_.live_count() == 0);
}

auto PhysicalAliasRegistry::acquire(Page page, MemoryType type) noexcept
    -> libk::Expected<AliasLease, AliasError> {
    if (!page.valid()) {
        return libk::unexpected(AliasError::ConflictingType);
    }

    {
        kernel::sync::IrqLockGuard guard{lock_};
        Claim* const current = tree_.find(page);
        if (current != nullptr) {
            if (current->type != type) {
                return libk::unexpected(AliasError::ConflictingType);
            }
            if (current->refs == libk::numeric_limits<usize>::max()) {
                return libk::unexpected(AliasError::QuotaExceeded);
            }
            ++current->refs;
            return libk::expected(AliasLease{*this, page, type});
        }
    }

    auto made = claims_.create(page, type);
    if (!made) {
        return libk::unexpected(
            made.error() == NodePoolError::OutOfMemory
                ? AliasError::OutOfMemory
                : AliasError::QuotaExceeded);
    }
    Claim* const candidate = made.value().object;
    Claim* discard{};
    AliasError error{AliasError::ConflictingType};
    bool accepted{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Claim* const current = tree_.find(page);
        if (current == nullptr) {
            tree_.insert(*candidate);
            accepted = true;
        } else {
            discard = candidate;
            if (current->type == type
                && current->refs != libk::numeric_limits<usize>::max()) {
                ++current->refs;
                accepted = true;
            } else if (current->type == type) {
                error = AliasError::QuotaExceeded;
            }
        }
    }
    if (discard != nullptr) {
        claims_.destroy(*discard);
    }
    if (!accepted) {
        return libk::unexpected(error);
    }
    return libk::expected(AliasLease{*this, page, type});
}

void PhysicalAliasRegistry::release(Page page, MemoryType type) noexcept {
    Claim* retired{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Claim* const claim = tree_.find(page);
        KASSERT(claim != nullptr);
        KASSERT(claim->type == type);
        KASSERT(claim->refs != 0);
        --claim->refs;
        if (claim->refs == 0) {
            tree_.erase(*claim);
            retired = claim;
        }
    }
    if (retired != nullptr) {
        claims_.destroy(*retired);
    }
}

auto PhysicalAliasRegistry::type_of(Page page) const noexcept
    -> libk::optional<MemoryType> {
    kernel::sync::IrqLockGuard guard{lock_};
    const Claim* const claim = tree_.find(page);
    return claim != nullptr
        ? libk::optional<MemoryType>{claim->type}
        : libk::nullopt;
}

auto PhysicalAliasRegistry::active_pages() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return tree_.size();
}

} // namespace kernel::mm
