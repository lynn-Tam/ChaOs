#include <ipc/transfer.hpp>

#include <arch/interrupt.hpp>
#include <core/debug.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/memory.hpp>
#include <libk/scope_guard.hpp>
#include <libk/utility.hpp>

namespace kernel::ipc {

auto Transfer::prepare(
    Transfer& transfer,
    cap::CSpace& source,
    cap::CSpace& destination,
    const Specs& specs) noexcept
    -> libk::Expected<void, cap::CSpaceError> {
    if (transfer.source_ != nullptr || !transfer.entries_.empty()) {
        return libk::unexpected(cap::CSpaceError::InvalidState);
    }
    transfer.source_ = &source;
    transfer.destination_ = &destination;
    bool prepared{};
    auto rollback = libk::on_scope_exit([&]() noexcept {
        if (!prepared) {
            transfer.reset();
        }
    });

    for (usize index = 0; index < specs.size(); ++index) {
        const TransferSpec& spec = specs[index];
        if (!spec.source || (spec.kind == TransferKind::Move
                && !spec.rights.empty())) {
            return libk::unexpected(cap::CSpaceError::InvalidHandle);
        }
        if (spec.kind == TransferKind::Move) {
            for (usize previous = 0; previous < index; ++previous) {
                if (specs[previous].kind == TransferKind::Move
                    && specs[previous].source == spec.source) {
                    return libk::unexpected(cap::CSpaceError::InvalidHandle);
                }
            }
        }

        auto reserved = destination.reserve();
        auto snapshot = source.snapshot(spec.source);
        if (!reserved || !snapshot) {
            return libk::unexpected(
                !reserved ? reserved.error() : snapshot.error());
        }
        cap::CSpace::Snapshot source_snapshot =
            libk::move(snapshot).value();
        cap::GrantLease lease = libk::move(source_snapshot.lease);
        auto effective = cap::compose(
            lease.kind(), lease.ceiling(), source_snapshot.view);
        if (!effective) {
            return libk::unexpected(
                cap::CSpace::policy_error(effective.error()));
        }

        cap::GrantRef prepared{};
        cap::CapView destination_view{};
        switch (spec.kind) {
        case TransferKind::Copy: {
            if (!effective.value().rights.contains(cap::Right::Duplicate)) {
                return libk::unexpected(cap::CSpaceError::Denied);
            }
            destination_view = cap::CapView{
                spec.rights, effective.value().data};
            auto valid = cap::compose(
                lease.kind(), effective.value().ceiling(), destination_view);
            auto cloned = source_snapshot.graph->ref(source_snapshot.key);
            if (!valid || !cloned) {
                return libk::unexpected(!valid
                    ? cap::CSpace::policy_error(valid.error())
                    : cap::CSpace::grant_error(cloned.error()));
            }
            prepared = libk::move(cloned).value();
            break;
        }
        case TransferKind::Move:
            destination_view = source_snapshot.view;
            break;
        case TransferKind::Delegate: {
            if (!effective.value().rights.contains(cap::Right::Delegate)) {
                return libk::unexpected(cap::CSpaceError::Denied);
            }
            const cap::GrantCeiling ceiling{
                spec.rights, effective.value().data};
            destination_view = cap::CapView{
                spec.rights, effective.value().data};
            if (!cap::attenuates(lease.kind(), effective.value(), ceiling)) {
                return libk::unexpected(cap::CSpaceError::Amplification);
            }
            auto valid = cap::compose(
                lease.kind(), ceiling, destination_view);
            auto charge = source.reserve_grant();
            auto target = lease.clone_target();
            if (!valid || !charge || !target) {
                return libk::unexpected(!valid
                    ? cap::CSpace::policy_error(valid.error())
                    : !charge ? charge.error()
                    : cap::CSpaceError::GrantUnavailable);
            }
            auto child = source_snapshot.graph->derive(
                libk::move(charge).value(),
                lease,
                libk::move(target).value(),
                ceiling);
            if (!child) {
                return libk::unexpected(
                    cap::CSpace::grant_error(child.error()));
            }
            prepared = libk::move(child).value();
            break;
        }
        }

        KASSERT(transfer.entries_.try_emplace_back(
            libk::move(reserved).value(),
            libk::move(lease),
            libk::move(prepared),
            spec.source,
            source_snapshot.key,
            source_snapshot.view,
            destination_view,
            spec.kind));
    }
    prepared = true;
    return libk::expected();
}

void Transfer::reset() noexcept {
    entries_.clear();
    source_ = nullptr;
    destination_ = nullptr;
}

auto Transfer::handles() const noexcept -> Handles {
    Handles result{};
    for (const Entry& entry : entries_) {
        KASSERT(result.try_push_back(entry.slot.handle()));
    }
    return result;
}

auto Transfer::commit() noexcept
    -> libk::Expected<Handles, TransferError> {
    if (source_ == nullptr || destination_ == nullptr) {
        return libk::unexpected(TransferError::InvalidSpec);
    }

    cap::CSpace* first = source_;
    cap::CSpace* second = destination_;
    if (reinterpret_cast<usize>(first) > reinterpret_cast<usize>(second)) {
        libk::swap(first, second);
    }

    const arch::InterruptState interrupts = arch::disable_interrupts();
    first->lock_.lock();
    if (second != first) {
        second->lock_.lock();
    }

    bool valid = true;
    for (Entry& entry : entries_) {
        const cap::CapHandle destination = entry.slot.handle();
        cap::CSpace::Slot* const target =
            destination_->slot(destination.index());
        valid = valid && target != nullptr
            && target->generation == destination.generation()
            && target->state == cap::CSpace::SlotState::Reserved;
        if (!valid || entry.kind != TransferKind::Move) {
            continue;
        }
        cap::CSpace::Slot* const source =
            source_->slot(entry.source.index());
        valid = source != nullptr
            && source->generation == entry.source.generation()
            && source->state == cap::CSpace::SlotState::Occupied
            && source->storage.capability.grant.key() == entry.key
            && source->storage.capability.view.rights == entry.original.rights
            && source->storage.capability.view.data == entry.original.data;
    }

    Handles handles{};
    libk::ManualLifetime<kernel::resource::Refund>
        refunds[MYOS_IPC_MAX_CAPS]{};
    usize refund_count{};
    if (valid) {
        for (Entry& entry : entries_) {
            cap::CSpace::Reservation& reservation = entry.slot;
            const cap::CapHandle handle = reservation.handle();
            cap::CSpace::Slot* const target =
                destination_->slot(handle.index());
            cap::CSpace::Capability capability{};
            if (entry.kind == TransferKind::Move) {
                cap::CSpace::Slot* const source =
                    source_->slot(entry.source.index());
                capability = libk::move(source->storage.capability);
                libk::destroy_at(&source->storage.capability);
                source_->unlink_occupied(entry.source.index(), *source);
                auto& refund = refunds[refund_count++].emplace(
                    source->sponsorship.detach());
                (void)refund;
                source->state = cap::CSpace::SlotState::Empty;
                KASSERT(source_->live_slots_ != 0);
                --source_->live_slots_;
                if (source_->accepting_) {
                    source_->push_free(entry.source.index(), *source);
                }
            } else {
                capability = cap::CSpace::Capability{
                    libk::move(entry.prepared), entry.view};
            }
            libk::construct_at(
                &target->storage.capability, libk::move(capability));
            if (reservation.charge_) {
                target->sponsorship.commit(libk::move(reservation.charge_));
            }
            target->state = cap::CSpace::SlotState::Occupied;
            destination_->link_occupied(handle.index(), *target);
            reservation.disarm();
            KASSERT(handles.try_push_back(handle));
        }
    }

    if (second != first) {
        second->lock_.unlock();
    }
    first->lock_.unlock();
    arch::restore_interrupts(interrupts);

    for (usize index = 0; index < refund_count; ++index) {
        refunds[index]->complete();
        refunds[index].reset();
    }
    if (!valid) {
        return libk::unexpected(TransferError::SourceChanged);
    }
    source_->finish_retire();
    if (destination_ != source_) {
        destination_->finish_retire();
    }
    reset();
    return libk::expected(libk::move(handles));
}

} // namespace kernel::ipc
