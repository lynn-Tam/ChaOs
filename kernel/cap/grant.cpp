#include <expected>
#include <cap/grant.hpp>

#include <object/ref.hpp>

#include <cap/cap.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <console.hpp>
#include <limits>
#include <libk/memory.hpp>
#include <utility>
#include <object/group.hpp>
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
        state_.load<libk::MemoryOrder::Acquire>());
    libk_assert(current == State::Idle || current == State::Detached);
    libk_assert(graph_ == nullptr && node_ == nullptr);
    libk_assert(work_.load<libk::MemoryOrder::Acquire>() == 0);
}

auto GrantAttachment::attached() const noexcept -> bool {
    const State current = static_cast<State>(
        state_.load<libk::MemoryOrder::Acquire>());
    return current == State::Attached || current == State::Invalidating;
}

auto GrantAttachment::busy() const noexcept -> bool {
    return work_.load<libk::MemoryOrder::Acquire>() != 0;
}

void GrantAttachment::reset() noexcept {
    libk_assert(graph_ == nullptr && node_ == nullptr);
    libk_assert(work_.load<libk::MemoryOrder::Acquire>() == 0);
    const State current = static_cast<State>(
        state_.load<libk::MemoryOrder::Acquire>());
    libk_assert(current == State::Detached || current == State::Idle);
    state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(State::Idle));
}

auto GrantAttachment::detach() noexcept -> bool {
    Graph* const graph = graph_;
    if (graph == nullptr) {
        return static_cast<State>(
            state_.load<libk::MemoryOrder::Acquire>()) == State::Detached
            && !busy();
    }
    return graph->detach(*this);
}

void GrantAttachment::drop_work() noexcept {
    const usize previous = work_.fetch_sub<libk::MemoryOrder::SeqCst>(1);
    libk_assert(previous != 0);
    u8 draining = static_cast<u8>(State::Draining);
    if (previous == 1 && state_.compare_exchange_strong<
            libk::MemoryOrder::SeqCst, libk::MemoryOrder::SeqCst>(
                draining, static_cast<u8>(State::Detached))) {
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
    libk_assert(node.slot->generation.load<libk::MemoryOrder::Acquire>()
        == d.gen);
    return Graph::key_of(node);
}

auto GrantLease::graph() const noexcept -> Graph& {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return *d.graph;
}

auto GrantLease::kind() const noexcept -> object::ObjectKind {
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
    return static_cast<const Graph::Node*>(d.node)->target.clone();
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
    auto target = parent->target.clone();
    if (!target) return std::unexpected(GrantError::InvalidState);
    return d.graph->create(std::move(charge), std::move(target).value(), ceiling, parent);
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
    object::ref<>&& target,
    View ceiling) noexcept -> std::expected<GrantRef, GrantError> {
    return create_root({}, std::move(target), ceiling);
}

auto Graph::create_root(
    resource::Reservation&& charge,
    object::ref<>&& target,
    View ceiling) noexcept -> std::expected<GrantRef, GrantError> {
    if (!target || !validate_ceiling(target.kind(), ceiling)) {
        return std::unexpected(GrantError::RightsViolation);
    }
    return create(
        std::move(charge), std::move(target), ceiling, nullptr);
}

auto Graph::derive(
    const GrantLease& source,
    object::ref<>&& target,
    View ceiling) noexcept -> std::expected<GrantRef, GrantError> {
    return derive({}, source, std::move(target), ceiling);
}

auto Graph::derive(
    resource::Reservation&& charge,
    const GrantLease& source,
    object::ref<>&& target,
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
    object::ref<>&& target,
    View ceiling,
    Node* parent) noexcept -> std::expected<GrantRef, GrantError> {
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
                    parent->slot->operations.load<libk::MemoryOrder::Acquire>())
            || parent->slot->state.load<libk::MemoryOrder::Acquire>()
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
            slot->occupied.store<libk::MemoryOrder::Release>(true);
        }
    }
    if (rejected) {
        returned = std::move(node->target);
        auto refund = node->sponsorship.detach();
        libk::destroy_at(node);
        {
            sync::Lock guard{lock_};
            slot->state.store<libk::MemoryOrder::Release>(
                GrantState::Revoked);
            slot->occupied.store<libk::MemoryOrder::Release>(false);
            static_cast<void>(storage_.release(*slot, false));
        }
        returned.reset();
        refund.complete();
        return std::unexpected(GrantError::InvalidState);
    }
    return (GrantRef{
        *this, slot,
        slot->generation.load<libk::MemoryOrder::Relaxed>()});
}

auto Graph::claim_slot() noexcept -> std::expected<Slot*, GrantError> {
    for (;;) {
        {
            sync::Lock guard{lock_};
            if (storage_.exhausted()) return std::unexpected(GrantError::GenerationExhausted);
            if (storage_.live() >= quota_.nodes) return std::unexpected(GrantError::QuotaExceeded);
            if (auto entry = storage_.claim(); entry.slot) {
                auto* slot = entry.slot;
                libk_assert(!slot->occupied.load<libk::MemoryOrder::Relaxed>());
                libk_assert(operation_count(slot->operations.load<libk::MemoryOrder::Relaxed>()) == 0);
                slot->generation.store<libk::MemoryOrder::Relaxed>(entry.generation);
                slot->state.store<libk::MemoryOrder::Relaxed>(GrantState::Live);
                slot->operations.store<libk::MemoryOrder::Relaxed>(0);
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
    return slot && slot->occupied.load<libk::MemoryOrder::Acquire>()
            && slot->generation.load<libk::MemoryOrder::Relaxed>() == key.generation
        ? slot->node() : nullptr;
}

auto Graph::find(GrantKey key) noexcept -> Node* {
    return const_cast<Node*>(
        static_cast<const Graph*>(this)->find(key));
}

auto Graph::find(GrantKey key) const noexcept -> const Node* {
    const Node* const node = locate(key);
    return node != nullptr
            && !admission_closed(node->slot->operations.load<
                libk::MemoryOrder::Acquire>())
        ? node : nullptr;
}

auto Graph::key_of(const Node& node) noexcept -> GrantKey {
    return GrantKey{
        reinterpret_cast<usize>(node.slot),
        node.slot->generation.load<libk::MemoryOrder::Relaxed>(),
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
    libk_assert(!admission_closed(node.slot->operations.load<
        libk::MemoryOrder::Acquire>()));
    if (node.slot->state.load<libk::MemoryOrder::Acquire>()
        != GrantState::Live) {
        return std::unexpected(GrantError::InvalidState);
    }
    if (node.refs == std::numeric_limits<usize>::max()) {
        return std::unexpected(GrantError::QuotaExceeded);
    }
    ++node.refs;
    return (GrantRef{
        *this, node.slot,
        node.slot->generation.load<libk::MemoryOrder::Relaxed>()});
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
    if (slot.generation.load<libk::MemoryOrder::Acquire>() != generation
        || !slot.occupied.load<libk::MemoryOrder::Acquire>()
        || slot.state.load<libk::MemoryOrder::Acquire>()
            != GrantState::Live) {
        return std::unexpected(GrantError::InvalidState);
    }

    usize operations = slot.operations.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if (admission_closed(operations)) {
            return std::unexpected(GrantError::InvalidState);
        }
        if (operation_count(operations) == operation_closed - 1) {
            return std::unexpected(GrantError::QuotaExceeded);
        }
        if (slot.operations.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Relaxed>(operations, operations + 1)) {
            break;
        }
    }

    if (slot.generation.load<libk::MemoryOrder::Acquire>() != generation
        || !slot.occupied.load<libk::MemoryOrder::Acquire>()
        || slot.state.load<libk::MemoryOrder::Acquire>()
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
            attachment.state_.load<libk::MemoryOrder::Relaxed>())
            != GrantAttachment::State::Idle) {
        return std::unexpected(GrantError::InvalidState);
    }
    auto* const node = static_cast<Node*>(source.h_.get().node);
    sync::Lock guard{lock_};
    if (node->slot->generation.load<libk::MemoryOrder::Acquire>()
            != source.h_.get().gen
        || node->slot->state.load<libk::MemoryOrder::Acquire>()
            != GrantState::Live
        || admission_closed(node->slot->operations.load<
            libk::MemoryOrder::Acquire>())) {
        return std::unexpected(GrantError::InvalidState);
    }
    attachment.graph_ = this;
    attachment.node_ = node;
    attachment.generation_ = source.h_.get().gen;
    attachment.state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(GrantAttachment::State::Attached));
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
    libk_assert(node.slot->generation.load<libk::MemoryOrder::Acquire>()
        == generation);
    release_operation(*node.slot);
}

void Graph::release_operation(Slot& slot) noexcept {
    usize operations = slot.operations.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        libk_assert(operation_count(operations) != 0);
        if (admission_closed(operations)) {
            break;
        }
        if (slot.operations.compare_exchange_weak<
                libk::MemoryOrder::Release,
                libk::MemoryOrder::Relaxed>(operations, operations - 1)) {
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
        libk_assert(slot.occupied.load<libk::MemoryOrder::Acquire>());
        const usize previous =
            slot.operations.fetch_sub<libk::MemoryOrder::AcqRel>(1);
        libk_assert(admission_closed(previous)
            && operation_count(previous) != 0);
        if (operation_count(previous) == 1
            && !slot.work_retained.exchange<libk::MemoryOrder::AcqRel>(true)) {
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
    libk_assert(slot.work_retained.load<libk::MemoryOrder::Acquire>());
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
        if (!slot.occupied.load<libk::MemoryOrder::Acquire>()) {
            slot.work_retained.store<libk::MemoryOrder::Release>(false);
            return;
        }
        Node& node = *slot.node();
        const GrantState state =
            slot.state.load<libk::MemoryOrder::Acquire>();
        const usize operations =
            slot.operations.load<libk::MemoryOrder::Acquire>();
        if (state == GrantState::Live && admission_closed(operations)) {
            libk_assert(operation_count(operations) == 0
                && node.refs == 0 && node.attachments.empty()
                && node.children.empty() && !node.allocation);
        } else if (state == GrantState::Revoking
            && operation_count(operations) == 0
            && node.attachments.empty()) {
            slot.state.store<libk::MemoryOrder::Release>(
                GrantState::Revoked);
            completed = std::exchange(node.revoke, nullptr);
            libk_assert(completed != nullptr);
            target_ref = std::move(node.target);
        } else if (state == GrantState::Revoking) {
            for (GrantAttachment& candidate : node.attachments) {
                if (static_cast<GrantAttachment::State>(
                        candidate.state_.load<libk::MemoryOrder::Relaxed>())
                    != GrantAttachment::State::Attached) {
                    continue;
                }
                libk_assert(candidate.work_.load<libk::MemoryOrder::Relaxed>()
                    != std::numeric_limits<usize>::max());
                static_cast<void>(candidate.work_.fetch_add<
                    libk::MemoryOrder::Relaxed>(1));
                candidate.state_.store<libk::MemoryOrder::Release>(
                    static_cast<u8>(GrantAttachment::State::Invalidating));
                attachment = &candidate;
                work = GrantWork{candidate};
                break;
            }
        }

        if (attachment == nullptr) {
            slot.work_retained.store<libk::MemoryOrder::Release>(false);
            reclaimable = node.refs == 0
                && operation_count(operations) == 0
                && node.attachments.empty()
                && node.children.empty()
                && !node.allocation;
        }
    }

    target_ref.reset();
    if (completed != nullptr) {
        completed->acknowledge();
        retry_allocations();
    }
    if (attachment != nullptr) {
        attachment->ops_->invalidate(
            attachment->context_,
            std::move(work));
        enqueue(slot);
    } else if (reclaimable) {
        reclaim(GrantKey{
            reinterpret_cast<usize>(&slot),
            slot.generation.load<libk::MemoryOrder::Relaxed>()}, false);
    }
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
        libk_assert(node.slot->generation.load<libk::MemoryOrder::Acquire>()
            == attachment.generation_);
        const u64 node_generation = attachment.generation_;
        const auto current = static_cast<GrantAttachment::State>(
            attachment.state_.load<libk::MemoryOrder::Relaxed>());
        libk_assert(current == GrantAttachment::State::Attached
            || current == GrantAttachment::State::Invalidating);
        node.attachments.erase(attachment);
        attachment.graph_ = nullptr;
        attachment.node_ = nullptr;
        attachment.generation_ = 0;
        attachment.state_.store<libk::MemoryOrder::SeqCst>(
            static_cast<u8>(GrantAttachment::State::Draining));
        u8 draining = static_cast<u8>(GrantAttachment::State::Draining);
        quiescent = attachment.work_.load<libk::MemoryOrder::SeqCst>() == 0
            && attachment.state_.compare_exchange_strong<
                libk::MemoryOrder::SeqCst, libk::MemoryOrder::SeqCst>(
                    draining, static_cast<u8>(GrantAttachment::State::Detached));

        if (node.slot->state.load<libk::MemoryOrder::Acquire>()
                == GrantState::Revoking
            && operation_count(node.slot->operations.load<
                    libk::MemoryOrder::Acquire>()) == 0
            && node.attachments.empty()) {
            node.slot->state.store<libk::MemoryOrder::Release>(
                GrantState::Revoked);
            completion = std::exchange(node.revoke, nullptr);
            libk_assert(completion != nullptr);
            target = std::move(node.target);
        }
        if (node.refs == 0
            && operation_count(node.slot->operations.load<
                    libk::MemoryOrder::Acquire>()) == 0
            && node.attachments.empty()
            && node.children.empty()
            && !node.allocation
            && !node.slot->work_retained.load<libk::MemoryOrder::Acquire>()) {
            reclaimable = &node;
            reclaim_generation = node_generation;
        }
    }
    if (completion != nullptr) {
        target.reset();
    }
    if (reclaimable != nullptr) {
        reclaim(GrantKey{
            reinterpret_cast<usize>(
                static_cast<Node*>(reclaimable)->slot),
            reclaim_generation}, false);
    }
    if (completion != nullptr) {
        completion->acknowledge();
        retry_allocations();
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
            if (node.refs != 0
                || !node.attachments.empty()
                || !node.children.empty()
                || node.allocation
                || node.slot->work_retained.load<libk::MemoryOrder::Acquire>()) {
                return;
            }
            const usize operations = node.slot->operations.fetch_or<
                libk::MemoryOrder::AcqRel>(operation_closed);
            if (operation_count(operations) != 0) {
                return;
            }
            parent = node.parent;
            if (parent != nullptr) {
                parent_key = key_of(*parent);
                parent->children.erase(node);
            }
            target = std::move(node.target);
            slot = node.slot;
        }

        target.reset();
        refund = slot->node()->sponsorship.detach();

        {
            sync::Lock guard{lock_};
            libk::destroy_at(slot->node());
            libk_assert(slot->occupied.load<libk::MemoryOrder::Acquire>());
            slot->state.store<libk::MemoryOrder::Release>(
                GrantState::Revoked);
            slot->occupied.store<libk::MemoryOrder::Release>(false);
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
              node->slot->state.load<libk::MemoryOrder::Acquire>())
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
        if (source->slot->state.load<libk::MemoryOrder::Acquire>()
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
            const auto state = node->slot->state.load<libk::MemoryOrder::Acquire>();
            if (state == GrantState::Revoking) return std::unexpected(GrantError::RevocationConflict);
            if (state != GrantState::Revoked) ++pending;
        }

        // Preflight and commit share the topology lock. Every selected node
        // contributes once, after its operations and attachments have drained.
        completion.initialize(pending + 1);
        for (Node* node = first; node; node = next(*source, *node)) {
            if (node->slot->state.load<libk::MemoryOrder::Acquire>() == GrantState::Revoked) continue;
            node->revoke = &completion;
            node->refs = 0;
            node->slot->work_retained.store<libk::MemoryOrder::Release>(true);
            static_cast<void>(node->slot->operations.fetch_or<libk::MemoryOrder::AcqRel>(operation_closed));
            node->slot->state.store<libk::MemoryOrder::Release>(GrantState::Revoking);
            enqueue(*node->slot);
        }
    }

    kick_work();
    completion.acknowledge();
    return {};
}

auto Graph::destroy_target(const GrantLease& source) noexcept -> std::expected<void, GrantError> {
    object::allocation* allocation{};
    {
        sync::Lock guard{lock_};
        Node* const node = locate(source.key());
        if (node == nullptr) return std::unexpected(GrantError::InvalidKey);
        for (Node* ancestor = node; ancestor != nullptr; ancestor = ancestor->parent) {
            if (ancestor->allocation) {
                libk_assert(ancestor->target.id() == node->target.id());
                allocation = ancestor->allocation;
                break;
            }
        }
    }
    if (allocation != nullptr) {
        // The source operation is part of the root's revoke barrier: neither
        // this record nor its sponsoring pool can detach until it is released.
        allocation->owner_->close_allocation(*allocation);
        return {};
    }
    auto target = source.clone_target();
    return target && target.value().retire() ? std::expected<void, GrantError>{}
        : std::expected<void, GrantError>{std::unexpected(GrantError::InvalidState)};
}

void Graph::revoke_allocation(
    object::allocation& allocation) noexcept {
    libk_assert(allocation.graph_ == this && allocation.root_.valid());
    for (;;) {
        const auto revoked = invalidate(allocation.root_, allocation.revoke_);
        if (revoked) break;
        libk_assert(revoked.error() == GrantError::RevocationConflict);
        {
            sync::Lock guard{lock_};
            Node* const root = locate(allocation.root_);
            libk_assert(root != nullptr);
            bool conflict{};
            for (Node* node = root; node; node = next(*root, *node)) {
                if (node->slot->state.load<libk::MemoryOrder::Acquire>() == GrantState::Revoking) {
                    conflict = true;
                    break;
                }
            }
            if (conflict) {
                // The allocation stays in Revoking and its pool keeps it
                // alive. The completion of the conflicting descendant will
                // retry this root after releasing the graph lock.
                allocation.revoke_retry_next_ = revoke_retry_;
                revoke_retry_ = &allocation;
                return;
            }
        }
        // The conflicting lineage completed before we could register. No
        // completion can wake us now, so retry against the current graph.
    }
    if (allocation.revoke_.complete() || !allocation.revoke_.arm()) {
        allocation.ready();
    }
}

void Graph::retry_allocations() noexcept {
    object::allocation* pending{};
    {
        sync::Lock guard{lock_};
        pending = std::exchange(revoke_retry_, nullptr);
    }
    while (pending != nullptr) {
        auto* const next = pending->revoke_retry_next_;
        pending->revoke_retry_next_ = nullptr;
        revoke_allocation(*pending);
        pending = next;
    }
}

void Graph::bind_allocation(
    GrantKey root, object::allocation& allocation) noexcept {
    sync::Lock guard{lock_};
    Node* const node = find(root);
    libk_assert(node != nullptr && node->parent == nullptr && !node->allocation);
    libk_assert(node->target.id() == allocation.target_.id());
    node->allocation = &allocation;
}

void Graph::release_allocation(
    GrantKey root, const object::allocation* allocation) noexcept {
    {
        sync::Lock guard{lock_};
        Node* const node = locate(root);
        libk_assert(node != nullptr && node->allocation == allocation);
        node->allocation = nullptr;
    }
    reclaim(root, false);
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
        libk_assert(!slot.occupied.load<libk::MemoryOrder::Acquire>());
    }
    auto backing = Storage::dispose(page);
    backing.reset();
}

} // namespace cap
