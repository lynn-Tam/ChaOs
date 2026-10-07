#include <expected>
#include <ipc/transfer.hpp>
#include <cap/graph.hpp>
#include <sync.hpp>

#include <cpu.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <libk/memory.hpp>
#include <libk/scope_guard.hpp>
#include <utility>

namespace ipc {

auto Transfer::prepare(
    Transfer& transfer,
    cap::CSpace& source,
    cap::CSpace& destination,
    const Specs& specs) noexcept
    -> std::expected<void, cap::CSpaceError> {
    if (transfer.source_ != nullptr || !transfer.entries_.empty()) {
        return std::unexpected(cap::CSpaceError::InvalidState);
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
            return std::unexpected(cap::CSpaceError::InvalidHandle);
        }
        if (spec.kind == TransferKind::Move) {
            for (usize previous = 0; previous < index; ++previous) {
                if (specs[previous].kind == TransferKind::Move
                    && specs[previous].source == spec.source) {
                    return std::unexpected(cap::CSpaceError::InvalidHandle);
                }
            }
        }

        auto reserved = destination.reserve();
        auto snapshot = source.snapshot(spec.source);
        if (!reserved || !snapshot) {
            return std::unexpected(
                !reserved ? reserved.error() : snapshot.error());
        }
        cap::CSpace::Snapshot source_snapshot =
            std::move(snapshot).value();
        cap::GrantLease lease = std::move(source_snapshot.lease);

        cap::GrantRef prepared{};
        cap::View destination_view{};
        switch (spec.kind) {
        case TransferKind::Copy: {
            if (!source_snapshot.view.rights.contains(cap::Right::Duplicate)) {
                return std::unexpected(cap::CSpaceError::Denied);
            }
            destination_view = cap::View{
                spec.rights, source_snapshot.view.data};
            auto valid = cap::compose(
                lease.kind(), source_snapshot.view, destination_view);
            auto cloned = lease.graph().ref(lease.key());
            if (!valid || !cloned) {
                return std::unexpected(!valid
                    ? cap::CSpace::policy_error(valid.error())
                    : cap::CSpace::grant_error(cloned.error()));
            }
            prepared = std::move(cloned).value();
            break;
        }
        case TransferKind::Move:
            destination_view = source_snapshot.view;
            break;
        case TransferKind::Delegate: {
            if (!source_snapshot.view.rights.contains(cap::Right::Delegate)) {
                return std::unexpected(cap::CSpaceError::Denied);
            }
            const cap::View ceiling{
                spec.rights, source_snapshot.view.data};
            destination_view = cap::View{
                spec.rights, source_snapshot.view.data};
            if (!cap::attenuates(lease.kind(), source_snapshot.view, ceiling)) {
                return std::unexpected(cap::CSpaceError::Amplification);
            }
            auto valid = cap::compose(
                lease.kind(), ceiling, destination_view);
            auto charge = source.reserve_grant();
            auto target = lease.clone_target();
            if (!valid || !charge || !target) {
                return std::unexpected(!valid
                    ? cap::CSpace::policy_error(valid.error())
                    : !charge ? charge.error()
                    : cap::CSpaceError::GrantUnavailable);
            }
            auto child = lease.graph().derive(
                std::move(charge).value(),
                lease,
                std::move(target).value(),
                ceiling);
            if (!child) {
                return std::unexpected(
                    cap::CSpace::grant_error(child.error()));
            }
            prepared = std::move(child).value();
            break;
        }
        }

        const auto key = lease.key();
        libk_assert(transfer.entries_.try_emplace_back(
            std::move(reserved).value(),
            std::move(lease),
            std::move(prepared),
            spec.source,
            key,
            source_snapshot.view,
            destination_view,
            spec.kind));
    }
    prepared = true;
    return {};
}

void Transfer::reset() noexcept {
    entries_.clear();
    source_ = nullptr;
    destination_ = nullptr;
}

auto Transfer::handles() const noexcept -> Handles {
    Handles result{};
    for (const Entry& entry : entries_) {
        libk_assert(result.try_push_back(entry.slot.handle()));
    }
    return result;
}

auto Transfer::commit() noexcept
    -> std::expected<Handles, TransferError> {
    if (source_ == nullptr || destination_ == nullptr) {
        return std::unexpected(TransferError::InvalidSpec);
    }

    sync::Pair locks{
        source_->lock_, destination_->lock_};

    bool valid = true;
    for (Entry& entry : entries_) {
        valid = valid && destination_->reserved(entry.slot) != nullptr;
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
    resource::Charge refund{};
    if (valid) {
        for (Entry& entry : entries_) {
            cap::CSpace::Reservation& reservation = entry.slot;
            const cap::Handle handle = reservation.handle();
            cap::CSpace::Capability capability{};
            if (entry.kind == TransferKind::Move) {
                cap::CSpace::Slot* const source =
                    source_->slot(entry.source.index());
                capability = std::move(source->storage.capability);
                libk::destroy_at(&source->storage.capability);
                source_->unlink_occupied(entry.source.index(), *source);
                if (source_->charge_) {
                    refund.merge(source_->charge_.split({.caps = 1}));
                }
                source->state = cap::CSpace::SlotState::Empty;
                libk_assert(source_->live_slots_ != 0);
                --source_->live_slots_;
                if (source_->accepting_) {
                    source_->push_free(entry.source.index(), *source);
                }
            } else {
                capability = cap::CSpace::Capability{
                    std::move(entry.prepared), entry.view};
            }
            destination_->publish(reservation, std::move(capability));
            libk_assert(handles.try_push_back(handle));
        }
    }

    locks.release();

    refund.reset();
    if (!valid) {
        return std::unexpected(TransferError::SourceChanged);
    }
    source_->finish_retire();
    if (destination_ != source_) {
        destination_->finish_retire();
    }
    reset();
    return handles;
}

} // namespace ipc
