#include <expected>
#include <cap/cspace.hpp>
#include <cap/grant.hpp>

#include <libk/memory.hpp>
#include <utility>
#include <mm/vspace.hpp>
#include <object/ref.hpp>
#include <sync.hpp>
#include <libk/scope_guard.hpp>
#include <type_traits>

namespace cap {

CSpace::CSpace(mm::Pmm& pmm) noexcept
    : CSpace(pmm, Quota{}) {}

CSpace::CSpace(mm::Pmm& pmm, Quota quota) noexcept
    : pmm_(&pmm), pages_(pmm.group()), quota_(quota) {}

CSpace::Reservation::Reservation(Reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      handle_(std::exchange(other.handle_, Handle{})),
      charge_(std::move(other.charge_)) {}

auto CSpace::Reservation::operator=(Reservation&& other) noexcept
    -> Reservation& {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        handle_ = std::exchange(other.handle_, Handle{});
        charge_ = std::move(other.charge_);
    }
    return *this;
}

CSpace::Reservation::~Reservation() noexcept {
    reset();
}

void CSpace::Reservation::reset() noexcept {
    CSpace* const owner = std::exchange(owner_, nullptr);
    const Handle handle = std::exchange(handle_, Handle{});
    if (owner != nullptr) {
        owner->rollback(handle);
    }
}

CSpace::~CSpace() noexcept {
    libk_assert(!accepting_);
    libk_assert(!growing_);
    libk_assert(!releasing_);
    libk_assert(retired_);
    libk_assert(root_ == nullptr);
    libk_assert(page_count_ == 0);
    libk_assert(live_slots_ == 0);
    libk_assert(bindings_ == 0);
    libk_assert(escrows_ == 0);
    libk_assert(!pages_);
    libk_assert(!charge_);
}

auto CSpace::reserve() noexcept
    -> std::expected<Reservation, CSpaceError> {
    resource::Charge slot_charge{};
    if (sponsor_ != nullptr) {
        auto charged = sponsor_->acquire(resource::budget{.caps = 1});
        if (!charged) {
            return std::unexpected(CSpaceError::ResourceExhausted);
        }
        slot_charge = std::move(charged).value();
    }
    for (;;) {
        {
            sync::Lock guard{lock_};
            if (!accepting_) {
                return std::unexpected(CSpaceError::InvalidState);
            }
            if (live_slots_ >= quota_.slots) {
                return std::unexpected(CSpaceError::SlotQuota);
            }

            while (free_head_ != invalid_index) {
                const u32 index = free_head_;
                Slot* const available = slot(index);
                libk_assert(available != nullptr);
                libk_assert(available->state == SlotState::Empty);
                free_head_ = available->next;
                available->next = invalid_index;

                if (available->generation == Handle::max_generation) {
                    available->state = SlotState::Quarantined;
                    ++quarantined_slots_;
                    continue;
                }

                ++available->generation;
                available->state = SlotState::Reserved;
                ++live_slots_;
                const Handle handle = Handle::make(
                    index, available->generation);
                libk_assert(handle);
                return (Reservation{
                    *this, handle, std::move(slot_charge)});
            }

            const usize capacity = next_leaf_ * leaf_slots;
            if (quarantined_slots_ != 0
                && capacity == quarantined_slots_
                && (next_leaf_ == max_leaves
                    || page_count_ == quota_.pages)) {
                return std::unexpected(CSpaceError::GenerationExhausted);
            }
            if (next_leaf_ == max_leaves) {
                return std::unexpected(CSpaceError::PageQuota);
            }
            if (growing_) {
                return std::unexpected(CSpaceError::Contended);
            }
            growing_ = true;
        }

        auto expanded = grow();
        if (!expanded) {
            return std::unexpected(expanded.error());
        }
    }
}

auto CSpace::reserve_grant() noexcept
    -> std::expected<resource::Reservation, CSpaceError> {
    if (sponsor_ == nullptr) {
        return (resource::Reservation{});
    }
    auto charged = sponsor_->reserve(Graph::node_charge());
    if (!charged) {
        return std::unexpected(CSpaceError::ResourceExhausted);
    }
    return (std::move(charged).value());
}

auto CSpace::insert(
    GrantRef&& grant,
    View view) noexcept -> std::expected<Handle, CSpaceError> {
    auto reserved = reserve();
    if (!reserved) {
        return std::unexpected(reserved.error());
    }
    return insert(
        std::move(reserved).value(), std::move(grant), view);
}

auto CSpace::prepare(Reservation&& slot, GrantRef&& grant, View view) noexcept
    -> std::expected<NewCap, CSpaceError> {
    if (!grant) return std::unexpected(CSpaceError::GrantUnavailable);
    auto pin = grant.acquire();
    if (!pin) return std::unexpected(grant_error(pin.error()));
    auto valid = compose(pin->kind(), pin->ceiling(), view);
    if (!valid) return std::unexpected(policy_error(valid.error()));
    return NewCap{std::move(slot), std::move(grant), std::move(*pin), view};
}

auto CSpace::insert(Reservation&& slot, GrantRef&& grant, View view) noexcept
    -> std::expected<Handle, CSpaceError> {
    auto cap = prepare(std::move(slot), std::move(grant), view);
    if (!cap) return std::unexpected(cap.error());
    const Handle handle = cap->handle();
    auto done = insert(std::span{&*cap, 1});
    return done ? std::expected<Handle, CSpaceError>{handle}
        : std::unexpected(done.error());
}

auto CSpace::insert(std::span<NewCap> caps) noexcept
    -> std::expected<void, CSpaceError> {
    sync::Lock guard{lock_};
    if (!accepting_) return std::unexpected(CSpaceError::InvalidState);
    for (auto& cap : caps) {
        if (!cap.grant_ || !cap.pin_ || !reserved(cap.slot_))
            return std::unexpected(CSpaceError::InvalidState);
    }
    for (auto& cap : caps)
        publish(cap.slot_, Capability{std::move(cap.grant_), cap.view_});
    return {};
}

auto CSpace::close(Handle handle) noexcept
    -> std::expected<void, CSpaceError> {
    GrantRef released{};
    resource::Charge refund{};
    {
        sync::Lock guard{lock_};
        Slot* const occupied = handle ? slot(handle.index()) : nullptr;
        if (occupied == nullptr
            || occupied->generation != handle.generation()) {
            return std::unexpected(CSpaceError::InvalidHandle);
        }
        if (occupied->state != SlotState::Occupied) {
            return std::unexpected(CSpaceError::InvalidState);
        }

        Capability& capability = occupied->storage.capability;
        unlink_occupied(handle.index(), *occupied);
        released = std::move(capability.grant);
        libk::destroy_at(&capability);
        if (charge_) refund = charge_.split({.caps = 1});
        occupied->state = SlotState::Empty;
        libk_assert(live_slots_ != 0);
        --live_slots_;
        if (accepting_) {
            push_free(handle.index(), *occupied);
        }
    }
    released.reset();
    refund.reset();
    finish_retire();
    return {};
}

auto CSpace::prepare_xfer(CSpace& dst, const XferSpec& spec) noexcept
    -> std::expected<Xfer, CSpaceError> {
    auto source = snapshot(spec.source);
    if (!source) return std::unexpected(source.error());
    auto& lease = source->lease;
    View view = source->view;
    GrantRef grant;
    if (spec.op == XferOp::Move) {
        const auto* rights = std::get_if<Rights>(&spec.limit);
        if (!rights || !rights->empty()) return std::unexpected(CSpaceError::InvalidDescriptor);
    } else {
        const bool derive = spec.op == XferOp::Derive;
        if (!view.rights.contains(derive ? Right::Delegate : Right::Duplicate))
            return std::unexpected(CSpaceError::Denied);
        auto requested = std::visit([&](const auto& limit) noexcept -> std::expected<View, CSpaceError> {
            using T = std::remove_cvref_t<decltype(limit)>;
            if constexpr (std::is_same_v<T, Rights>) return View{limit, view.data};
            else if constexpr (std::is_same_v<T, View>) return limit;
            else {
                auto v = make_attenuation_ceiling(lease.kind(), view, limit);
                return v ? std::expected<View, CSpaceError>{*v}
                         : std::unexpected(CSpaceError::InvalidDescriptor);
            }
        }, spec.limit);
        if (!requested) return std::unexpected(requested.error());
        auto valid = compose(lease.kind(), view, *requested);
        if (!valid) return std::unexpected(policy_error(valid.error()));
        view = *requested;
        auto made = [&]() noexcept -> std::expected<GrantRef, CSpaceError> {
            if (!derive) {
                auto ref = lease.graph().ref(lease.key());
                return ref ? std::expected<GrantRef, CSpaceError>{std::move(*ref)}
                           : std::unexpected(grant_error(ref.error()));
            }
            auto charge = reserve_grant();
            if (!charge) return std::unexpected(charge.error());
            auto target = lease.clone_target();
            if (!target) return std::unexpected(CSpaceError::GrantUnavailable);
            auto child = lease.graph().derive(std::move(*charge), lease, std::move(*target), view);
            return child ? std::expected<GrantRef, CSpaceError>{std::move(*child)}
                         : std::unexpected(grant_error(child.error()));
        }();
        if (!made) return std::unexpected(made.error());
        grant = std::move(*made);
    }
    auto slot = dst.reserve();
    if (!slot) return std::unexpected(slot.error());
    return Xfer{std::move(*slot), std::move(lease), std::move(grant),
                spec.source, source->view, view, spec.op};
}

auto CSpace::commit_xfers(CSpace& dst, std::span<Xfer> entries) noexcept
    -> std::expected<void, CSpaceError> {
    resource::Charge refund;
    sync::Pair locks{lock_, dst.lock_};
    if (!dst.accepting_) return std::unexpected(CSpaceError::InvalidState);
    for (auto& entry : entries) {
        if (!dst.reserved(entry.slot)) return std::unexpected(CSpaceError::InvalidState);
        if (entry.op != XferOp::Move) continue;
        auto* source = slot(entry.source.index());
        if (!accepting_ || !source || source->generation != entry.source.generation()
            || source->state != SlotState::Occupied
            || source->storage.capability.grant.key() != entry.lease.key()
            || source->storage.capability.view.rights != entry.original.rights
            || source->storage.capability.view.data != entry.original.data)
            return std::unexpected(CSpaceError::InvalidState);
    }
    for (auto& entry : entries) {
        Capability cap;
        if (entry.op == XferOp::Move) {
            auto* source = slot(entry.source.index());
            cap = std::move(source->storage.capability);
            libk::destroy_at(&source->storage.capability);
            unlink_occupied(entry.source.index(), *source);
            if (charge_) refund.merge(charge_.split({.caps = 1}));
            source->state = SlotState::Empty;
            libk_assert(live_slots_ != 0);
            --live_slots_;
            push_free(entry.source.index(), *source);
        } else cap = Capability{std::move(entry.grant), entry.view};
        dst.publish(entry.slot, std::move(cap));
    }
    locks.release();
    refund.reset();
    finish_retire();
    if (&dst != this) dst.finish_retire();
    return {};
}

auto CSpace::transfer(Handle source, CSpace& dst, XferOp op, XferLimit limit) noexcept
    -> std::expected<Handle, CSpaceError> {
    auto entry = prepare_xfer(dst, {source, std::move(limit), op});
    if (!entry) return std::unexpected(entry.error());
    const auto handle = entry->slot.handle();
    auto done = commit_xfers(dst, std::span{&*entry, 1});
    return done ? std::expected<Handle, CSpaceError>{handle} : std::unexpected(done.error());
}

auto CSpace::revoke(
    Handle source_handle,
    GrantRevoke& completion,
    bool include_source) noexcept -> std::expected<void, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return std::unexpected(copied.error());
    }
    Snapshot source = std::move(copied).value();
    GrantLease lease = std::move(source.lease);
    if (!source.view.rights.contains(Right::Revoke)) {
        return std::unexpected(CSpaceError::Denied);
    }
    const GrantKey key = lease.key();
    auto started = include_source
        ? lease.graph().invalidate(key, completion)
        : lease.graph().revoke_descendants(key, completion);
    return started
        ? std::expected<void, CSpaceError>{}
        : std::expected<void, CSpaceError>{
              std::unexpected(grant_error(started.error()))};
}

auto CSpace::destroy(Handle source_handle) noexcept
    -> std::expected<void, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return std::unexpected(copied.error());
    }
    Snapshot source = std::move(copied).value();
    GrantLease lease = std::move(source.lease);
    if (!source.view.rights.contains(Right::Destroy)) {
        return std::unexpected(CSpaceError::Denied);
    }
    if (lease.kind() == object::ObjectKind::VSpace) {
        const auto* limit = std::get_if<VmLimit>(&source.view.data);
        auto target = lease.clone_target();
        if (!target) return std::unexpected(CSpaceError::InvalidHandle);
        auto space = target.value().as<mm::VSpace>();
        // Region-local Destroy cannot retire the containing address space.
        if (!space || limit == nullptr || !space.value()->can_destroy_object(*limit))
            return std::unexpected(CSpaceError::Denied);
    }
    auto destroyed = lease.graph().destroy_target(lease);
    return destroyed ? std::expected<void, CSpaceError>{}
        : std::expected<void, CSpaceError>{std::unexpected(grant_error(destroyed.error()))};
}

auto CSpace::snapshot(Handle handle) noexcept
    -> std::expected<Snapshot, CSpaceError> {
    sync::Lock guard{lock_};
    if (!accepting_) {
        return std::unexpected(CSpaceError::InvalidState);
    }
    Slot* const occupied = handle ? slot(handle.index()) : nullptr;
    if (occupied == nullptr
        || occupied->generation != handle.generation()) {
        return std::unexpected(CSpaceError::InvalidHandle);
    }
    if (occupied->state == SlotState::Empty
        || occupied->state == SlotState::Quarantined) {
        return std::unexpected(CSpaceError::InvalidHandle);
    }
    if (occupied->state != SlotState::Occupied) {
        return std::unexpected(CSpaceError::InvalidState);
    }
    const Capability& capability = occupied->storage.capability;
    auto acquired = capability.grant.acquire();
    if (!acquired) {
        return std::unexpected(grant_error(acquired.error()));
    }
    GrantLease lease = std::move(acquired).value();
    const View original = capability.view;
    guard.unlock();
    auto view = compose(lease.kind(), lease.ceiling(), original);
    if (!view) return std::unexpected(policy_error(view.error()));
    return (Snapshot{std::move(lease), view.value()});
}

auto CSpace::reserved(const Reservation& r) noexcept -> Slot* {
    libk_assert(lock_.held());
    if (r.owner_ != this || !r.handle_) return nullptr;
    auto* target = slot(r.handle_.index());
    return target && target->generation == r.handle_.generation()
        && target->state == SlotState::Reserved ? target : nullptr;
}

void CSpace::publish(Reservation& r, Capability&& cap) noexcept {
    auto* target = reserved(r);
    libk_assert(target && cap.grant);
    libk::construct_at(&target->storage.capability, std::move(cap));
    charge_.merge(std::move(r.charge_));
    target->state = SlotState::Occupied;
    link_occupied(r.handle_.index(), *target);
    r.disarm();
}

void CSpace::rollback(Handle handle) noexcept {
    {
        sync::Lock guard{lock_};
        Slot* const reserved = handle ? slot(handle.index()) : nullptr;
        libk_assert(reserved != nullptr);
        libk_assert(reserved->generation == handle.generation());
        libk_assert(reserved->state == SlotState::Reserved);
        reserved->state = SlotState::Empty;
        libk_assert(live_slots_ != 0);
        --live_slots_;
        if (accepting_) {
            push_free(handle.index(), *reserved);
        }
    }
    finish_retire();
}

auto CSpace::grow() noexcept -> std::expected<void, CSpaceError> {
    usize leaf_number{};
    usize high{};
    bool needs_root{};
    bool needs_mid{};
    usize required{};
    bool quota_failed{};
    {
        sync::Lock guard{lock_};
        libk_assert(growing_);
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
        return std::unexpected(CSpaceError::PageQuota);
    }

    resource::Charge capacity{};
    if (sponsor_ != nullptr) {
        auto charged = sponsor_->acquire({.memory = required * mm::page_size});
        if (!charged) {
            {
                sync::Lock guard{lock_};
                libk_assert(growing_);
                growing_ = false;
            }
            finish_retire();
            return std::unexpected(CSpaceError::ResourceExhausted);
        }
        capacity = std::move(charged).value();
    }

    DirPage* new_root{};
    DirPage* new_mid{};
    LeafPage* new_leaf{};
    bool allocated{true};
    {
        auto pending = pages_.owner().group();
        mm::Page root_page{};
        mm::Page mid_page{};
        mm::Page leaf_page{};

        if (needs_root) {
            auto page = pending.allocate();
            if (page) {
                root_page = page.value();
            } else {
                allocated = false;
            }
        }
        if (allocated && needs_mid) {
            auto page = pending.allocate();
            if (page) {
                mid_page = page.value();
            } else {
                allocated = false;
            }
        }
        if (allocated) {
            auto page = pending.allocate();
            if (page) {
                leaf_page = page.value();
            } else {
                allocated = false;
            }
        }

        if (allocated) {
            if (needs_root) {
                new_root = libk::construct_at(
                    reinterpret_cast<DirPage*>(pending.bytes(root_page)),
                    root_page);
            }
            if (needs_mid) {
                new_mid = libk::construct_at(
                    reinterpret_cast<DirPage*>(pending.bytes(mid_page)),
                    mid_page);
            }
            new_leaf = libk::construct_at(
                reinterpret_cast<LeafPage*>(pending.bytes(leaf_page)),
                leaf_page);
            pages_.append(std::move(pending));
        }
    }

    if (!allocated) {
        {
            sync::Lock guard{lock_};
            libk_assert(growing_);
            growing_ = false;
        }
        finish_retire();
        return std::unexpected(CSpaceError::OutOfMemory);
    }

    {
        sync::Lock guard{lock_};
        libk_assert(growing_);
        if (needs_root) {
            libk_assert(root_ == nullptr);
            root_ = new_root;
        }
        auto* mid = static_cast<DirPage*>(root_->children[high]);
        if (needs_mid) {
            libk_assert(mid == nullptr);
            root_->children[high] = new_mid;
            mid = new_mid;
        }
        const usize low = leaf_number & (dir_entries - 1);
        libk_assert(mid->children[low] == nullptr);
        mid->children[low] = new_leaf;
        ++next_leaf_;
        page_count_ += required;
        charge_.merge(std::move(capacity));
        for (usize offset = leaf_slots; offset > 0; --offset) {
            const usize index = leaf_number * leaf_slots + offset - 1;
            push_free(index, new_leaf->slots[offset - 1]);
        }
        growing_ = false;
    }
    finish_retire();
    return {};
}

auto CSpace::slot(usize index) noexcept -> Slot* {
    return const_cast<Slot*>(static_cast<const CSpace*>(this)->slot(index));
}

auto CSpace::slot(usize index) const noexcept -> const Slot* {
    if (index > Handle::max_index || root_ == nullptr) {
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
    libk_assert(index <= Handle::max_index);
    libk_assert(empty.state == SlotState::Empty);
    libk_assert(empty.previous == invalid_index);
    empty.next = free_head_;
    free_head_ = static_cast<u32>(index);
}

void CSpace::link_occupied(usize index, Slot& occupied) noexcept {
    libk_assert(index <= Handle::max_index);
    libk_assert(occupied.state == SlotState::Occupied);
    libk_assert(occupied.next == invalid_index);
    libk_assert(occupied.previous == invalid_index);
    occupied.next = occupied_head_;
    if (occupied_head_ != invalid_index) {
        Slot* const old_head = slot(occupied_head_);
        libk_assert(old_head != nullptr);
        libk_assert(old_head->state == SlotState::Occupied);
        libk_assert(old_head->previous == invalid_index);
        old_head->previous = static_cast<u32>(index);
    }
    occupied_head_ = static_cast<u32>(index);
}

void CSpace::unlink_occupied(usize index, Slot& occupied) noexcept {
    libk_assert(index <= Handle::max_index);
    libk_assert(occupied.state == SlotState::Occupied);
    if (occupied.previous == invalid_index) {
        libk_assert(occupied_head_ == index);
        occupied_head_ = occupied.next;
    } else {
        Slot* const previous = slot(occupied.previous);
        libk_assert(previous != nullptr);
        libk_assert(previous->state == SlotState::Occupied);
        libk_assert(previous->next == index);
        previous->next = occupied.next;
    }
    if (occupied.next != invalid_index) {
        Slot* const next = slot(occupied.next);
        libk_assert(next != nullptr);
        libk_assert(next->state == SlotState::Occupied);
        libk_assert(next->previous == index);
        next->previous = occupied.previous;
    }
    occupied.next = invalid_index;
    occupied.previous = invalid_index;
}

void CSpace::retire() noexcept {
    bool prepare{};
    {
        sync::Lock guard{lock_};
        prepare = accepting_;
    }
    if (prepare) {
        libk_assert(prepare_retire());
    }
    {
        sync::Lock guard{lock_};
        if (retired_) {
            return;
        }
        libk_assert(!accepting_);
    }

    for (;;) {
        GrantRef released{};
        resource::Charge refund{};
        bool found{};
        {
            sync::Lock guard{lock_};
            if (occupied_head_ != invalid_index) {
                const usize index = occupied_head_;
                Slot* const current = slot(index);
                libk_assert(current != nullptr);
                libk_assert(current->state == SlotState::Occupied);
                unlink_occupied(index, *current);
                Capability& capability = current->storage.capability;
                released = std::move(capability.grant);
                libk::destroy_at(&capability);
                if (charge_) refund = charge_.split({.caps = 1});
                current->state = SlotState::Empty;
                libk_assert(live_slots_ != 0);
                --live_slots_;
                found = true;
            }
        }
        if (!found) {
            break;
        }
        released.reset();
        refund.reset();
    }

    finish_retire();
}

auto CSpace::prepare_retire() noexcept -> bool {
    sync::Lock guard{lock_};
    if (!accepting_ || retired_ || releasing_ || bindings_ != 0
        || escrows_ != 0) {
        return false;
    }
    accepting_ = false;
    return true;
}

auto CSpace::attach_execution() noexcept -> bool {
    sync::Lock guard{lock_};
    if (!accepting_ || retired_ || releasing_
        || bindings_ == std::numeric_limits<usize>::max()) {
        return false;
    }
    ++bindings_;
    return true;
}

void CSpace::detach_execution() noexcept {
    sync::Lock guard{lock_};
    libk_assert(bindings_ != 0);
    --bindings_;
}

auto CSpace::escrow_move(
    Handle source_handle,
    GrantRef& grant,
    View& view,
    Reservation& reservation) noexcept
    -> std::expected<void, CSpaceError> {
    sync::Lock guard{lock_};
    Slot* const source = source_handle
        ? slot(source_handle.index()) : nullptr;
    if (!accepting_) {
        return std::unexpected(CSpaceError::InvalidState);
    }
    if (source == nullptr || source->generation != source_handle.generation()) {
        return std::unexpected(CSpaceError::InvalidHandle);
    }
    if (source->state != SlotState::Occupied) {
        return std::unexpected(CSpaceError::InvalidState);
    }
    unlink_occupied(source_handle.index(), *source);
    Capability capability = std::move(source->storage.capability);
    libk::destroy_at(&source->storage.capability);
    grant = std::move(capability.grant);
    view = capability.view;
    source->state = SlotState::Reserved;
    reservation = Reservation{*this, source_handle, {}};
    retain_escrow();
    return {};
}

auto CSpace::escrow_restore(
    Reservation& reservation,
    GrantRef&& grant,
    View view) noexcept -> bool {
    sync::Lock guard{lock_};
    if (reservation.owner_ != this || !grant) {
        return false;
    }
    const Handle handle = reservation.handle_;
    Slot* const target = handle ? slot(handle.index()) : nullptr;
    if (target == nullptr || target->generation != handle.generation()
        || target->state != SlotState::Reserved) {
        return false;
    }
    libk::construct_at(
        &target->storage.capability, Capability{std::move(grant), view});
    target->state = SlotState::Occupied;
    link_occupied(handle.index(), *target);
    reservation.disarm();
    release_escrow();
    return true;
}

auto CSpace::escrow_drop(
    Reservation& reservation) noexcept -> resource::Charge {
    resource::Charge refund{};
    {
        sync::Lock guard{lock_};
        libk_assert(reservation.owner_ == this);
        const Handle handle = reservation.handle_;
        Slot* const target = handle ? slot(handle.index()) : nullptr;
        libk_assert(target != nullptr
            && target->generation == handle.generation()
            && target->state == SlotState::Reserved);
        if (charge_) refund = charge_.split({.caps = 1});
        target->state = SlotState::Empty;
        libk_assert(live_slots_ != 0);
        --live_slots_;
        if (accepting_) {
            push_free(handle.index(), *target);
        }
        reservation.disarm();
        release_escrow();
    }
    finish_retire();
    return refund;
}

void CSpace::retain_escrow() noexcept {
    libk_assert(escrows_ != std::numeric_limits<usize>::max());
    ++escrows_;
}

void CSpace::release_escrow() noexcept {
    libk_assert(escrows_ != 0);
    --escrows_;
}

void CSpace::bind_sponsor(
    resource::Sponsorship& sponsor) noexcept {
    libk_assert(sponsor_ == nullptr && sponsor);
    sponsor_ = &sponsor;
}

auto CSpace::binding_count() const noexcept -> usize {
    sync::Lock guard{lock_};
    return bindings_;
}

void CSpace::finish_retire() noexcept {
    DirPage* root{};
    resource::Charge refund{};
    {
        sync::Lock guard{lock_};
        if (accepting_ || growing_ || live_slots_ != 0 || bindings_ != 0
            || escrows_ != 0
            || releasing_ || retired_) {
            return;
        }
        releasing_ = true;
        root = root_;
        libk_assert(charge_.amount().caps == 0);
        refund = std::move(charge_);
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
                    const mm::Page page = leaf->page;
                    libk::destroy_at(leaf);
                    auto backing = pages_.detach(page);
                    libk_assert(backing);
                    backing->reset();
                }
            }
            const mm::Page page = mid->page;
            libk::destroy_at(mid);
            auto backing = pages_.detach(page);
            libk_assert(backing);
            backing->reset();
        }
        const mm::Page page = root->page;
        libk::destroy_at(root);
        auto backing = pages_.detach(page);
        libk_assert(backing);
        backing->reset();
    }
    pages_.reset();
    refund.reset();
    {
        sync::Lock guard{lock_};
        libk_assert(releasing_);
        releasing_ = false;
        retired_ = true;
    }
}

auto CSpace::live_slots() const noexcept -> usize {
    sync::Lock guard{lock_};
    return live_slots_;
}

auto CSpace::table_pages() const noexcept -> usize {
    sync::Lock guard{lock_};
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

auto Batch::prepare(Batch& batch, CSpace& src, CSpace& dst, const Specs& specs) noexcept
    -> std::expected<void, CSpaceError> {
    if (batch.source_ || !batch.empty()) return std::unexpected(CSpaceError::InvalidState);
    batch.source_ = &src;
    batch.destination_ = &dst;
    bool done{};
    libk::scope_exit rollback{[&]() noexcept { if (!done) batch.abort(); }};
    for (usize i = 0; i < specs.size(); ++i) {
        if (specs[i].op == XferOp::Move) {
            for (usize j = 0; j < i; ++j)
                if (specs[j].op == XferOp::Move && specs[j].source == specs[i].source)
                    return std::unexpected(CSpaceError::InvalidHandle);
        }
        auto entry = src.prepare_xfer(dst, specs[i]);
        if (!entry) return std::unexpected(entry.error());
        libk_assert(batch.entries_.try_push_back(std::move(*entry)));
    }
    done = true;
    return {};
}

auto Batch::handles() const noexcept -> Handles {
    Handles result;
    for (auto& entry : entries_) libk_assert(result.try_push_back(entry.slot.handle()));
    return result;
}

auto Batch::commit() noexcept -> std::expected<Handles, CSpaceError> {
    if (!source_ || !destination_) return std::unexpected(CSpaceError::InvalidState);
    auto result = handles();
    auto done = source_->commit_xfers(*destination_, {entries_.data(), entries_.size()});
    if (!done) return std::unexpected(done.error());
    abort();
    return result;
}

} // namespace cap
