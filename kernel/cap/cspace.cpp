#include <cap/cspace.hpp>

#include <cpu/cpu_registry.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>

namespace kernel::cap {

CSpace::CSpace(kernel::mm::Pmm& pmm) noexcept
    : CSpace(pmm, Quota{}) {}

CSpace::CSpace(kernel::mm::Pmm& pmm, Quota quota) noexcept
    : pmm_(&pmm), pages_(pmm.make_page_group()), quota_(quota) {}

CSpace::Reservation::Reservation(Reservation&& other) noexcept
    : owner_(libk::exchange(other.owner_, nullptr)),
      handle_(libk::exchange(other.handle_, CapHandle{})),
      charge_(libk::move(other.charge_)) {}

auto CSpace::Reservation::operator=(Reservation&& other) noexcept
    -> Reservation& {
    if (this != &other) {
        reset();
        owner_ = libk::exchange(other.owner_, nullptr);
        handle_ = libk::exchange(other.handle_, CapHandle{});
        charge_ = libk::move(other.charge_);
    }
    return *this;
}

CSpace::Reservation::~Reservation() noexcept {
    reset();
}

void CSpace::Reservation::reset() noexcept {
    CSpace* const owner = libk::exchange(owner_, nullptr);
    const CapHandle handle = libk::exchange(handle_, CapHandle{});
    if (owner != nullptr) {
        owner->rollback(handle);
    }
}

CSpace::~CSpace() noexcept {
    KASSERT(!accepting_);
    KASSERT(!growing_);
    KASSERT(!releasing_);
    KASSERT(retired_);
    KASSERT(root_ == nullptr);
    KASSERT(page_count_ == 0);
    KASSERT(live_slots_ == 0);
    KASSERT(bindings_ == 0);
    KASSERT(!pages_);
}

auto CSpace::reserve() noexcept
    -> libk::Expected<Reservation, CSpaceError> {
    kernel::resource::Reservation slot_charge{};
    if (sponsor_ != nullptr) {
        auto charged = sponsor_->reserve(kernel::resource::Budget{.caps = 1});
        if (!charged) {
            return libk::unexpected(CSpaceError::ResourceExhausted);
        }
        slot_charge = libk::move(charged).value();
    }
    for (;;) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (!accepting_) {
                return libk::unexpected(CSpaceError::InvalidState);
            }
            if (live_slots_ >= quota_.slots) {
                return libk::unexpected(CSpaceError::SlotQuota);
            }

            while (free_head_ != invalid_index) {
                const u32 index = free_head_;
                Slot* const available = slot(index);
                KASSERT(available != nullptr);
                KASSERT(available->state == SlotState::Empty);
                free_head_ = available->next;
                available->next = invalid_index;

                if (available->generation == CapHandle::max_generation) {
                    available->state = SlotState::Quarantined;
                    ++quarantined_slots_;
                    continue;
                }

                ++available->generation;
                available->state = SlotState::Reserved;
                ++live_slots_;
                const CapHandle handle = CapHandle::make(
                    index, available->generation);
                KASSERT(handle);
                return libk::expected(Reservation{
                    *this, handle, libk::move(slot_charge)});
            }

            const usize capacity = next_leaf_ * leaf_slots;
            if (quarantined_slots_ != 0
                && capacity == quarantined_slots_
                && (next_leaf_ == max_leaves
                    || page_count_ == quota_.pages)) {
                return libk::unexpected(CSpaceError::GenerationExhausted);
            }
            if (next_leaf_ == max_leaves) {
                return libk::unexpected(CSpaceError::PageQuota);
            }
            if (growing_) {
                return libk::unexpected(CSpaceError::Contended);
            }
            growing_ = true;
        }

        auto expanded = grow();
        if (!expanded) {
            return libk::unexpected(expanded.error());
        }
    }
}

auto CSpace::reserve_grant() noexcept
    -> libk::Expected<kernel::resource::Reservation, CSpaceError> {
    if (sponsor_ == nullptr) {
        return libk::expected(kernel::resource::Reservation{});
    }
    auto charged = sponsor_->reserve(GrantGraph::node_charge());
    if (!charged) {
        return libk::unexpected(CSpaceError::ResourceExhausted);
    }
    return libk::expected(libk::move(charged).value());
}

auto CSpace::reserve_derivation() noexcept
    -> libk::Expected<DerivationReservation, CSpaceError> {
    // User-visible semantic derivation must never create an uncharged Grant.
    // Kernel bootstrap uses the lower-level construction path explicitly.
    if (sponsor_ == nullptr) {
        return libk::unexpected(CSpaceError::ResourceExhausted);
    }
    auto slot = reserve();
    if (!slot) {
        return libk::unexpected(slot.error());
    }
    auto grant = reserve_grant();
    if (!grant) {
        return libk::unexpected(grant.error());
    }
    return libk::expected(DerivationReservation{
        libk::move(slot).value(), libk::move(grant).value()});
}

auto CSpace::insert(
    GrantRef&& grant,
    CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError> {
    auto reserved = reserve();
    if (!reserved) {
        return libk::unexpected(reserved.error());
    }
    return insert(
        libk::move(reserved).value(), libk::move(grant), view);
}

auto CSpace::insert(
    Reservation&& reserved,
    GrantRef&& grant,
    CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError> {
    if (!grant) {
        return libk::unexpected(CSpaceError::GrantUnavailable);
    }
    auto acquired = grant.acquire();
    if (!acquired) {
        return libk::unexpected(grant_error(acquired.error()));
    }
    GrantLease lease = libk::move(acquired).value();
    auto effective = compose(lease.kind(), lease.ceiling(), view);
    if (!effective) {
        return libk::unexpected(policy_error(effective.error()));
    }

    Reservation reservation = libk::move(reserved);
    return commit(reservation, libk::move(grant), view);
}

auto CSpace::close(CapHandle handle) noexcept
    -> libk::Expected<void, CSpaceError> {
    GrantRef released{};
    kernel::resource::Refund refund{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const occupied = handle ? slot(handle.index()) : nullptr;
        if (occupied == nullptr
            || occupied->generation != handle.generation()) {
            return libk::unexpected(CSpaceError::InvalidHandle);
        }
        if (occupied->state != SlotState::Occupied) {
            return libk::unexpected(CSpaceError::InvalidState);
        }

        Capability& capability = occupied->storage.capability;
        unlink_occupied(handle.index(), *occupied);
        released = libk::move(capability.grant);
        libk::destroy_at(&capability);
        refund = occupied->sponsorship.detach();
        occupied->state = SlotState::Empty;
        KASSERT(live_slots_ != 0);
        --live_slots_;
        if (accepting_) {
            push_free(handle.index(), *occupied);
        }
    }
    released.reset();
    refund.complete();
    finish_retire();
    return libk::expected();
}

auto CSpace::duplicate(
    CapHandle source_handle,
    CSpace& destination,
    CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return libk::unexpected(copied.error());
    }
    Snapshot source = libk::move(copied).value();
    GrantLease lease = libk::move(source.lease);
    auto effective = compose(lease.kind(), lease.ceiling(), source.view);
    if (!effective) {
        return libk::unexpected(policy_error(effective.error()));
    }
    if (!effective.value().rights.contains(Right::Duplicate)) {
        return libk::unexpected(CSpaceError::Denied);
    }
    auto destination_authority = compose(
        lease.kind(), effective.value().ceiling(), view);
    if (!destination_authority) {
        return libk::unexpected(policy_error(destination_authority.error()));
    }

    auto cloned = source.graph->ref(source.key);
    if (!cloned) {
        return libk::unexpected(grant_error(cloned.error()));
    }
    auto reserved = destination.reserve();
    if (!reserved) {
        return libk::unexpected(reserved.error());
    }
    Reservation reservation = libk::move(reserved).value();
    GrantRef reference = libk::move(cloned).value();
    return destination.commit(reservation, libk::move(reference), view);
}

auto CSpace::duplicate(
    CapHandle source_handle,
    CSpace& destination,
    Rights rights) noexcept -> libk::Expected<CapHandle, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return libk::unexpected(copied.error());
    }
    Snapshot source = libk::move(copied).value();
    const GrantLease& lease = source.lease;
    auto effective = compose(lease.kind(), lease.ceiling(), source.view);
    if (!effective) {
        return libk::unexpected(policy_error(effective.error()));
    }
    return duplicate(
        source_handle,
        destination,
        CapView{rights, effective.value().data});
}

auto CSpace::delegate(
    CapHandle source_handle,
    CSpace& destination,
    GrantCeiling ceiling,
    CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return libk::unexpected(copied.error());
    }
    Snapshot source = libk::move(copied).value();
    GrantLease lease = libk::move(source.lease);
    auto effective = compose(lease.kind(), lease.ceiling(), source.view);
    if (!effective) {
        return libk::unexpected(policy_error(effective.error()));
    }
    if (!effective.value().rights.contains(Right::Delegate)) {
        return libk::unexpected(CSpaceError::Denied);
    }
    if (!attenuates(lease.kind(), effective.value(), ceiling)) {
        return libk::unexpected(CSpaceError::Amplification);
    }
    auto child_rights = compose(lease.kind(), ceiling, view);
    if (!child_rights) {
        return libk::unexpected(policy_error(child_rights.error()));
    }

    auto target = lease.clone_target();
    if (!target) {
        return libk::unexpected(CSpaceError::GrantUnavailable);
    }
    kernel::resource::Reservation grant_charge{};
    if (sponsor_ != nullptr) {
        auto charged = sponsor_->reserve(source.graph->node_charge());
        if (!charged) {
            return libk::unexpected(CSpaceError::ResourceExhausted);
        }
        grant_charge = libk::move(charged).value();
    }
    auto child = source.graph->derive(
        libk::move(grant_charge),
        lease,
        libk::move(target).value(),
        ceiling);
    if (!child) {
        return libk::unexpected(grant_error(child.error()));
    }
    auto reserved = destination.reserve();
    if (!reserved) {
        return libk::unexpected(reserved.error());
    }
    Reservation reservation = libk::move(reserved).value();
    GrantRef child_ref = libk::move(child).value();
    return destination.commit(reservation, libk::move(child_ref), view);
}

auto CSpace::delegate(
    CapHandle source_handle,
    CSpace& destination,
    Rights rights) noexcept -> libk::Expected<CapHandle, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return libk::unexpected(copied.error());
    }
    Snapshot source = libk::move(copied).value();
    const GrantLease& lease = source.lease;
    auto effective = compose(lease.kind(), lease.ceiling(), source.view);
    if (!effective) {
        return libk::unexpected(policy_error(effective.error()));
    }
    const GrantCeiling ceiling{rights, effective.value().data};
    return delegate(
        source_handle,
        destination,
        ceiling,
        CapView{rights, effective.value().data});
}

auto CSpace::revoke(
    CapHandle source_handle,
    GrantRevoke& completion,
    bool include_source) noexcept -> libk::Expected<void, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return libk::unexpected(copied.error());
    }
    Snapshot source = libk::move(copied).value();
    GrantLease lease = libk::move(source.lease);
    auto effective = compose(lease.kind(), lease.ceiling(), source.view);
    if (!effective) {
        return libk::unexpected(policy_error(effective.error()));
    }
    if (!effective.value().rights.contains(Right::Revoke)) {
        return libk::unexpected(CSpaceError::Denied);
    }
    const GrantKey key = lease.key();
    auto started = include_source
        ? source.graph->invalidate(key, completion)
        : source.graph->revoke_descendants(key, completion);
    return started
        ? libk::Expected<void, CSpaceError>{libk::expected()}
        : libk::Expected<void, CSpaceError>{
              libk::unexpected(grant_error(started.error()))};
}

auto CSpace::revoke(
    CapHandle source_handle,
    Thread& thread,
    CpuRegistry& cpus,
    bool include_source) noexcept
    -> libk::Expected<kernel::operation::State, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return libk::unexpected(copied.error());
    }
    Snapshot source = libk::move(copied).value();
    GrantLease lease = libk::move(source.lease);
    auto effective = compose(lease.kind(), lease.ceiling(), source.view);
    if (!effective) {
        return libk::unexpected(policy_error(effective.error()));
    }
    if (!effective.value().rights.contains(Right::Revoke)) {
        return libk::unexpected(CSpaceError::Denied);
    }

    auto made = source.graph->create_revoke_wait();
    if (!made) {
        return libk::unexpected(grant_error(made.error()));
    }
    GrantRevokeWait* const operation = made.value();
    if (!thread.begin_wait(operation->relation(), cpus)) {
        source.graph->destroy_revoke_wait(*operation);
        return libk::unexpected(CSpaceError::Contended);
    }

    const GrantKey key = lease.key();
    auto started = include_source
        ? source.graph->invalidate(key, operation->completion_)
        : source.graph->revoke_descendants(key, operation->completion_);
    if (!started) {
        thread.cancel_wait();
        return libk::unexpected(grant_error(started.error()));
    }
    return libk::expected(operation->arm()
        ? kernel::operation::State::Waiting
        : kernel::operation::State::Complete);
}

auto CSpace::destroy(CapHandle source_handle) noexcept
    -> libk::Expected<void, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return libk::unexpected(copied.error());
    }
    Snapshot source = libk::move(copied).value();
    GrantLease lease = libk::move(source.lease);
    auto effective = compose(lease.kind(), lease.ceiling(), source.view);
    if (!effective) {
        return libk::unexpected(policy_error(effective.error()));
    }
    if (!effective.value().rights.contains(Right::Destroy)) {
        return libk::unexpected(CSpaceError::Denied);
    }
    auto target = lease.clone_target();
    if (!target) {
        return libk::unexpected(CSpaceError::GrantUnavailable);
    }
    return target.value().retire()
        ? libk::Expected<void, CSpaceError>{libk::expected()}
        : libk::Expected<void, CSpaceError>{
              libk::unexpected(CSpaceError::InvalidState)};
}

auto CSpace::move(
    CapHandle source_handle,
    CSpace& destination) noexcept -> libk::Expected<CapHandle, CSpaceError> {
    auto reserved = destination.reserve();
    if (!reserved) {
        return libk::unexpected(reserved.error());
    }
    Reservation reservation = libk::move(reserved).value();
    const CapHandle destination_handle = reservation.handle();
    kernel::resource::Refund source_refund{};

    kernel::sync::OrderedIrqLockPair locks{lock_, destination.lock_};

    Slot* const source = source_handle ? slot(source_handle.index()) : nullptr;
    Slot* const target = destination.slot(destination_handle.index());
    CSpaceError error = CSpaceError::InvalidHandle;
    bool transferred{};
    if (!accepting_) {
        error = CSpaceError::InvalidState;
    } else if (source == nullptr
        || source->generation != source_handle.generation()) {
        error = CSpaceError::InvalidHandle;
    } else if (source->state != SlotState::Occupied) {
        error = CSpaceError::InvalidState;
    } else if (target == nullptr
        || target->generation != destination_handle.generation()
        || target->state != SlotState::Reserved
        || !destination.accepting_) {
        error = CSpaceError::InvalidState;
    } else {
        libk::construct_at(
            &target->storage.capability,
            libk::move(source->storage.capability));
        libk::destroy_at(&source->storage.capability);
        if (reservation.charge_) {
            target->sponsorship.commit(libk::move(reservation.charge_));
        }
        source_refund = source->sponsorship.detach();
        target->state = SlotState::Occupied;
        destination.link_occupied(destination_handle.index(), *target);
        unlink_occupied(source_handle.index(), *source);
        source->state = SlotState::Empty;
        KASSERT(live_slots_ != 0);
        --live_slots_;
        push_free(source_handle.index(), *source);
        reservation.disarm();
        transferred = true;
    }

    locks.release();
    source_refund.complete();

    return transferred
        ? libk::Expected<CapHandle, CSpaceError>{
              libk::expected(destination_handle)}
        : libk::Expected<CapHandle, CSpaceError>{libk::unexpected(error)};
}

auto CSpace::snapshot(CapHandle handle) noexcept
    -> libk::Expected<Snapshot, CSpaceError> {
    kernel::sync::IrqLockGuard guard{lock_};
    if (!accepting_) {
        return libk::unexpected(CSpaceError::InvalidState);
    }
    Slot* const occupied = handle ? slot(handle.index()) : nullptr;
    if (occupied == nullptr
        || occupied->generation != handle.generation()) {
        return libk::unexpected(CSpaceError::InvalidHandle);
    }
    if (occupied->state != SlotState::Occupied) {
        return libk::unexpected(CSpaceError::InvalidState);
    }
    const Capability& capability = occupied->storage.capability;
    auto acquired = capability.grant.acquire();
    if (!acquired) {
        return libk::unexpected(grant_error(acquired.error()));
    }
    return libk::expected(Snapshot{
        capability.grant.graph(),
        capability.grant.key(),
        capability.view,
        libk::move(acquired).value(),
    });
}

auto CSpace::commit(
    Reservation& reservation,
    GrantRef&& grant,
    CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError> {
    kernel::sync::IrqLockGuard guard{lock_};
    return commit_locked(reservation, libk::move(grant), view);
}

auto CSpace::commit_locked(
    Reservation& reservation,
    GrantRef&& grant,
    CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError> {
    kernel::sync::LockAccess::assert_held(lock_);
    if (reservation.owner_ != this || !grant || !accepting_) {
        return libk::unexpected(CSpaceError::InvalidState);
    }
    const CapHandle handle = reservation.handle_;
    Slot* const target = handle ? slot(handle.index()) : nullptr;
    if (target == nullptr
        || target->generation != handle.generation()
        || target->state != SlotState::Reserved) {
        return libk::unexpected(CSpaceError::InvalidState);
    }
    libk::construct_at(
        &target->storage.capability,
        Capability{libk::move(grant), view});
    if (reservation.charge_) {
        target->sponsorship.commit(libk::move(reservation.charge_));
    }
    target->state = SlotState::Occupied;
    link_occupied(handle.index(), *target);
    reservation.disarm();
    return libk::expected(handle);
}

void CSpace::rollback(CapHandle handle) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const reserved = handle ? slot(handle.index()) : nullptr;
        KASSERT(reserved != nullptr);
        KASSERT(reserved->generation == handle.generation());
        KASSERT(reserved->state == SlotState::Reserved);
        reserved->state = SlotState::Empty;
        KASSERT(live_slots_ != 0);
        --live_slots_;
        if (accepting_) {
            push_free(handle.index(), *reserved);
        }
    }
    finish_retire();
}

auto CSpace::grow() noexcept -> libk::Expected<void, CSpaceError> {
    usize leaf_number{};
    usize high{};
    bool needs_root{};
    bool needs_mid{};
    usize required{};
    bool quota_failed{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(growing_);
        leaf_number = next_leaf_;
        high = leaf_number >> dir_bits;
        needs_root = root_ == nullptr;
        needs_mid = needs_root || root_->children[high] == nullptr;
        required = usize{1} + static_cast<usize>(needs_root)
            + static_cast<usize>(needs_mid);
        if (required > quota_.pages - page_count_) {
            growing_ = false;
            quota_failed = true;
        }
    }
    if (quota_failed) {
        finish_retire();
        return libk::unexpected(CSpaceError::PageQuota);
    }

    kernel::resource::Reservation charges[3]{};
    if (sponsor_ != nullptr) {
        for (usize index = 0; index < required; ++index) {
            auto charged = sponsor_->reserve(kernel::resource::Budget{
                .memory = kernel::mm::page_size});
            if (!charged) {
                {
                    kernel::sync::IrqLockGuard guard{lock_};
                    KASSERT(growing_);
                    growing_ = false;
                }
                finish_retire();
                return libk::unexpected(CSpaceError::ResourceExhausted);
            }
            charges[index] = libk::move(charged).value();
        }
    }

    DirPage* new_root{};
    DirPage* new_mid{};
    LeafPage* new_leaf{};
    bool allocated{true};
    {
        auto extension = pages_.extend();
        kernel::mm::Page root_page{};
        kernel::mm::Page mid_page{};
        kernel::mm::Page leaf_page{};

        if (needs_root) {
            auto page = extension.allocate_page();
            if (page) {
                root_page = page.value();
            } else {
                allocated = false;
            }
        }
        if (allocated && needs_mid) {
            auto page = extension.allocate_page();
            if (page) {
                mid_page = page.value();
            } else {
                allocated = false;
            }
        }
        if (allocated) {
            auto page = extension.allocate_page();
            if (page) {
                leaf_page = page.value();
            } else {
                allocated = false;
            }
        }

        if (allocated) {
            if (needs_root) {
                new_root = libk::construct_at(
                    reinterpret_cast<DirPage*>(extension.bytes(root_page)),
                    root_page,
                    libk::move(charges[0]));
            }
            if (needs_mid) {
                new_mid = libk::construct_at(
                    reinterpret_cast<DirPage*>(extension.bytes(mid_page)),
                    mid_page,
                    libk::move(charges[static_cast<usize>(needs_root)]));
            }
            new_leaf = libk::construct_at(
                reinterpret_cast<LeafPage*>(extension.bytes(leaf_page)),
                static_cast<u32>(leaf_number * leaf_slots),
                leaf_page,
                libk::move(charges[required - 1]));
            extension.commit();
        }
    }

    if (!allocated) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(growing_);
            growing_ = false;
        }
        finish_retire();
        return libk::unexpected(CSpaceError::OutOfMemory);
    }

    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(growing_);
        if (needs_root) {
            KASSERT(root_ == nullptr);
            root_ = new_root;
        }
        auto* mid = static_cast<DirPage*>(root_->children[high]);
        if (needs_mid) {
            KASSERT(mid == nullptr);
            root_->children[high] = new_mid;
            mid = new_mid;
        }
        const usize low = leaf_number & (dir_entries - 1);
        KASSERT(mid->children[low] == nullptr);
        mid->children[low] = new_leaf;
        ++next_leaf_;
        page_count_ += required;
        for (usize offset = leaf_slots; offset > 0; --offset) {
            const usize index = leaf_number * leaf_slots + offset - 1;
            push_free(index, new_leaf->slots[offset - 1]);
        }
        growing_ = false;
    }
    finish_retire();
    return libk::expected();
}

auto CSpace::slot(usize index) noexcept -> Slot* {
    return const_cast<Slot*>(static_cast<const CSpace*>(this)->slot(index));
}

auto CSpace::slot(usize index) const noexcept -> const Slot* {
    if (index > CapHandle::max_index || root_ == nullptr) {
        return nullptr;
    }
    const usize leaf_number = index >> leaf_bits;
    if (leaf_number >= next_leaf_) {
        return nullptr;
    }
    const usize high = leaf_number >> dir_bits;
    const usize low = leaf_number & (dir_entries - 1);
    const auto* const mid = static_cast<const DirPage*>(
        root_->children[high]);
    if (mid == nullptr) {
        return nullptr;
    }
    const auto* const leaf = static_cast<const LeafPage*>(mid->children[low]);
    return leaf != nullptr ? &leaf->slots[index & (leaf_slots - 1)] : nullptr;
}

void CSpace::push_free(usize index, Slot& empty) noexcept {
    KASSERT(index <= CapHandle::max_index);
    KASSERT(empty.state == SlotState::Empty);
    KASSERT(empty.previous == invalid_index);
    empty.next = free_head_;
    free_head_ = static_cast<u32>(index);
}

void CSpace::link_occupied(usize index, Slot& occupied) noexcept {
    KASSERT(index <= CapHandle::max_index);
    KASSERT(occupied.state == SlotState::Occupied);
    KASSERT(occupied.next == invalid_index);
    KASSERT(occupied.previous == invalid_index);
    occupied.next = occupied_head_;
    if (occupied_head_ != invalid_index) {
        Slot* const old_head = slot(occupied_head_);
        KASSERT(old_head != nullptr);
        KASSERT(old_head->state == SlotState::Occupied);
        KASSERT(old_head->previous == invalid_index);
        old_head->previous = static_cast<u32>(index);
    }
    occupied_head_ = static_cast<u32>(index);
}

void CSpace::unlink_occupied(usize index, Slot& occupied) noexcept {
    KASSERT(index <= CapHandle::max_index);
    KASSERT(occupied.state == SlotState::Occupied);
    if (occupied.previous == invalid_index) {
        KASSERT(occupied_head_ == index);
        occupied_head_ = occupied.next;
    } else {
        Slot* const previous = slot(occupied.previous);
        KASSERT(previous != nullptr);
        KASSERT(previous->state == SlotState::Occupied);
        KASSERT(previous->next == index);
        previous->next = occupied.next;
    }
    if (occupied.next != invalid_index) {
        Slot* const next = slot(occupied.next);
        KASSERT(next != nullptr);
        KASSERT(next->state == SlotState::Occupied);
        KASSERT(next->previous == index);
        next->previous = occupied.previous;
    }
    occupied.next = invalid_index;
    occupied.previous = invalid_index;
}

void CSpace::retire() noexcept {
    bool prepare{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        prepare = accepting_;
    }
    if (prepare) {
        KASSERT(prepare_retire());
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (retired_) {
            return;
        }
        KASSERT(!accepting_);
    }

    for (;;) {
        GrantRef released{};
        kernel::resource::Refund refund{};
        bool found{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (occupied_head_ != invalid_index) {
                const usize index = occupied_head_;
                Slot* const current = slot(index);
                KASSERT(current != nullptr);
                KASSERT(current->state == SlotState::Occupied);
                unlink_occupied(index, *current);
                Capability& capability = current->storage.capability;
                released = libk::move(capability.grant);
                libk::destroy_at(&capability);
                refund = current->sponsorship.detach();
                current->state = SlotState::Empty;
                KASSERT(live_slots_ != 0);
                --live_slots_;
                found = true;
            }
        }
        if (!found) {
            break;
        }
        released.reset();
        refund.complete();
    }

    finish_retire();
}

auto CSpace::prepare_retire() noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (!accepting_ || retired_ || releasing_ || bindings_ != 0) {
        return false;
    }
    accepting_ = false;
    return true;
}

auto CSpace::attach_execution() noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (!accepting_ || retired_ || releasing_
        || bindings_ == libk::numeric_limits<usize>::max()) {
        return false;
    }
    ++bindings_;
    return true;
}

void CSpace::detach_execution() noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(bindings_ != 0);
    --bindings_;
}

void CSpace::bind_sponsor(
    kernel::resource::Sponsorship& sponsor) noexcept {
    KASSERT(sponsor_ == nullptr && sponsor);
    sponsor_ = &sponsor;
}

auto CSpace::binding_count() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return bindings_;
}

void CSpace::finish_retire() noexcept {
    DirPage* root{};
    usize leaves{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (accepting_ || growing_ || live_slots_ != 0 || bindings_ != 0
            || releasing_ || retired_) {
            return;
        }
        releasing_ = true;
        root = root_;
        leaves = next_leaf_;
        root_ = nullptr;
        free_head_ = invalid_index;
        occupied_head_ = invalid_index;
        next_leaf_ = 0;
        page_count_ = 0;
        quarantined_slots_ = 0;
    }

    if (root != nullptr) {
        for (usize high = 0; high < dir_entries; ++high) {
            auto* const mid = static_cast<DirPage*>(root->children[high]);
            if (mid == nullptr) {
                continue;
            }
            for (usize low = 0; low < dir_entries; ++low) {
                auto* const leaf = static_cast<LeafPage*>(mid->children[low]);
                if (leaf != nullptr) {
                    KASSERT((leaf->base_index >> leaf_bits) < leaves);
                    auto refund = leaf->sponsorship.detach();
                    const kernel::mm::Page page = leaf->page;
                    libk::destroy_at(leaf);
                    auto backing = pages_.detach(page);
                    KASSERT(backing);
                    backing->reset();
                    refund.complete();
                }
            }
            auto refund = mid->sponsorship.detach();
            const kernel::mm::Page page = mid->page;
            libk::destroy_at(mid);
            auto backing = pages_.detach(page);
            KASSERT(backing);
            backing->reset();
            refund.complete();
        }
        auto refund = root->sponsorship.detach();
        const kernel::mm::Page page = root->page;
        libk::destroy_at(root);
        auto backing = pages_.detach(page);
        KASSERT(backing);
        backing->reset();
        refund.complete();
    }
    pages_.reset();
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(releasing_);
        releasing_ = false;
        retired_ = true;
    }
}

auto CSpace::live_slots() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return live_slots_;
}

auto CSpace::table_pages() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return page_count_;
}

auto CSpace::policy_error(PolicyError error) noexcept -> CSpaceError {
    switch (error) {
    case PolicyError::UnsupportedKind:
        return CSpaceError::WrongKind;
    case PolicyError::InvalidRights:
    case PolicyError::InvalidData:
    case PolicyError::Amplification:
        return CSpaceError::Amplification;
    case PolicyError::Denied:
        return CSpaceError::Denied;
    }
    return CSpaceError::Denied;
}

auto CSpace::grant_error(GrantError error) noexcept -> CSpaceError {
    switch (error) {
    case GrantError::InvalidKey:
    case GrantError::InvalidState:
    case GrantError::RevocationConflict:
        return CSpaceError::GrantUnavailable;
    case GrantError::WrongKind:
        return CSpaceError::WrongKind;
    case GrantError::RightsViolation:
        return CSpaceError::Amplification;
    case GrantError::OutOfMemory:
        return CSpaceError::OutOfMemory;
    case GrantError::QuotaExceeded:
        return CSpaceError::SlotQuota;
    case GrantError::GenerationExhausted:
        return CSpaceError::GenerationExhausted;
    }
    return CSpaceError::GrantUnavailable;
}

} // namespace kernel::cap
#include <cpu/cpu_registry.hpp>
#include <thread/thread.hpp>
