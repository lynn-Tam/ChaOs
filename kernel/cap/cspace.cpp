#include <expected>
#include <cap/cspace.hpp>
#include <cap/grant.hpp>

#include <libk/memory.hpp>
#include <utility>
#include <mm/vspace.hpp>
#include <object/pool.hpp>
#include <sync.hpp>
#include <libk/scope_guard.hpp>
#include <type_traits>

namespace cap {

auto CSpace::inspect(Handle handle) noexcept -> std::expected<CapInfo, CSpaceError> {
    auto* cell = handle ? slot(handle.index()) : nullptr;
    if (!cell) return std::unexpected(CSpaceError::InvalidHandle);
    sync::Lock guard{cell->lock};
    if (!accepting_ || cell->generation != handle.generation() || cell->state != SlotState::Occupied)
        return std::unexpected(CSpaceError::InvalidHandle);
    const auto& cap = cell->storage.capability;
    CapInfo info{.selector = handle.raw(), .object_id = cap.grant.target_id().value,
        .grant_id = cap.grant.id().value, .state = CAP_INFO_UNAVAILABLE, .reserved = {}, .view = {}};
    if (const auto view = cap.grant.graph().view(cap.grant.key(), cap.view)) {
        info.view = *view; info.state = CAP_INFO_VALID;
    }
    return info;
}

auto CSpace::inspect(Handle cursor, CSpaceInfo& info, u64 epoch) noexcept -> std::expected<void, CSpaceError> {
    sync::Lock guard{lock_};
    if (!accepting_ || reassigning_) return std::unexpected(CSpaceError::InvalidState);
    if (epoch_ == std::numeric_limits<u64>::max() || (cursor && !epoch) || (epoch && epoch != epoch_))
        return std::unexpected(CSpaceError::InvalidHandle);
    auto index = occupied_head_;
    if (cursor) {
        auto* previous = slot(cursor.index());
        if (!previous || previous->generation != cursor.generation() || previous->state != SlotState::Occupied)
            return std::unexpected(CSpaceError::InvalidHandle);
        index = previous->next;
    }
    info = {};
    info.version = CSPACE_INFO_VERSION; info.size = sizeof(info);
    info.slots = live_slots_; info.pages = page_count_; info.epoch = epoch_;
    while (index != invalid_index && info.count < CAP_INFO_PAGE) {
        auto& row = *slot(index);
        const auto& cap = row.storage.capability;
        auto& record = info.records[info.count++];
        record.selector = Handle::make(index, row.generation).raw();
        record.object_id = cap.grant.target_id().value;
        record.grant_id = cap.grant.id().value;
        record.state = CAP_INFO_UNAVAILABLE;
        if (const auto view = cap.grant.graph().view(cap.grant.key(), cap.view)) {
            record.view = *view;
            record.state = CAP_INFO_VALID;
        }
        index = row.next;
    }
    if (index != invalid_index) info.cursor = info.records[info.count - 1].selector;
    return {};
}

auto CSpace::inspect(GrantQuery query, usize depth, GrantRecords& out) noexcept -> status_t {
    sync::Lock guard{lock_};
    auto handle = Handle{query.selector};
    auto* cell = slot(handle.index());
    if (!handle || !cell || cell->generation != handle.generation() || cell->state != SlotState::Occupied)
        return STATUS_STALE;
    auto& grant = cell->storage.capability.grant;
    return grant.graph().inspect(grant.key(), query, depth, out);
}

auto CSpace::observe(Handle seed, GrantId selected, usize depth) noexcept -> std::expected<Graph::Sample, status_t> {
    sync::Lock guard{lock_};
    auto* cell = seed ? slot(seed.index()) : nullptr;
    if (!cell || cell->generation != seed.generation() || cell->state != SlotState::Occupied)
        return std::unexpected(STATUS_INVALID_CAP);
    auto& grant = cell->storage.capability.grant;
    auto sample = grant.graph().observe(grant.key(), selected, depth);
    return sample ? std::expected<Graph::Sample, status_t>{std::move(*sample)} : std::unexpected(STATUS_NOT_FOUND);
}

CSpace::CSpace(mm::Pmm& pmm) noexcept
    : CSpace(pmm, Quota{}) {}

CSpace::CSpace(mm::Pmm& pmm, Quota quota) noexcept
    : pmm_(&pmm), pages_(pmm.group()), quota_(quota) {}

CSpace::Reservation::Reservation(Reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      handle_(std::exchange(other.handle_, Handle{})),
      usage_(std::move(other.usage_)) {}

auto CSpace::Reservation::operator=(Reservation&& other) noexcept
    -> Reservation& {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        handle_ = std::exchange(other.handle_, Handle{});
        usage_ = std::move(other.usage_);
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
    libk_assert(!usage_);
}

auto CSpace::reserve() noexcept
    -> std::expected<Reservation, CSpaceError> {
    {
        sync::Lock guard{lock_};
        if (!accepting_ || reassigning_) return std::unexpected(CSpaceError::InvalidState);
        ++preparing_;
    }
    libk::scope_exit prepared{[&]() noexcept {
        { sync::Lock guard{lock_}; libk_assert(preparing_); --preparing_; }
        finish_retire();
    }};
    resource::Usage slot_usage{};
    if (allocation_ != nullptr) {
        auto charged = allocation_->acquire(resource::Capacity{.caps = 1});
        if (!charged) {
            return std::unexpected(CSpaceError::ResourceExhausted);
        }
        slot_usage = std::move(charged).value();
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
                sync::Lock local{available->lock};
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
                    *this, handle, std::move(slot_usage)});
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
    if (allocation_ == nullptr) {
        return (resource::Reservation{});
    }
    auto charged = allocation_->reserve(Graph::node_usage());
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
    return NewCap{std::move(slot), Capability{std::move(grant), view}};
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

auto CSpace::insert(std::span<NewCap> caps, Graph::Admission admission,
    std::span<const object::ref<>*> gates, libk::delegate<void() noexcept> commit,
    std::span<const Graph::Attach> relations) noexcept -> std::expected<void, CSpaceError> {
    if (caps.size() > IPC_MAX_CAPS) return std::unexpected(CSpaceError::InvalidDescriptor);
    std::array<Capability*, IPC_MAX_CAPS> values;
    std::array<Reservation*, IPC_MAX_CAPS> slots;
    for (usize i = 0; i < caps.size(); ++i) { values[i] = &caps[i].cap_; slots[i] = &caps[i].slot_; }
    return install(std::span{values}.first(caps.size()), std::span{slots}.first(caps.size()), admission, gates, commit, relations);
}

auto CSpace::install(std::span<Capability*> caps, std::span<Reservation*> reservations, Graph::Admission admission,
    std::span<const object::ref<>*> gates, libk::delegate<void() noexcept> commit,
    std::span<const Graph::Attach> relations) noexcept -> std::expected<void, CSpaceError> {
    libk::scope_exit notify{[&]() noexcept { post(); }};
    if (caps.size() > IPC_MAX_CAPS || gates.size() > IPC_MAX_CAPS || admission.grants_.size() > IPC_MAX_CAPS
        || relations.size() > Graph::max_attachments) return std::unexpected(CSpaceError::InvalidDescriptor);
    std::array<object::Link::Prepared, Graph::max_attachments> prepared;
    for (usize i = 0; i < relations.size(); ++i) {
        const auto [grant, attachment, target] = relations[i];
        if (!attachment || !attachment->ops_ || bool(grant) == bool(target)
            || (grant && !*grant) || (target && !*target)) return std::unexpected(CSpaceError::InvalidState);
        auto made = attachment->link_.prepare(grant ? grant->target() : *target, attachment->ops_->observe);
        if (!made) return std::unexpected(CSpaceError::GrantUnavailable);
        prepared[i] = std::move(*made);
    }
    auto* graph = caps.empty() ? admission.graph_ : &caps.front()->grant.graph();
    for (const auto& relation : relations)
        if (relation.grant && (!graph || &relation.grant->graph() != graph))
            return std::unexpected(CSpaceError::InvalidState);
    if (admission.graph_ && admission.graph_ != graph) return std::unexpected(CSpaceError::InvalidState);
    std::array<const object::ref<>*, IPC_MAX_CAPS * 3> targets;
    libk::inplace_vector<const GrantPin*, IPC_MAX_CAPS * 2> grants;
    usize count{};
    auto add = [&](const GrantPin& pin) noexcept -> bool {
        if (&pin.graph() != graph) return false;
        targets[count++] = &pin.target();
        return grants.try_push_back(&pin);
    };
    for (auto* pin : admission.grants_) if (!pin || !*pin || !add(*pin))
        return std::unexpected(CSpaceError::GrantUnavailable);
    std::array<GrantPin, IPC_MAX_CAPS> pins;
    for (usize i = 0; i < caps.size(); ++i) {
        auto acquired = caps[i]->grant.acquire();
        if (!acquired || !compose(acquired->kind(), acquired->ceiling(), caps[i]->view))
            return std::unexpected(CSpaceError::GrantUnavailable);
        pins[i] = std::move(*acquired);
        if (!add(pins[i])) return std::unexpected(CSpaceError::GrantUnavailable);
    }
    for (auto* gate : gates) targets[count++] = gate;
    sync::Lock guard{lock_};
    if (!accepting_ || reassigning_ || reservations.size() != caps.size()) return std::unexpected(CSpaceError::InvalidState);
    std::array<sync::Spin*, IPC_MAX_CAPS> slots;
    for (usize i = 0; i < caps.size(); ++i) {
        auto* target = reserved(*reservations[i]);
        if (!target) return std::unexpected(CSpaceError::InvalidState);
        slots[i] = &target->lock;
    }
    sync::Set local{std::span{slots}.first(caps.size())};
    auto publish_all = [&] noexcept {
        for (usize i = 0; i < caps.size(); ++i) publish(*reservations[i], std::move(*caps[i]));
        if (commit) commit();
    };
    const auto participants = std::span{targets}.first(count);
    // An accepted operation installs its execution relation with its result
    // selectors and provider index. No hardware or foreign callbacks here.
    const bool done = !relations.empty()
        ? bool(graph && graph->attach_commit(relations, std::span{prepared}.first(relations.size()), participants,
            libk::delegate<void() noexcept>::bind(publish_all), grants.span()))
        : graph ? graph->admission(grants.span()).publish(participants, publish_all) == STATUS_OK
                : object::ref<>::publish(participants, publish_all);
    if (done) for (auto* slot : reservations) usage_.merge(std::move(slot->usage_));
    return done ? std::expected<void, CSpaceError>{} : std::unexpected(CSpaceError::GrantUnavailable);
}

auto CSpace::close(Handle handle) noexcept
    -> std::expected<void, CSpaceError> {
    libk::scope_exit notify{[&]() noexcept { post(); }};
    Capability released{};
    resource::Usage release{};
    {
        sync::Lock guard{lock_};
        if (reassigning_) return std::unexpected(CSpaceError::Contended);
        Slot* const occupied = handle ? slot(handle.index()) : nullptr;
        if (occupied == nullptr
            || occupied->generation != handle.generation()) {
            return std::unexpected(CSpaceError::InvalidHandle);
        }
        if (occupied->state != SlotState::Occupied) {
            return std::unexpected(CSpaceError::InvalidState);
        }

        sync::Lock local{occupied->lock};
        Capability& capability = occupied->storage.capability;
        unlink_occupied(handle.index(), *occupied);
        released = std::move(capability);
        libk::destroy_at(&capability);
        if (usage_) release = usage_.split({.caps = 1});
        occupied->state = SlotState::Empty;
        libk_assert(live_slots_ != 0);
        --live_slots_;
        if (accepting_) {
            push_free(handle.index(), *occupied);
        }
    }
    released = {};
    release.reset();
    finish_retire();
    return {};
}

auto CSpace::prepare_xfer(CSpace& dst, const XferSpec& spec, resource::Reservation reservation) noexcept
    -> std::expected<Xfer, CSpaceError> {
    auto source = snapshot(spec.source);
    if (!source) return std::unexpected(source.error());
    auto& pin = source->pin;
    if (source->token && spec.op != XferOp::Move) return std::unexpected(CSpaceError::Denied);
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
                auto v = make_attenuation_ceiling(pin.kind(), view, limit);
                return v ? std::expected<View, CSpaceError>{*v}
                         : std::unexpected(CSpaceError::InvalidDescriptor);
            }
        }, spec.limit);
        if (!requested) return std::unexpected(requested.error());
        auto valid = compose(pin.kind(), view, *requested);
        if (!valid) return std::unexpected(policy_error(valid.error()));
        view = *requested;
        auto made = [&]() noexcept -> std::expected<GrantRef, CSpaceError> {
            if (!derive) {
                auto ref = pin.clone();
                return ref ? std::expected<GrantRef, CSpaceError>{std::move(*ref)}
                           : std::unexpected(grant_error(ref.error()));
            }
            auto usage = reservation
                ? std::expected<resource::Reservation, CSpaceError>{std::move(reservation)}
                : reserve_grant();
            if (!usage) return std::unexpected(usage.error());
            auto target = pin.clone_target();
            if (!target) return std::unexpected(CSpaceError::GrantUnavailable);
            auto child = pin.graph().derive(std::move(*usage), pin, std::move(*target), view);
            return child ? std::expected<GrantRef, CSpaceError>{std::move(*child)}
                         : std::unexpected(grant_error(child.error()));
        }();
        if (!made) return std::unexpected(made.error());
        grant = std::move(*made);
    }
    auto slot = dst.reserve();
    if (!slot) return std::unexpected(slot.error());
    return Xfer{std::move(*slot), std::move(pin), std::move(grant),
                spec.source, source->view, view, spec.op};
}

auto CSpace::commit_xfers(CSpace& dst, std::span<Xfer> entries) noexcept
    -> std::expected<void, CSpaceError> {
    libk::scope_exit notify{[&]() noexcept { post(); if (&dst != this) dst.post(); }};
    if (entries.empty()) return {};
    if (entries.size() > IPC_MAX_CAPS) return std::unexpected(CSpaceError::InvalidDescriptor);
    resource::Usage release;
    sync::Pair locks{lock_, dst.lock_};
    std::array<sync::Spin*, IPC_MAX_CAPS * 2> slots;
    usize n{};
    for (auto& entry : entries) {
        auto* target = dst.reserved(entry.slot);
        if (!target) return std::unexpected(CSpaceError::InvalidState);
        slots[n++] = &target->lock;
        if (entry.op == XferOp::Move) {
            auto* source = slot(entry.source.index());
            if (!source) return std::unexpected(CSpaceError::InvalidHandle);
            slots[n++] = &source->lock;
        }
    }
    sync::Set local{std::span{slots}.first(n)};
    // Publication and revoke share the Grant structure boundary. Preparing a
    // transfer is not admission to publish it after its branch has closed.
    auto& graph = entries.front().pin.graph();
    sync::Lock admission{graph.lock_};
    if (reassigning_ || !dst.accepting_ || dst.reassigning_) return std::unexpected(CSpaceError::InvalidState);
    for (auto& entry : entries) {
        if (!dst.reserved(entry.slot)) return std::unexpected(CSpaceError::InvalidState);
        if (entry.op != XferOp::Move) continue;
        auto* source = slot(entry.source.index());
        if (!accepting_ || !source || source->generation != entry.source.generation()
            || source->state != SlotState::Occupied
            || source->storage.capability.grant.key() != entry.pin.key()
            || source->storage.capability.view.rights != entry.original.rights
            || source->storage.capability.view.data != entry.original.data)
            return std::unexpected(CSpaceError::InvalidState);
    }
    std::array<const object::ref<>*, IPC_MAX_CAPS> targets;
    usize count{};
    for (const auto& entry : entries) {
        if (&entry.pin.graph() != &graph) return std::unexpected(CSpaceError::GrantUnavailable);
        const auto* node = graph.find(entry.pin.key());
        if (!node) return std::unexpected(CSpaceError::GrantUnavailable);
        targets[count++] = &node->target;
    }
    const bool done = object::ref<>::publish(std::span{targets}.first(count), [&] {
      for (auto& entry : entries) {
        Capability cap;
        if (entry.op == XferOp::Move) {
            auto* source = slot(entry.source.index());
            cap = std::move(source->storage.capability);
            libk::destroy_at(&source->storage.capability);
            unlink_occupied(entry.source.index(), *source);
            source->state = SlotState::Empty;
            libk_assert(live_slots_ != 0);
            --live_slots_;
            push_free(entry.source.index(), *source);
        } else cap = Capability{std::move(entry.grant), entry.view};
        dst.publish(entry.slot, std::move(cap));
      }
    });
    if (!done) return std::unexpected(CSpaceError::GrantUnavailable);
    usize moved{};
    for (auto& entry : entries) {
        dst.usage_.merge(std::move(entry.slot.usage_));
        moved += entry.op == XferOp::Move;
    }
    if (usage_ && moved) release = usage_.split({.caps = moved});
    admission.restore();
    local.release();
    locks.release();
    release.reset();
    finish_retire();
    if (&dst != this) dst.finish_retire();
    return {};
}

auto CSpace::transfer(Handle source, CSpace& dst, XferOp op, XferLimit limit, resource::Reservation reservation) noexcept
    -> std::expected<Handle, CSpaceError> {
    auto entry = prepare_xfer(dst, {source, std::move(limit), op}, std::move(reservation));
    if (!entry) return std::unexpected(entry.error());
    const auto handle = entry->slot.handle();
    auto done = commit_xfers(dst, std::span{&*entry, 1});
    return done ? std::expected<Handle, CSpaceError>{handle} : std::unexpected(done.error());
}

auto CSpace::revoke(Handle handle, Source::Context* context, GrantId selected) noexcept -> std::expected<WaitResult, CSpaceError> {
    libk::scope_exit notify{[&]() noexcept { post(); }};
    sync::Lock guard{lock_};
    auto* found = handle ? slot(handle.index()) : nullptr;
    if (!found || found->generation != handle.generation() || found->state != SlotState::Occupied)
        return std::unexpected(CSpaceError::InvalidHandle);
    const auto& cap = found->storage.capability;
    if (!cap.view.rights.contains(Right::Revoke)) return std::unexpected(CSpaceError::Denied);
    // Slot admission permits only monotonic control of its original lineage.
    // No payload acquisition or new authorization is performed on a tombstone.
    const auto result = cap.grant.graph().close(cap.grant.key(), context, selected);
    if (result.status == STATUS_OK || result.status == STATUS_PENDING) changed();
    return result;
}

auto CSpace::destroy(Handle source_handle) noexcept
    -> std::expected<void, CSpaceError> {
    auto copied = snapshot(source_handle);
    if (!copied) {
        return std::unexpected(copied.error());
    }
    Snapshot source = std::move(copied).value();
    GrantPin pin = std::move(source.pin);
    if (!source.view.rights.contains(Right::Destroy)) {
        return std::unexpected(CSpaceError::Denied);
    }
    auto target = pin.clone_target();
    if (!target) return std::unexpected(CSpaceError::InvalidHandle);
    if (pin.kind() == ObjVSpace) {
        const auto* limit = std::get_if<VmLimit>(&source.view.data);
        auto space = target.value().as<mm::VSpace>();
        // Region-local Destroy cannot retire the containing address space.
        if (!space || limit == nullptr || !space.value()->can_destroy_object(*limit))
            return std::unexpected(CSpaceError::Denied);
    }
    return target->destroy() ? std::expected<void, CSpaceError>{}
        : std::unexpected(CSpaceError::GrantUnavailable);
}

auto CSpace::snapshot(Handle handle) noexcept -> std::expected<Snapshot, CSpaceError> {
    auto* occupied = handle ? slot(handle.index()) : nullptr;
    if (!occupied) return std::unexpected(CSpaceError::InvalidHandle);
    // The successful Grant operation is the use-admission point. Slot removal
    // and MOVE take this same local lock, so no detached key gains new use.
    std::expected<GrantPin, GrantError> acquired;
    View original;
    u64 token{};
    {
        sync::Lock guard{occupied->lock};
        if (!accepting_.load(std::memory_order_acquire)) return std::unexpected(CSpaceError::InvalidState);
        if (occupied->generation != handle.generation() || occupied->state != SlotState::Occupied)
            return std::unexpected(CSpaceError::InvalidHandle);
        acquired = occupied->storage.capability.grant.acquire();
        original = occupied->storage.capability.view;
        token = occupied->storage.capability.facet.get().token;
    }
    if (!acquired) return std::unexpected(grant_error(acquired.error()));
    auto view = compose(acquired->kind(), acquired->ceiling(), original);
    if (!view) return std::unexpected(policy_error(view.error()));
    return Snapshot{std::move(*acquired), *view, token};
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
    target->state = SlotState::Occupied;
    link_occupied(r.handle_.index(), *target);
    r.disarm();
}

void CSpace::rollback(Handle handle) noexcept {
    {
        sync::Lock guard{lock_};
        Slot* const reserved = handle ? slot(handle.index()) : nullptr;
        libk_assert(reserved != nullptr);
        sync::Lock local{reserved->lock};
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
        needs_mid = needs_root || root_.load()->children[high] == nullptr;
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

    resource::Usage capacity{};
    if (allocation_ != nullptr) {
        auto charged = allocation_->acquire({.memory = required * mm::page_size});
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
            root_.store(new_root, std::memory_order_release);
        }
        auto* mid = static_cast<DirPage*>(root_.load()->children[high].load(std::memory_order_acquire));
        if (needs_mid) {
            libk_assert(mid == nullptr);
            root_.load()->children[high].store(new_mid, std::memory_order_release);
            mid = new_mid;
        }
        const usize low = leaf_number & (dir_entries - 1);
        libk_assert(mid->children[low] == nullptr);
        mid->children[low].store(new_leaf, std::memory_order_release);
        ++next_leaf_;
        page_count_ += required;
        usage_.merge(std::move(capacity));
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
    // Published pages stay until the actual Env binding / CSpace use drains.
    auto* root = root_.load(std::memory_order_acquire);
    if (index > Handle::max_index || !root) return nullptr;
    const auto leaf_number = index >> leaf_bits;
    auto* mid = static_cast<const DirPage*>(root->children[leaf_number >> dir_bits].load(std::memory_order_acquire));
    auto* leaf = mid ? static_cast<const LeafPage*>(mid->children[leaf_number & (dir_entries - 1)].load(std::memory_order_acquire)) : nullptr;
    return leaf ? &leaf->slots[index & (leaf_slots - 1)] : nullptr;
}

void CSpace::push_free(usize index, Slot& empty) noexcept {
    libk_assert(index <= Handle::max_index);
    libk_assert(empty.state == SlotState::Empty);
    libk_assert(empty.previous == invalid_index);
    empty.next = free_head_;
    free_head_ = static_cast<u32>(index);
}

void CSpace::link_occupied(usize index, Slot& occupied) noexcept {
    changed();
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
    changed();
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

void CSpace::retire(object::cleanup&& done) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(done);
    }
    retire();
}

void CSpace::changed() noexcept {
    libk_assert(lock_.held());
    if (epoch_ != std::numeric_limits<u64>::max()) ++epoch_;
    dirty_ = true;
}
void CSpace::retire() noexcept {
    bool retired;
    { sync::Lock guard{lock_}; accepting_ = false; retired = retired_; }
    if (retired) { complete_cleanup(); return; }
    run_work();
}
void CSpace::run_work() noexcept {
    auto execution = object::ref<>::retain_alive(job_.key());
    bool dirty, closing;
    {
        sync::Lock guard{lock_};
        if (advancing_) { again_ = true; return; }
        advancing_ = true; dirty = std::exchange(dirty_, false); closing = !accepting_;
    }
    libk::scope_exit leave{[&]() noexcept {
        sync::Lock guard{lock_};
        advancing_ = false;
        const bool again = std::exchange(again_, false);
        if (!retired_ && (again || dirty_ || (closing && (occupied_head_ != invalid_index || releasing_)))) {
            libk_assert(work_); post();
        }
    }};
    if (dirty) object::changed(job_.key());
    if (!closing) return;
    for (usize step = 0; step < 8; ++step) {
        Capability released;
        resource::Usage release;
        {
            sync::Lock guard{lock_};
            if (occupied_head_ == invalid_index) break;
            const auto index = occupied_head_;
            auto& current = *slot(index);
            sync::Lock local{current.lock};
            unlink_occupied(index, current);
            auto& cap = current.storage.capability;
            released = std::move(cap); libk::destroy_at(&cap);
            if (usage_) release = usage_.split({.caps=1});
            current.state = SlotState::Empty;
            libk_assert(live_slots_); --live_slots_;
        }
    }
    finish_retire();
    bool releasing;
    { sync::Lock guard{lock_}; releasing = releasing_; }
    if (releasing) release();
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
    {
        sync::Lock guard{lock_};
        libk_assert(bindings_ != 0);
        --bindings_;
    }
    finish_retire();
}

auto CSpace::escrow_move(
    Handle source_handle,
    Capability& cap,
    Reservation& reservation) noexcept
    -> std::expected<void, CSpaceError> {
    libk::scope_exit notify{[&]() noexcept { post(); }};
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
    sync::Lock local{source->lock};
    unlink_occupied(source_handle.index(), *source);
    Capability capability = std::move(source->storage.capability);
    libk::destroy_at(&source->storage.capability);
    cap = std::move(capability);
    source->state = SlotState::Reserved;
    reservation = Reservation{*this, source_handle, {}};
    retain_escrow();
    return {};
}

auto CSpace::escrow_restore(
    Reservation& reservation,
    Capability&& cap) noexcept -> bool {
    libk::scope_exit notify{[&]() noexcept { post(); }};
    sync::Lock guard{lock_};
    if (!accepting_ || reservation.owner_ != this || !cap.grant) {
        return false;
    }
    const Handle handle = reservation.handle_;
    Slot* const target = handle ? slot(handle.index()) : nullptr;
    if (target == nullptr || target->generation != handle.generation()
        || target->state != SlotState::Reserved) {
        return false;
    }
    sync::Lock local{target->lock};
    libk::construct_at(
        &target->storage.capability, std::move(cap));
    target->state = SlotState::Occupied;
    link_occupied(handle.index(), *target);
    reservation.disarm();
    release_escrow();
    return true;
}

auto CSpace::escrow_drop(
    Reservation& reservation) noexcept -> resource::Usage {
    resource::Usage release{};
    {
        sync::Lock guard{lock_};
        libk_assert(reservation.owner_ == this);
        const Handle handle = reservation.handle_;
        Slot* const target = handle ? slot(handle.index()) : nullptr;
        libk_assert(target != nullptr
            && target->generation == handle.generation()
            && target->state == SlotState::Reserved);
        sync::Lock local{target->lock};
        if (usage_) release = usage_.split({.caps = 1});
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
    return release;
}

void CSpace::retain_escrow() noexcept {
    libk_assert(escrows_ != std::numeric_limits<usize>::max());
    ++escrows_;
}

void CSpace::release_escrow() noexcept {
    libk_assert(escrows_ != 0);
    --escrows_;
}

void CSpace::bind_allocation(
    resource::Allocation& allocation) noexcept {
    libk_assert(allocation_ == nullptr && allocation);
    allocation_ = &allocation;
}

auto CSpace::binding_count() const noexcept -> usize {
    sync::Lock guard{lock_};
    return bindings_;
}

void CSpace::finish_retire() noexcept {
    {
        sync::Lock guard{lock_};
        if (accepting_ || growing_ || preparing_ || reassigning_ || live_slots_ || bindings_ || escrows_ || releasing_ || retired_) return;
        releasing_ = true; retiring_ = root_.exchange(nullptr);
        libk_assert(usage_.amount().caps == 0);
        free_head_ = occupied_head_ = invalid_index;
        quarantined_slots_ = 0;
    }
    run_work();
}

void CSpace::release() noexcept {
    for (usize step = 0; retiring_ && step < 8; ++step) {
        mm::Page page;
        const auto high = next_leaf_ / dir_entries;
        auto* empty = next_leaf_ % dir_entries == 0 && high < dir_entries
            ? static_cast<DirPage*>(retiring_->children[high].load()) : nullptr;
        if (empty) {
            libk_assert(!empty->children[0]);
            page = empty->page; retiring_->children[high] = nullptr; libk::destroy_at(empty);
        } else if (next_leaf_) {
            const auto index = --next_leaf_;
            auto* mid = static_cast<DirPage*>(retiring_->children[index / dir_entries].load());
            auto* leaf = static_cast<LeafPage*>(mid->children[index % dir_entries].load());
            libk_assert(leaf);
            page = leaf->page; mid->children[index % dir_entries] = nullptr; libk::destroy_at(leaf);
        } else {
            page = retiring_->page; libk::destroy_at(retiring_); retiring_ = nullptr;
        }
        auto backing = pages_.detach(page); libk_assert(backing); backing->reset();
        { sync::Lock guard{lock_}; libk_assert(page_count_); --page_count_; }
    }
    if (retiring_) return;
    pages_.reset(); usage_.reset();
    {
        sync::Lock guard{lock_};
        libk_assert(releasing_ && !page_count_);
        releasing_ = false; retired_ = true;
    }
    if (work_) work_->close(job_);
    complete_cleanup();
}

void CSpace::complete_cleanup() noexcept {
    object::cleanup done;
    {
        sync::Lock guard{lock_};
        if (retired_) done = std::move(cleanup_);
    }
    if (done) done.complete();
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
