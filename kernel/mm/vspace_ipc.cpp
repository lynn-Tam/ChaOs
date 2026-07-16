#include <mm/vspace.hpp>

#include "vspace_internal.hpp"

#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <object/memory_pool.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

IpcRelation::~IpcRelation() noexcept {
    KASSERT(!mapping_hook_.is_linked());
    KASSERT(mapping_ == nullptr && !active_);
}

IpcBufferBinding::IpcBufferBinding(IpcBufferBinding&& other) noexcept
    : owner_(libk::exchange(other.owner_, nullptr)),
      relation_(libk::exchange(other.relation_, nullptr)),
      memory_(libk::move(other.memory_)),
      object_(other.object_),
      mapping_(other.mapping_),
      virtual_(other.virtual_),
      access_(other.access_) {}

auto IpcBufferBinding::operator=(IpcBufferBinding&& other) noexcept
    -> IpcBufferBinding& {
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

IpcBufferBinding::~IpcBufferBinding() noexcept {
    reset();
}

auto IpcBufferBinding::valid() const noexcept -> bool {
    return owner_ != nullptr && relation_ != nullptr
        && owner_->ipc_active(*relation_);
}

void IpcBufferBinding::reset() noexcept {
    VSpace* const owner = libk::exchange(owner_, nullptr);
    IpcRelation* const relation = libk::exchange(relation_, nullptr);
    if (owner != nullptr) {
        KASSERT(relation != nullptr);
        owner->detach_ipc(*relation);
    }
    memory_.reset();
}

auto VSpace::bind_ipc(IpcBufferRequest&& request) noexcept
    -> libk::Expected<IpcBufferBinding, VSpaceError> {
    const auto page_count = request.virtual_range.page_count();
    if (!request.memory || !request.mapping.valid()
        || !valid_access(request.access)
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
        Mapping* const current = mappings_.find(request.mapping.node);
        if (current == nullptr || current->key_ != request.mapping
            || current->state_ != MappingState::Live
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

    auto made = ipc_relations_.create(*mapping);
    if (!made) {
        kernel::sync::IrqLockGuard guard{lock_};
        release_claim();
        return libk::unexpected(node_error(made.error()));
    }
    IpcRelation* const relation = made.value().object;
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (claim_.region != mapping->parent_
            || claim_.range != request.virtual_range
            || !validate()) {
            release_claim();
            ipc_relations_.destroy(*relation);
            return libk::unexpected(VSpaceError::InvalidMapping);
        }
        relation->mapping_ = mapping;
        relation->active_ = true;
        mapping->ipc_relations_.push_back(*relation);
        release_claim();
    }
    return libk::expected(IpcBufferBinding{
        *this,
        *relation,
        libk::move(request.memory),
        request.object,
        request.mapping,
        request.virtual_range,
        request.access});
}

void VSpace::detach_ipc(IpcRelation& relation) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (relation.mapping_hook_.is_linked()) {
            KASSERT(relation.mapping_ != nullptr);
            relation.mapping_->ipc_relations_.erase(relation);
        }
        relation.mapping_ = nullptr;
        relation.active_ = false;
    }
    ipc_relations_.destroy(relation);
}

auto VSpace::ipc_active(const IpcRelation& relation) const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return relation.active_ && relation.mapping_ != nullptr
        && relation.mapping_hook_.is_linked();
}

void VSpace::invalidate_ipc(Mapping& mapping) noexcept {
    while (!mapping.ipc_relations_.empty()) {
        IpcRelation& relation = mapping.ipc_relations_.front();
        mapping.ipc_relations_.erase(relation);
        relation.mapping_ = nullptr;
        relation.active_ = false;
    }
}

} // namespace kernel::mm
