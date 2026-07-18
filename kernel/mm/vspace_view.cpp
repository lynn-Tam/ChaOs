#include <mm/vspace.hpp>

#include "vspace_internal.hpp"

#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <object/memory_pool.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

UserViewRelation::~UserViewRelation() noexcept {
    KASSERT(!mapping_hook_.is_linked());
    KASSERT(mapping_ == nullptr && !active_);
}

UserView::UserView(UserView&& other) noexcept
    : owner_(libk::exchange(other.owner_, nullptr)),
      relation_(libk::exchange(other.relation_, nullptr)),
      memory_(libk::move(other.memory_)),
      object_(other.object_),
      mapping_(other.mapping_),
      virtual_(other.virtual_),
      access_(other.access_) {}

auto UserView::operator=(UserView&& other) noexcept
    -> UserView& {
    if (this != &other) {
        reset();
        owner_ = libk::exchange(other.owner_, nullptr);
        relation_ = libk::exchange(other.relation_, nullptr);
        memory_ = libk::move(other.memory_);
        object_ = other.object_;
        mapping_ = other.mapping_;
        virtual_ = other.virtual_;
        access_ = other.access_;
    }
    return *this;
}

UserView::~UserView() noexcept {
    reset();
}

auto UserView::valid() const noexcept -> bool {
    return owner_ != nullptr && relation_ != nullptr
        && owner_->view_active(*relation_);
}

void UserView::reset() noexcept {
    VSpace* const owner = libk::exchange(owner_, nullptr);
    UserViewRelation* const relation = libk::exchange(relation_, nullptr);
    if (owner != nullptr) {
        KASSERT(relation != nullptr);
        owner->detach_view(*relation);
    }
    memory_.reset();
}

auto VSpace::bind_view(UserViewRequest&& request) noexcept
    -> libk::Expected<UserView, VSpaceError> {
    const auto page_count = request.virtual_range.page_count();
    if (!request.memory || !valid_access(request.access)
        || !valid_user_range(request.virtual_range)
        || !page_count || request.object.page_count != *page_count) {
        return libk::unexpected(VSpaceError::InvalidRange);
    }
    auto memory_pin = request.memory.pin<MemoryObject>();
    if (!memory_pin) {
        return libk::unexpected(VSpaceError::InvalidAuthority);
    }
    MemoryObject& memory = memory_pin.value().get();
    Mapping* mapping{};
    auto validate = [&]() noexcept -> bool {
        AddressRegion* region = root_region_;
        LayoutNode* node{};
        while (region != nullptr) {
            node = region->children_.lower_bound(
                request.virtual_range.base());
            if (node == nullptr
                || node->range_.base() > request.virtual_range.base()) {
                node = node != nullptr
                    ? region->children_.previous(*node)
                    : region->children_.maximum();
            }
            if (node == nullptr
                || !node->range_.contains(request.virtual_range)) {
                return false;
            }
            if (node->kind_ != LayoutKind::Region) {
                break;
            }
            region = static_cast<AddressRegion*>(node);
        }
        if (node == nullptr || node->kind_ != LayoutKind::Mapping) {
            return false;
        }
        auto* const current = static_cast<Mapping*>(node);
        if (current->state_ != MappingState::Live
            || !current->range_.contains(request.virtual_range)
            || !current->access_.contains(request.access)
            || &current->authority_->memory() != &memory) {
            return false;
        }
        const auto page_offset =
            current->range_.page_offset(request.virtual_range.base());
        KASSERT(page_offset);
        const ObjectRange expected{
            current->object_.first + *page_offset,
            *page_count};
        if (expected != request.object) {
            return false;
        }
        mapping = current;
        return true;
    };
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!validate()) {
            return libk::unexpected(VSpaceError::InvalidMapping);
        }
        auto claimed = begin_claim(
            *mapping->parent_, request.virtual_range, false);
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
    }

    auto made = views_.create(*mapping);
    if (!made) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(node_error(made.error()));
    }
    UserViewRelation* const relation = made.value().object;
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (claim_.region != mapping->parent_
            || claim_.range != request.virtual_range
            || !validate()) {
            release_claim();
            views_.destroy(*relation);
            return libk::unexpected(VSpaceError::InvalidMapping);
        }
        relation->mapping_ = mapping;
        relation->active_ = true;
        mapping->views_.push_back(*relation);
        release_claim();
    }
    return libk::expected(UserView{
        *this,
        *relation,
        libk::move(request.memory),
        request.object,
        mapping->key_,
        request.virtual_range,
        request.access});
}

void VSpace::detach_view(UserViewRelation& relation) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (relation.mapping_hook_.is_linked()) {
            KASSERT(relation.mapping_ != nullptr);
            relation.mapping_->views_.erase(relation);
        }
        relation.mapping_ = nullptr;
        relation.active_ = false;
    }
    views_.destroy(relation);
}

auto VSpace::view_active(const UserViewRelation& relation) const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return relation.active_ && relation.mapping_ != nullptr
        && relation.mapping_hook_.is_linked();
}

void VSpace::invalidate_views(Mapping& mapping) noexcept {
    while (!mapping.views_.empty()) {
        UserViewRelation& relation = mapping.views_.front();
        mapping.views_.erase(relation);
        relation.mapping_ = nullptr;
        relation.active_ = false;
    }
}

} // namespace kernel::mm
