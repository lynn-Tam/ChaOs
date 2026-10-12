#include <expected>
#include <cap/grant.hpp>

#include <object/ref.hpp>

#include <cap/cap.hpp>
#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <console.hpp>
#include <limits>
#include <libk/memory.hpp>
#include <utility>
#include <sync.hpp>

namespace cap {

GrantWork::GrantWork(GrantWork&& other) noexcept
    : attachment_(std::exchange(other.attachment_, nullptr)) {}

auto GrantWork::operator=(GrantWork&& other) noexcept -> GrantWork& {
    if (this != &other) {
        reset();
        attachment_ = std::exchange(other.attachment_, nullptr);
    }
    return *this;
}

GrantWork::~GrantWork() noexcept {
    reset();
}

void GrantWork::reset() noexcept {
    GrantAttachment* const attachment =
        std::exchange(attachment_, nullptr);
    if (attachment != nullptr) {
        attachment->drop_work();
    }
}

GrantAttachment::~GrantAttachment() noexcept {
    const State current = static_cast<State>(
        state_.load(std::memory_order_acquire));
    libk_assert(current == State::Idle || current == State::Detached);
    libk_assert(graph_ == nullptr && node_ == nullptr);
    libk_assert(work_.load(std::memory_order_acquire) == 0);
}

auto GrantAttachment::attached() const noexcept -> bool {
    const State current = static_cast<State>(
        state_.load(std::memory_order_acquire));
    return current == State::Attached || current == State::Invalidating;
}

auto GrantAttachment::busy() const noexcept -> bool {
    return work_.load(std::memory_order_acquire) != 0;
}

void GrantAttachment::reset() noexcept {
    libk_assert(graph_ == nullptr && node_ == nullptr);
    libk_assert(work_.load(std::memory_order_acquire) == 0);
    const State current = static_cast<State>(
        state_.load(std::memory_order_acquire));
    libk_assert(current == State::Detached || current == State::Idle);
    state_.store(
        static_cast<u8>(State::Idle), std::memory_order_release);
}

auto GrantAttachment::detach() noexcept -> bool {
    Graph* const graph = graph_;
    if (graph == nullptr) {
        return static_cast<State>(
            state_.load(std::memory_order_acquire)) == State::Detached
            && !busy();
    }
    return graph->detach(*this);
}

void GrantAttachment::drop_work() noexcept {
    const usize previous = work_.fetch_sub(1, std::memory_order_seq_cst);
    libk_assert(previous != 0);
    u8 draining = static_cast<u8>(State::Draining);
    if (previous == 1 && state_.compare_exchange_strong(
                draining, static_cast<u8>(State::Detached), std::memory_order_seq_cst, std::memory_order_seq_cst)) {
        libk_assert(ops_ != nullptr && ops_->released != nullptr);
        ops_->released(context_);
    }
}

Graph::Graph(mm::Pmm& pmm, ::WorkQueue& work) noexcept
    : Graph(pmm, work, Quota{}) {}

Graph::Graph(mm::Pmm& pmm, ::WorkQueue& work, Quota quota) noexcept
    : pmm_(&pmm),
      quota_(quota), executor_(work), job_(Work::Fn::bind<&Graph::run_work>(*this)) {}

auto GrantLease::key() const noexcept -> GrantKey {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    const auto& node = *static_cast<const Graph::Node*>(d.node);
    libk_assert(node.slot->generation.load(std::memory_order_acquire)
        == d.gen);
    return Graph::key_of(node);
}

auto GrantLease::graph() const noexcept -> Graph& {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return *d.graph;
}

auto GrantLease::kind() const noexcept -> ObjKind {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return static_cast<const Graph::Node*>(d.node)->target.kind();
}

auto GrantLease::get() const noexcept -> void* {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    // Revocation and reclamation cannot move Node::target until operations=0.
    return static_cast<const Graph::Node*>(d.node)->target.get();
}

auto GrantLease::target_live() const noexcept -> bool {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return static_cast<const Graph::Node*>(d.node)->target.live();
}

auto GrantLease::ceiling() const noexcept -> View {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return static_cast<const Graph::Node*>(d.node)->ceiling;
}

auto GrantLease::clone_target() const noexcept
    -> std::expected<object::ref<>, object::error> {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    const auto& target = static_cast<const Graph::Node*>(d.node)->target;
    if (!target.live()) return std::unexpected(object::error::closed);
    return target;
}

auto GrantLease::attach(GrantAttachment& attachment) const noexcept
    -> std::expected<void, GrantError> {
    const auto& d = h_.get();
    if (d.graph == nullptr) {
        return std::unexpected(GrantError::InvalidKey);
    }
    return d.graph->attach(*this, attachment);
}

auto GrantLease::mint(resource::Reservation&& charge, View ceiling) const noexcept
    -> std::expected<GrantRef, GrantError> {
    const auto& d = h_.get();
    if (!d.graph) return std::unexpected(GrantError::InvalidKey);
    auto* parent = static_cast<Graph::Node*>(d.node);
    if (!validate_ceiling(parent->target.kind(), ceiling))
        return std::unexpected(GrantError::RightsViolation);
    return d.graph->create(std::move(charge), object::ref<>{parent->target}, ceiling, parent);
}

void GrantLease::Drop::operator()(Data& d) const noexcept {
    d.graph->drop_lease(d.node, d.gen);
}

auto GrantRef::key() const noexcept -> GrantKey {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    libk_assert(d.slot != nullptr && d.gen != 0);
    return GrantKey{reinterpret_cast<usize>(d.slot), d.gen};
}

auto GrantRef::graph() const noexcept -> Graph& {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return *d.graph;
}

auto GrantRef::clone() const noexcept
    -> std::expected<GrantRef, GrantError> {
    const auto& d = h_.get();
    if (d.graph == nullptr) {
        return std::unexpected(GrantError::InvalidKey);
    }
    return d.graph->ref(key());
}

auto GrantRef::acquire() const noexcept
    -> std::expected<GrantLease, GrantError> {
    const auto& d = h_.get();
    if (d.graph == nullptr) {
        return std::unexpected(GrantError::InvalidKey);
    }
    return d.graph->try_acquire(
        *static_cast<Graph::Slot*>(d.slot), d.gen);
}

void GrantRef::Drop::operator()(Data& d) const noexcept {
    d.graph->drop_ref(d.slot, d.gen);
}

void GrantRevoke::initialize(usize pending) noexcept { completion_.initialize(pending); }

void GrantRevoke::acknowledge() noexcept { static_cast<void>(completion_.acknowledge()); }

Graph::~Graph() noexcept {
    executor_.close(job_);
    libk_assert(work_.empty());
    libk_assert(storage_.live() == 0 && growing_ == 0);
    while (auto* page = storage_.take_page()) release_page(*page);
}

auto Graph::create_root(
    object::ref<> target,
    View ceiling) noexcept -> std::expected<GrantRef, GrantError> {
    return create_root({}, std::move(target), ceiling);
}

auto Graph::create_root(
    resource::Reservation&& charge,
    object::ref<> target,
    View ceiling) noexcept -> std::expected<GrantRef, GrantError> {
    if (!target || !validate_ceiling(target.kind(), ceiling)) {
        return std::unexpected(GrantError::RightsViolation);
    }
    return create(
        std::move(charge), std::move(target), ceiling, nullptr);
}

auto Graph::derive(
    const GrantLease& source,
    object::ref<> target,
    View ceiling) noexcept -> std::expected<GrantRef, GrantError> {
    return derive({}, source, std::move(target), ceiling);
}

auto Graph::derive(
    resource::Reservation&& charge,
    const GrantLease& source,
    object::ref<> target,
    View ceiling) noexcept -> std::expected<GrantRef, GrantError> {
    if (source.h_.get().graph != this || !target) {
        return std::unexpected(GrantError::InvalidKey);
    }
    auto* const parent = static_cast<Node*>(source.h_.get().node);
    if (target.kind() != parent->target.kind()) {
        return std::unexpected(GrantError::WrongKind);
    }
    if (target.id() != parent->target.id()) {
        return std::unexpected(GrantError::InvalidKey);
    }
    if (!validate_ceiling(target.kind(), ceiling)
        || !attenuates(
            target.kind(),
            parent->ceiling,
            ceiling)) {
        return std::unexpected(GrantError::RightsViolation);
    }
    return create(
        std::move(charge), std::move(target), ceiling, parent);
}

auto Graph::create(
    resource::Reservation&& charge,
    object::ref<> target,
    View ceiling,
    Node* parent) noexcept -> std::expected<GrantRef, GrantError> {
    if (!target.live()) return std::unexpected(GrantError::InvalidState);
    auto claimed = claim_slot();
    if (!claimed) {
        return std::unexpected(claimed.error());
    }
    Slot* const slot = claimed.value();
    Node* const node = libk::construct_at(
        slot->node(),
        *slot,
        std::move(target),
        ceiling,
        parent,
        std::move(charge));

    object::ref<> returned{};
    bool rejected{};
    {
        sync::Lock guard{lock_};
        if (parent != nullptr && (admission_closed(
                    parent->slot->operations.load(std::memory_order_acquire))
            || parent->slot->state.load(std::memory_order_acquire)
                != GrantState::Live)) {
            rejected = true;
        } else {
            node->refs = 1;
            if (parent != nullptr) {
                parent->children.push_back(*node);
            }
            // claim_slot() reserves storage but does not publish a Node.
            // Graph scans may dereference the payload only after construction
            // and parent linkage are complete under this lock.
            slot->occupied.store(true, std::memory_order_release);
        }
    }
    if (rejected) {
        returned = std::move(node->target);
        auto refund = node->sponsorship.detach();
        libk::destroy_at(node);
        {
            sync::Lock guard{lock_};
            slot->state.store(
                GrantState::Revoked, std::memory_order_release);
            slot->occupied.store(false, std::memory_order_release);
            static_cast<void>(storage_.release(*slot, false));
        }
        returned.reset();
        refund.complete();
        return std::unexpected(GrantError::InvalidState);
    }
    return (GrantRef{
        *this, slot,
        slot->generation.load(std::memory_order_relaxed)});
}

auto Graph::claim_slot() noexcept -> std::expected<Slot*, GrantError> {
    for (;;) {
        {
            sync::Lock guard{lock_};
            if (storage_.exhausted()) return std::unexpected(GrantError::GenerationExhausted);
            if (storage_.live() >= quota_.nodes) return std::unexpected(GrantError::QuotaExceeded);
            if (auto entry = storage_.claim(); entry.slot) {
                auto* slot = entry.slot;
                libk_assert(!slot->occupied.load(std::memory_order_relaxed));
                libk_assert(operation_count(slot->operations.load(std::memory_order_relaxed)) == 0);
                slot->generation.store(entry.generation, std::memory_order_relaxed);
                slot->state.store(GrantState::Live, std::memory_order_relaxed);
                slot->operations.store(0, std::memory_order_relaxed);
                return (slot);
            }
            const usize max_pages = quota_.nodes / slots_per_page
                + (quota_.nodes % slots_per_page != 0);
            if (storage_.pages() + growing_ >= max_pages)
                return std::unexpected(GrantError::QuotaExceeded);
            ++growing_;
        }
        auto made = make_page();
        sync::Lock guard{lock_};
        libk_assert(growing_ != 0);
        --growing_;
        if (!made) return std::unexpected(made.error());
        storage_.add(*made.value());
    }
}

auto Graph::make_page() noexcept -> std::expected<PageHeader*, GrantError> {
    auto backing = pmm_->allocate_page();
    if (!backing) return std::unexpected(GrantError::OutOfMemory);
    return (Storage::make(std::move(backing).value(), [](Slot&) noexcept {}));
}

auto Graph::locate(GrantKey key) noexcept -> Node* {
    return const_cast<Node*>(
        static_cast<const Graph*>(this)->locate(key));
}

auto Graph::locate(GrantKey key) const noexcept -> const Node* {
    if (!key.valid()) {
        return nullptr;
    }
    const auto* slot = storage_.find(key.slot);
    return slot && slot->occupied.load(std::memory_order_acquire)
            && slot->generation.load(std::memory_order_relaxed) == key.generation
        ? slot->node() : nullptr;
}

auto Graph::find(GrantKey key) noexcept -> Node* {
    return const_cast<Node*>(
        static_cast<const Graph*>(this)->find(key));
}

auto Graph::find(GrantKey key) const noexcept -> const Node* {
    const Node* const node = locate(key);
    return node != nullptr
            && !admission_closed(node->slot->operations.load(std::memory_order_acquire))
        ? node : nullptr;
}

auto Graph::key_of(const Node& node) noexcept -> GrantKey {
    return GrantKey{
        reinterpret_cast<usize>(node.slot),
        node.slot->generation.load(std::memory_order_relaxed),
    };
}

auto Graph::ref(GrantKey key) noexcept
    -> std::expected<GrantRef, GrantError> {
    sync::Lock guard{lock_};
    Node* const node = find(key);
    return node != nullptr
        ? try_ref(*node)
        : std::expected<GrantRef, GrantError>{
              std::unexpected(GrantError::InvalidKey)};
}

auto Graph::try_ref(Node& node) noexcept
    -> std::expected<GrantRef, GrantError> {
    libk_assert(!admission_closed(node.slot->operations.load(std::memory_order_acquire)));
    if (node.slot->state.load(std::memory_order_acquire)
        != GrantState::Live) {
        return std::unexpected(GrantError::InvalidState);
    }
    if (node.refs == std::numeric_limits<usize>::max()) {
        return std::unexpected(GrantError::QuotaExceeded);
    }
    ++node.refs;
    return (GrantRef{
        *this, node.slot,
        node.slot->generation.load(std::memory_order_relaxed)});
}

auto Graph::acquire(GrantKey key) noexcept
    -> std::expected<GrantLease, GrantError> {
    sync::Lock guard{lock_};
    Node* const node = find(key);
    return node != nullptr
        ? try_acquire(*node->slot, key.generation)
        : std::expected<GrantLease, GrantError>{
              std::unexpected(GrantError::InvalidKey)};
}

auto Graph::try_acquire(Slot& slot, u64 generation) noexcept
    -> std::expected<GrantLease, GrantError> {
    if (slot.generation.load(std::memory_order_acquire) != generation
        || !slot.occupied.load(std::memory_order_acquire)
        || slot.state.load(std::memory_order_acquire)
            != GrantState::Live) {
        return std::unexpected(GrantError::InvalidState);
    }

    usize operations = slot.operations.load(std::memory_order_acquire);
    for (;;) {
        if (admission_closed(operations)) {
            return std::unexpected(GrantError::InvalidState);
        }
        if (operation_count(operations) == operation_closed - 1) {
            return std::unexpected(GrantError::QuotaExceeded);
        }
        if (slot.operations.compare_exchange_weak(operations, operations + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            break;
        }
    }

    if (slot.generation.load(std::memory_order_acquire) != generation
        || !slot.occupied.load(std::memory_order_acquire)
        || slot.state.load(std::memory_order_acquire)
            != GrantState::Live) {
        release_operation(slot);
        return std::unexpected(GrantError::InvalidState);
    }
    Node& node = *slot.node();
    return (GrantLease{
        *this, &node, generation});
}

auto Graph::attach(
    const GrantLease& source,
    GrantAttachment& attachment) noexcept
    -> std::expected<void, GrantError> {
    if (source.h_.get().graph != this
        || attachment.graph_ != nullptr
        || attachment.node_ != nullptr
        || attachment.ops_ == nullptr
        || attachment.ops_->invalidate == nullptr
        || attachment.ops_->released == nullptr
        || static_cast<GrantAttachment::State>(
            attachment.state_.load(std::memory_order_relaxed))
            != GrantAttachment::State::Idle) {
        return std::unexpected(GrantError::InvalidState);
    }
    auto* const node = static_cast<Node*>(source.h_.get().node);
    sync::Lock guard{lock_};
    if (node->slot->generation.load(std::memory_order_acquire)
            != source.h_.get().gen
        || node->slot->state.load(std::memory_order_acquire)
            != GrantState::Live
        || admission_closed(node->slot->operations.load(std::memory_order_acquire))) {
        return std::unexpected(GrantError::InvalidState);
    }
    attachment.graph_ = this;
    attachment.node_ = node;
    attachment.generation_ = source.h_.get().gen;
    attachment.state_.store(
        static_cast<u8>(GrantAttachment::State::Attached), std::memory_order_release);
    node->attachments.push_back(attachment);
    return {};
}

void Graph::drop_ref(void* raw, u64 generation) noexcept {
    libk_assert(raw != nullptr && generation != 0);
    reclaim(GrantKey{reinterpret_cast<usize>(raw), generation}, true);
}

void Graph::drop_lease(void* raw, u64 generation) noexcept {
    libk_assert(raw != nullptr);
    auto& node = *static_cast<Node*>(raw);
    libk_assert(node.slot->generation.load(std::memory_order_acquire)
        == generation);
    release_operation(*node.slot);
}

void Graph::release_operation(Slot& slot) noexcept {
    usize operations = slot.operations.load(std::memory_order_acquire);
    for (;;) {
        libk_assert(operation_count(operations) != 0);
        if (admission_closed(operations)) {
            break;
        }
        if (slot.operations.compare_exchange_weak(operations, operations - 1, std::memory_order_release, std::memory_order_relaxed)) {
            // The steady-state release is wholly slot-local. If close races
            // this CAS, either the CAS wins and close samples the new count,
            // or the closed bit changes the word and sends us to cold path.
            return;
        }
    }

    bool schedule{};
    {
        // Once admission is closed, the graph lock is the lifetime barrier
        // between the final lease and node destruction. work_retained means
        // one queued or in-flight worker already owns the retry obligation.
        sync::Lock guard{lock_};
        libk_assert(slot.occupied.load(std::memory_order_acquire));
        const usize previous =
            slot.operations.fetch_sub(1, std::memory_order_acq_rel);
        libk_assert(admission_closed(previous)
            && operation_count(previous) != 0);
        if (operation_count(previous) == 1
            && !slot.work_retained.exchange(true, std::memory_order_acq_rel)) {
            schedule = true;
        }
    }
    if (schedule) {
        enqueue(slot);
        kick_work();
    }
}

void Graph::enqueue(Slot& slot) noexcept {
    sync::Lock guard{work_lock_};
    libk_assert(slot.work_retained.load(std::memory_order_acquire));
    if (!slot.work_hook.is_linked()) {
        work_.push_back(slot);
    }
}

auto Graph::take_work() noexcept -> Slot* {
    sync::Lock guard{work_lock_};
    return work_.empty() ? nullptr : &work_.pop_front();
}

void Graph::kick_work() noexcept { executor_.post(job_); }

void Graph::run_work() noexcept {
    if (service(8)) executor_.post(job_);
}

auto Graph::service(usize budget) noexcept -> bool {
    for (usize i = 0; i < budget; ++i) {
        auto* slot = take_work();
        if (!slot) break;
        service_slot(*slot);
    }
    return work_pending();
}

auto Graph::work_pending() const noexcept -> bool {
    sync::Lock guard{work_lock_};
    return !work_.empty();
}

void Graph::service_slot(Slot& slot) noexcept {
    object::ref<> target_ref{};
    GrantRevoke* completed{};
    GrantAttachment* attachment{};
    GrantWork work{};
    bool reclaimable{};
    {
        sync::Lock guard{lock_};
        if (!slot.occupied.load(std::memory_order_acquire)) {
            slot.work_retained.store(false, std::memory_order_release);
            return;
        }
        Node& node = *slot.node();
        const GrantState state =
            slot.state.load(std::memory_order_acquire);
        const usize operations =
            slot.operations.load(std::memory_order_acquire);
        if (state == GrantState::Live && admission_closed(operations)) {
            libk_assert(operation_count(operations) == 0
                && node.refs == 0 && node.attachments.empty()
                && node.children.empty());
        } else if (state == GrantState::Revoking
            && operation_count(operations) == 0
            && node.attachments.empty()) {
            slot.state.store(
                GrantState::Revoked, std::memory_order_release);
            completed = std::exchange(node.revoke, nullptr);
            target_ref = std::move(node.target);
        } else if (state == GrantState::Revoking) {
            for (GrantAttachment& candidate : node.attachments) {
                if (static_cast<GrantAttachment::State>(
                        candidate.state_.load(std::memory_order_relaxed))
                    != GrantAttachment::State::Attached) {
                    continue;
                }
                libk_assert(candidate.work_.load(std::memory_order_relaxed)
                    != std::numeric_limits<usize>::max());
                static_cast<void>(candidate.work_.fetch_add(1, std::memory_order_relaxed));
                candidate.state_.store(
                    static_cast<u8>(GrantAttachment::State::Invalidating), std::memory_order_release);
                attachment = &candidate;
                work = GrantWork{candidate};
                break;
            }
        }

        if (attachment == nullptr) {
            slot.work_retained.store(false, std::memory_order_release);
            reclaimable = node.refs == 0
                && operation_count(operations) == 0
                && node.attachments.empty()
                && node.children.empty();
        }
    }

    target_ref.reset();
    if (completed != nullptr) {
        completed->acknowledge();
    }
    if (attachment != nullptr) {
        attachment->ops_->invalidate(
            attachment->context_,
            std::move(work));
        enqueue(slot);
    } else if (reclaimable) {
        reclaim(GrantKey{
            reinterpret_cast<usize>(&slot),
            slot.generation.load(std::memory_order_relaxed)}, false);
    }
    retry_revokes();
    // Taking a retained slot always commits one canonical handoff: an
    // attachment enters invalidation, revoke reaches terminal state, or the
    // worker releases the retained obligation to its current blocker.
}

auto Graph::detach(GrantAttachment& attachment) noexcept -> bool {
    object::ref<> target{};
    GrantRevoke* completion{};
    void* reclaimable{};
    u64 reclaim_generation{};
    bool quiescent{};
    {
        sync::Lock guard{lock_};
        if (attachment.graph_ != this || attachment.node_ == nullptr) {
            return false;
        }
        auto& node = *static_cast<Node*>(attachment.node_);
        libk_assert(node.slot->generation.load(std::memory_order_acquire)
            == attachment.generation_);
        const u64 node_generation = attachment.generation_;
        const auto current = static_cast<GrantAttachment::State>(
            attachment.state_.load(std::memory_order_relaxed));
        libk_assert(current == GrantAttachment::State::Attached
            || current == GrantAttachment::State::Invalidating);
        node.attachments.erase(attachment);
        attachment.graph_ = nullptr;
        attachment.node_ = nullptr;
        attachment.generation_ = 0;
        attachment.state_.store(
            static_cast<u8>(GrantAttachment::State::Draining), std::memory_order_seq_cst);
        u8 draining = static_cast<u8>(GrantAttachment::State::Draining);
        quiescent = attachment.work_.load(std::memory_order_seq_cst) == 0
            && attachment.state_.compare_exchange_strong(
                    draining, static_cast<u8>(GrantAttachment::State::Detached), std::memory_order_seq_cst, std::memory_order_seq_cst);

        if (node.slot->state.load(std::memory_order_acquire)
                == GrantState::Revoking
            && operation_count(node.slot->operations.load(std::memory_order_acquire)) == 0
            && node.attachments.empty()) {
            node.slot->state.store(
                GrantState::Revoked, std::memory_order_release);
            completion = std::exchange(node.revoke, nullptr);
            target = std::move(node.target);
        }
        if (node.refs == 0 && node.children.empty()
            && !node.slot->work_retained.load(std::memory_order_acquire)) {
            reclaimable = &node;
            reclaim_generation = node_generation;
        }
    }
    target.reset();
    if (reclaimable != nullptr) {
        reclaim(GrantKey{
            reinterpret_cast<usize>(
                static_cast<Node*>(reclaimable)->slot),
            reclaim_generation}, false);
    }
    if (completion != nullptr) {
        completion->acknowledge();
        retry_revokes();
    }
    return quiescent;
}

void Graph::reclaim(
    GrantKey initial,
    bool drop_reference) noexcept {
    GrantKey current = initial;
    bool drop = drop_reference;
    while (current.valid()) {
        object::ref<> target{};
        resource::Refund refund{};
        Node* parent{};
        GrantKey parent_key{};
        Slot* slot{};
        bool orphaned{};
        {
            sync::Lock guard{lock_};
            Node* const found = locate(current);
            if (found == nullptr) {
                return;
            }
            Node& node = *found;
            if (drop) {
                // A permanent revoke invalidates all outstanding GrantRef
                // tokens. Their later destruction is a stale-key no-op.
                if (node.refs == 0) {
                    return;
                }
                --node.refs;
            }
            if (node.refs != 0 || !node.children.empty()
                || node.slot->work_retained.load(std::memory_order_acquire)) return;
            if (!node.attachments.empty()) {
                for (const auto& attachment : node.attachments)
                    if (attachment.ops_->retain) return;
                // No holder can use this branch again. Drain weak observers
                // through the same worker as explicit revocation, without a waiter.
                node.slot->operations.fetch_or(operation_closed, std::memory_order_acq_rel);
                node.slot->state.store(GrantState::Revoking, std::memory_order_release);
                node.slot->work_retained.store(true, std::memory_order_release);
                enqueue(*node.slot);
                orphaned = true;
            } else {
                const usize operations = node.slot->operations.fetch_or(operation_closed, std::memory_order_acq_rel);
                if (operation_count(operations) != 0) return;
                parent = node.parent;
                if (parent != nullptr) {
                    parent_key = key_of(*parent);
                    parent->children.erase(node);
                }
                target = std::move(node.target);
                slot = node.slot;
            }
        }

        if (orphaned) { kick_work(); return; }
        target.reset();
        refund = slot->node()->sponsorship.detach();

        {
            sync::Lock guard{lock_};
            libk::destroy_at(slot->node());
            libk_assert(slot->occupied.load(std::memory_order_acquire));
            slot->state.store(
                GrantState::Revoked, std::memory_order_release);
            slot->occupied.store(false, std::memory_order_release);
            static_cast<void>(storage_.release(*slot, false));
        }
        refund.complete();
        // A non-empty child list itself prevents parent reclamation; it does
        // not need a second synthetic reference count.
        current = parent_key;
        drop = false;
    }
}

auto Graph::state(GrantKey key) const noexcept
    -> std::expected<GrantState, GrantError> {
    sync::Lock guard{lock_};
    const Node* const node = locate(key);
    return node != nullptr
        ? (
              node->slot->state.load(std::memory_order_acquire))
        : std::expected<GrantState, GrantError>{
              std::unexpected(GrantError::InvalidKey)};
}

auto Graph::live_count() const noexcept -> usize {
    sync::Lock guard{lock_};
    return storage_.live();
}

auto Graph::node_charge() noexcept -> resource::budget {
    return resource::budget{.memory = sizeof(Slot)};
}

auto Graph::revoke_descendants(
    GrantKey source,
    GrantRevoke& completion) noexcept
    -> std::expected<void, GrantError> {
    return revoke(source, completion, false);
}

auto Graph::invalidate(
    GrantKey source,
    GrantRevoke& completion) noexcept
    -> std::expected<void, GrantError> {
    return revoke(source, completion, true);
}

auto Graph::revoke(
    GrantKey source_key,
    GrantRevoke& completion,
    bool include_source) noexcept -> std::expected<void, GrantError> {
    Node* source{};
    GrantRef source_hold{};
    {
        sync::Lock guard{lock_};
        source = find(source_key);
        if (source == nullptr) {
            return std::unexpected(GrantError::InvalidKey);
        }
        if (source->slot->state.load(std::memory_order_acquire)
                != GrantState::Live
            || completion.initialized()) {
            return std::unexpected(GrantError::InvalidState);
        }
        auto held = try_ref(*source);
        if (!held) {
            return std::unexpected(held.error());
        }
        source_hold = std::move(held).value();

        Node* const first = include_source ? source : next(*source, *source);
        usize pending{};
        for (Node* node = first; node; node = next(*source, *node)) {
            const auto state = node->slot->state.load(std::memory_order_acquire);
            if (state == GrantState::Revoking) return std::unexpected(GrantError::RevocationConflict);
            if (state != GrantState::Revoked) ++pending;
        }

        // Preflight and commit share the topology lock. Every selected node
        // contributes once, after its operations and attachments have drained.
        completion.initialize(pending + 1);
        for (Node* node = first; node; node = next(*source, *node)) {
            if (node->slot->state.load(std::memory_order_acquire) == GrantState::Revoked) continue;
            node->revoke = &completion;
            node->refs = 0;
            node->slot->work_retained.store(true, std::memory_order_release);
            static_cast<void>(node->slot->operations.fetch_or(operation_closed, std::memory_order_acq_rel));
            node->slot->state.store(GrantState::Revoking, std::memory_order_release);
            enqueue(*node->slot);
        }
    }

    kick_work();
    completion.acknowledge();
    return {};
}

void Graph::close(GrantKey source, GrantRevoke& request) noexcept {
    libk_assert(!request.initialized() && !request.source_.valid());
    for (;;) {
        const auto revoked = invalidate(source, request);
        if (revoked) break;
        libk_assert(revoked.error() == GrantError::RevocationConflict
            || revoked.error() == GrantError::InvalidKey
            || revoked.error() == GrantError::InvalidState);
        {
            sync::Lock guard{lock_};
            Node* const root = locate(source);
            bool conflict{};
            if (root) {
                for (Node* node = root; node; node = next(*root, *node)) {
                    const auto ops = node->slot->operations.load(std::memory_order_acquire);
                    conflict |= node->slot->state.load(std::memory_order_acquire) == GrantState::Revoking
                        || (admission_closed(ops) && operation_count(ops) != 0);
                }
            }
            if (conflict) {
                // Register under the completion lock, closing the lost-wake
                // window between a rejected attempt and descendant completion.
                request.source_ = source;
                request.next_ = revoke_retry_;
                revoke_retry_ = &request;
                return;
            }
            if (root && root->slot->state.load(std::memory_order_acquire) == GrantState::Live
                && !admission_closed(root->slot->operations.load(std::memory_order_acquire))) continue;
            request.initialize(1);
        }
        // A reclaimed generation or fully revoked subtree needs no new work.
        request.acknowledge();
        break;
    }
    if (request.completion_.notifiable()) request.completion_.notify_when_ready();
}

void Graph::retry_revokes() noexcept {
    GrantRevoke* pending{};
    {
        sync::Lock guard{lock_};
        pending = std::exchange(revoke_retry_, nullptr);
    }
    while (pending) {
        auto* next = pending->next_;
        const auto source = std::exchange(pending->source_, GrantKey{});
        pending->next_ = nullptr;
        close(source, *pending);
        pending = next;
    }
}

// Preorder successor within one subtree. The caller holds lock_; topology
// cannot change, and no recursion stack or auxiliary index is needed.
auto Graph::next(Node& root, Node& node) noexcept -> Node* {
    if (!node.children.empty()) return &node.children.front();
    for (Node* n = &node; n != &root; n = n->parent) {
        auto& siblings = n->parent->children;
        auto it = siblings.iterator_to(*n);
        if (++it != siblings.end()) return &*it;
    }
    return nullptr;
}

void Graph::release_page(PageHeader& page) noexcept {
    libk_assert(page.live == 0);
    for (const auto& slot : Storage::slots(page)) {
        libk_assert(!slot.occupied.load(std::memory_order_acquire));
    }
    auto backing = Storage::dispose(page);
    backing.reset();
}

} // namespace cap
