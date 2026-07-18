#include <cap/grant_graph.hpp>

#include <cap/policy.hpp>
#include <core/debug.hpp>
#include <libk/limits.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
#include <object/resource_pool.hpp>
#include <object/sched_pool.hpp>
#include <object/thread_pool.hpp>
#include <object/vproc_pool.hpp>
#include <resource/pool.hpp>
#include <thread/thread.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::cap {

GrantWork::GrantWork(GrantWork&& other) noexcept
    : attachment_(libk::exchange(other.attachment_, nullptr)) {}

auto GrantWork::operator=(GrantWork&& other) noexcept -> GrantWork& {
    if (this != &other) {
        reset();
        attachment_ = libk::exchange(other.attachment_, nullptr);
    }
    return *this;
}

GrantWork::~GrantWork() noexcept {
    reset();
}

void GrantWork::reset() noexcept {
    GrantAttachment* const attachment =
        libk::exchange(attachment_, nullptr);
    if (attachment != nullptr) {
        attachment->drop_work();
    }
}

GrantAttachment::~GrantAttachment() noexcept {
    const State current = static_cast<State>(
        state_.load<libk::MemoryOrder::Acquire>());
    KASSERT(current == State::Idle || current == State::Detached);
    KASSERT(graph_ == nullptr && node_ == nullptr);
    KASSERT(work_.load<libk::MemoryOrder::Acquire>() == 0);
}

auto GrantAttachment::attached() const noexcept -> bool {
    const State current = static_cast<State>(
        state_.load<libk::MemoryOrder::Acquire>());
    return current == State::Attached || current == State::Invalidating;
}

auto GrantAttachment::busy() const noexcept -> bool {
    return work_.load<libk::MemoryOrder::Acquire>() != 0;
}

auto GrantAttachment::detach() noexcept -> bool {
    GrantGraph* const graph = graph_;
    if (graph == nullptr) {
        return static_cast<State>(
            state_.load<libk::MemoryOrder::Acquire>()) == State::Detached
            && !busy();
    }
    return graph->detach(*this);
}

void GrantAttachment::drop_work() noexcept {
    const usize previous = work_.fetch_sub<libk::MemoryOrder::SeqCst>(1);
    KASSERT(previous != 0);
    if (previous == 1
        && static_cast<State>(state_.load<libk::MemoryOrder::SeqCst>())
            == State::Detached) {
        KASSERT(ops_ != nullptr && ops_->released != nullptr);
        ops_->released(context_);
    }
}

GrantGraph::GrantGraph(kernel::mm::Pmm& pmm) noexcept
    : GrantGraph(pmm, Quota{}) {}

GrantGraph::GrantGraph(kernel::mm::Pmm& pmm, Quota quota) noexcept
    : pmm_(&pmm),
      quota_(quota),
      revoke_waits_(pmm),
      close_waits_(pmm) {}

GrantRevokeWait::GrantRevokeWait(GrantGraph& graph) noexcept
    : graph_(&graph),
      completion_(kernel::sync::Completion::Notifier::bind<
          &GrantRevokeWait::ready>(*this)),
      relation_(kernel::operation::Completion::bind<
          GrantRevokeWait,
          &GrantRevokeWait::complete,
          &GrantRevokeWait::read,
          &GrantRevokeWait::release,
          &GrantRevokeWait::cancel>(*this)) {}

void GrantRevokeWait::ready() noexcept {
    KASSERT(completion_.complete());
    relation_.signal();
}

auto GrantRevokeWait::read() noexcept -> kernel::operation::Result {
    KASSERT(completion_.complete());
    return {};
}

void GrantRevokeWait::release() noexcept {
    GrantGraph* const graph = graph_;
    KASSERT(graph != nullptr);
    graph->destroy_revoke_wait(*this);
}

auto GrantRevokeWait::cancel() noexcept -> bool {
    return !completion_.initialized();
}

auto GrantGraph::create_revoke_wait() noexcept
    -> libk::Expected<GrantRevokeWait*, GrantError> {
    auto made = revoke_waits_.create(*this);
    if (!made) {
        switch (made.error()) {
        case kernel::mm::NodePoolError::OutOfMemory:
            return libk::unexpected(GrantError::OutOfMemory);
        case kernel::mm::NodePoolError::QuotaExceeded:
            return libk::unexpected(GrantError::QuotaExceeded);
        case kernel::mm::NodePoolError::GenerationExhausted:
            return libk::unexpected(GrantError::GenerationExhausted);
        case kernel::mm::NodePoolError::ResourceExhausted:
            return libk::unexpected(GrantError::OutOfMemory);
        }
    }
    return libk::expected(made.value().object);
}

void GrantGraph::destroy_revoke_wait(GrantRevokeWait& operation) noexcept {
    revoke_waits_.destroy(operation);
}

auto GrantGraph::create_close_wait() noexcept
    -> libk::Expected<kernel::resource::CloseWait*, GrantError> {
    auto made = close_waits_.create(*this);
    if (!made) {
        switch (made.error()) {
        case kernel::mm::NodePoolError::OutOfMemory:
            return libk::unexpected(GrantError::OutOfMemory);
        case kernel::mm::NodePoolError::QuotaExceeded:
            return libk::unexpected(GrantError::QuotaExceeded);
        case kernel::mm::NodePoolError::GenerationExhausted:
            return libk::unexpected(GrantError::GenerationExhausted);
        case kernel::mm::NodePoolError::ResourceExhausted:
            return libk::unexpected(GrantError::OutOfMemory);
        }
    }
    return libk::expected(made.value().object);
}

void GrantGraph::destroy_close_wait(
    kernel::resource::CloseWait& operation) noexcept {
    close_waits_.destroy(operation);
}

auto GrantGraph::close_pool(
    kernel::resource::ResourcePool& pool,
    const kernel::object::ObjectRef& self,
    kernel::Thread& thread,
    kernel::CpuRegistry& cpus) noexcept
    -> libk::Expected<kernel::operation::State, GrantError> {
    auto made = create_close_wait();
    if (!made) {
        return libk::unexpected(made.error());
    }
    kernel::resource::CloseWait* const operation = made.value();
    if (!thread.begin_wait(operation->relation(), cpus)) {
        KASSERT(operation->cancel());
        destroy_close_wait(*operation);
        return libk::unexpected(GrantError::InvalidState);
    }
    if (!pool.observe_refund(self, operation->notifier())) {
        thread.cancel_wait();
        return libk::unexpected(GrantError::InvalidState);
    }
    operation->commit();
    static_cast<void>(pool.close());
    return libk::expected(operation->arm()
        ? kernel::operation::State::Waiting
        : kernel::operation::State::Complete);
}

GrantLease::GrantLease(GrantLease&& other) noexcept
    : graph_(libk::exchange(other.graph_, nullptr)),
      node_(libk::exchange(other.node_, nullptr)),
      generation_(libk::exchange(other.generation_, u64{})) {}

auto GrantLease::operator=(GrantLease&& other) noexcept -> GrantLease& {
    if (this != &other) {
        reset();
        graph_ = libk::exchange(other.graph_, nullptr);
        node_ = libk::exchange(other.node_, nullptr);
        generation_ = libk::exchange(other.generation_, u64{});
    }
    return *this;
}

GrantLease::~GrantLease() noexcept {
    reset();
}

auto GrantLease::key() const noexcept -> GrantKey {
    KASSERT(graph_ != nullptr);
    const auto& node = *static_cast<const GrantGraph::Node*>(node_);
    KASSERT(node.slot->generation == generation_);
    return GrantGraph::key_of(node);
}

auto GrantLease::kind() const noexcept -> object::ObjectKind {
    KASSERT(graph_ != nullptr);
    return static_cast<const GrantGraph::Node*>(node_)->target.kind();
}

auto GrantLease::ceiling() const noexcept -> GrantCeiling {
    KASSERT(graph_ != nullptr);
    return static_cast<const GrantGraph::Node*>(node_)->ceiling;
}

auto GrantLease::clone_target() const noexcept
    -> libk::Expected<object::ObjectRef, object::ObjectError> {
    KASSERT(graph_ != nullptr);
    return static_cast<const GrantGraph::Node*>(node_)->target.clone();
}

auto GrantLease::attach(GrantAttachment& attachment) const noexcept
    -> libk::Expected<void, GrantError> {
    if (graph_ == nullptr) {
        return libk::unexpected(GrantError::InvalidKey);
    }
    return graph_->attach(*this, attachment);
}

auto GrantLease::derive_region(
    kernel::resource::Reservation&& charge,
    object::ObjectRef&& target,
    GrantCeiling ceiling,
    RegionDerivation proof) const noexcept
    -> libk::Expected<GrantRef, GrantError> {
    if (graph_ == nullptr) {
        return libk::unexpected(GrantError::InvalidKey);
    }
    return graph_->derive_region(
        libk::move(charge), *this, libk::move(target), ceiling, proof);
}

void GrantLease::reset() noexcept {
    GrantGraph* const graph = libk::exchange(graph_, nullptr);
    void* const node = libk::exchange(node_, nullptr);
    const u64 generation = libk::exchange(generation_, u64{});
    if (graph != nullptr) {
        graph->drop_lease(node, generation);
    }
}

GrantRef::GrantRef(GrantRef&& other) noexcept
    : graph_(libk::exchange(other.graph_, nullptr)),
      key_(libk::exchange(other.key_, GrantKey{})) {}

auto GrantRef::operator=(GrantRef&& other) noexcept -> GrantRef& {
    if (this != &other) {
        reset();
        graph_ = libk::exchange(other.graph_, nullptr);
        key_ = libk::exchange(other.key_, GrantKey{});
    }
    return *this;
}

GrantRef::~GrantRef() noexcept {
    reset();
}

auto GrantRef::key() const noexcept -> GrantKey {
    KASSERT(graph_ != nullptr);
    KASSERT(key_.valid());
    return key_;
}

auto GrantRef::graph() const noexcept -> GrantGraph& {
    KASSERT(graph_ != nullptr);
    return *graph_;
}

auto GrantRef::clone() const noexcept
    -> libk::Expected<GrantRef, GrantError> {
    if (graph_ == nullptr) {
        return libk::unexpected(GrantError::InvalidKey);
    }
    return graph_->ref(key());
}

auto GrantRef::acquire() const noexcept
    -> libk::Expected<GrantLease, GrantError> {
    if (graph_ == nullptr) {
        return libk::unexpected(GrantError::InvalidKey);
    }
    return graph_->acquire(key());
}

void GrantRef::reset() noexcept {
    GrantGraph* const graph = libk::exchange(graph_, nullptr);
    const GrantKey key = libk::exchange(key_, GrantKey{});
    if (graph != nullptr) {
        graph->drop_ref(key);
    }
}

void GrantRevoke::initialize(usize pending) noexcept {
    completion_.initialize(pending);
}

void GrantRevoke::acknowledge() noexcept {
    completion_.acknowledge();
}

GrantGraph::~GrantGraph() noexcept {
    KASSERT(close_waits_.live_count() == 0);
    KASSERT(revoke_waits_.live_count() == 0);
    KASSERT(live_nodes_ == 0);
    while (pages_ != nullptr) {
        PageHeader* const page = pages_;
        pages_ = page->next;
        release_page(*page);
    }
    KASSERT(page_count_ == 0);
}

auto GrantGraph::create_root(
    object::ObjectRef&& target,
    GrantCeiling ceiling) noexcept -> libk::Expected<GrantRef, GrantError> {
    return create_root({}, libk::move(target), ceiling);
}

auto GrantGraph::create_root(
    kernel::resource::Reservation&& charge,
    object::ObjectRef&& target,
    GrantCeiling ceiling) noexcept -> libk::Expected<GrantRef, GrantError> {
    if (!target || !validate_ceiling(target.kind(), ceiling)) {
        return libk::unexpected(GrantError::RightsViolation);
    }
    return create(
        libk::move(charge), libk::move(target), ceiling, nullptr);
}

auto GrantGraph::create_allocation(
    kernel::resource::Permit& permit,
    kernel::resource::Reservation&& charge,
    object::ObjectRef&& target,
    GrantCeiling ceiling) noexcept
    -> libk::Expected<kernel::resource::AllocationTxn, GrantError> {
    if (!permit || !target) {
        return libk::unexpected(GrantError::InvalidState);
    }
    auto capability_target = target.clone();
    if (!capability_target) {
        return libk::unexpected(GrantError::InvalidState);
    }
    auto created = create_root(
        libk::move(charge),
        libk::move(capability_target).value(),
        ceiling);
    if (!created) {
        return libk::unexpected(created.error());
    }
    GrantRef root = libk::move(created).value();
    const GrantKey key = root.key();
    kernel::resource::Allocation* allocation{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Node* const node = find(key);
        KASSERT(node != nullptr && !node->allocation);
        allocation = &node->allocation;
        allocation->graph_ = this;
        allocation->root_ = key;
        allocation->target_ = libk::move(target);
        allocation->state_ = kernel::resource::AllocationState::Pending;
    }
    KASSERT(permit.pool_ != nullptr);
    permit.pool_->attach(permit, *allocation);
    return libk::expected(kernel::resource::AllocationTxn{
        *this, *allocation, libk::move(root)});
}

auto GrantGraph::derive(
    const GrantLease& source,
    object::ObjectRef&& target,
    GrantCeiling ceiling) noexcept -> libk::Expected<GrantRef, GrantError> {
    return derive({}, source, libk::move(target), ceiling);
}

auto GrantGraph::derive(
    kernel::resource::Reservation&& charge,
    const GrantLease& source,
    object::ObjectRef&& target,
    GrantCeiling ceiling) noexcept -> libk::Expected<GrantRef, GrantError> {
    if (source.graph_ != this || !target) {
        return libk::unexpected(GrantError::InvalidKey);
    }
    auto* const parent = static_cast<Node*>(source.node_);
    if (target.kind() != parent->target.kind()) {
        return libk::unexpected(GrantError::WrongKind);
    }
    if (!validate_ceiling(target.kind(), ceiling)
        || !attenuates(
            target.kind(),
            EffectiveAuthority{
                parent->ceiling.rights, parent->ceiling.data},
            ceiling)) {
        return libk::unexpected(GrantError::RightsViolation);
    }
    return create(
        libk::move(charge), libk::move(target), ceiling, parent);
}

auto GrantGraph::derive_region(
    kernel::resource::Reservation&& charge,
    const GrantLease& source,
    object::ObjectRef&& target,
    GrantCeiling ceiling,
    RegionDerivation proof) noexcept
    -> libk::Expected<GrantRef, GrantError> {
    if (source.graph_ != this || !target
        || target.kind() != object::ObjectKind::VSpace) {
        return libk::unexpected(GrantError::InvalidKey);
    }
    auto* const parent = static_cast<Node*>(source.node_);
    if (target.kind() != parent->target.kind()
        || !validate_ceiling(target.kind(), ceiling)) {
        return libk::unexpected(GrantError::RightsViolation);
    }
    const auto* const parent_data =
        libk::get_if<VSpaceAuthority>(&parent->ceiling.data);
    const auto* const child_data =
        libk::get_if<VSpaceAuthority>(&ceiling.data);
    if (parent_data == nullptr || child_data == nullptr
        || parent_data->region != proof.parent_
        || child_data->region != proof.child_
        || child_data->range != proof.range_
        || !parent_data->range.contains(proof.range_)
        || !parent_data->access.contains(child_data->access)
        || !parent_data->types.contains(child_data->types)
        || !parent->ceiling.rights.contains(ceiling.rights)) {
        return libk::unexpected(GrantError::RightsViolation);
    }
    return create(
        libk::move(charge), libk::move(target), ceiling, parent);
}

auto GrantGraph::create(
    kernel::resource::Reservation&& charge,
    object::ObjectRef&& target,
    GrantCeiling ceiling,
    Node* parent) noexcept -> libk::Expected<GrantRef, GrantError> {
    auto claimed = claim_slot();
    if (!claimed) {
        return libk::unexpected(claimed.error());
    }
    Slot* const slot = claimed.value();
    Node* const node = libk::construct_at(
        slot->node(),
        *slot,
        libk::move(target),
        ceiling,
        parent,
        libk::move(charge));

    object::ObjectRef returned{};
    bool rejected{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (parent != nullptr && (parent->reclaiming
            || parent->state != GrantState::Live)) {
            rejected = true;
        } else {
            node->refs = 1;
            if (parent != nullptr) {
                parent->children.push_back(*node);
            }
        }
    }
    if (rejected) {
        returned = libk::move(node->target);
        auto refund = node->sponsorship.detach();
        libk::destroy_at(node);
        {
            kernel::sync::IrqLockGuard guard{lock_};
            slot->occupied = false;
            slot->next_free = slot->page->free_head;
            slot->page->free_head = slot;
            ++slot->page->free_count;
            --slot->page->live_count;
            --live_nodes_;
        }
        returned.reset();
        refund.complete();
        return libk::unexpected(GrantError::InvalidState);
    }
    return libk::expected(GrantRef{*this, key_of(*node)});
}

auto GrantGraph::claim_slot() noexcept
    -> libk::Expected<Slot*, GrantError> {
    for (;;) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (live_nodes_ >= quota_.nodes) {
                return libk::unexpected(GrantError::QuotaExceeded);
            }
            for (PageHeader* page = pages_; page != nullptr; page = page->next) {
                while (page->free_head != nullptr) {
                    Slot* const slot = page->free_head;
                    page->free_head = slot->next_free;
                    slot->next_free = nullptr;
                    --page->free_count;
                    if (slot->generation == libk::numeric_limits<u64>::max()) {
                        slot->quarantined = true;
                        ++quarantined_slots_;
                        continue;
                    }
                    ++slot->generation;
                    slot->occupied = true;
                    ++page->live_count;
                    ++live_nodes_;
                    return libk::expected(slot);
                }
            }
            if (page_count_ >= quota_.pages) {
                return libk::unexpected(
                    quarantined_slots_ == page_count_ * slots_per_page
                        ? GrantError::GenerationExhausted
                        : GrantError::QuotaExceeded);
            }
            ++page_count_;
        }

        auto made = make_page();
        if (!made) {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(page_count_ != 0);
            --page_count_;
            return libk::unexpected(made.error());
        }
        PageHeader* const page = made.value();
        kernel::sync::IrqLockGuard guard{lock_};
        page->next = pages_;
        pages_ = page;
    }
}

auto GrantGraph::make_page() noexcept
    -> libk::Expected<PageHeader*, GrantError> {
    auto allocation = pmm_->allocate_page();
    if (!allocation) {
        return libk::unexpected(GrantError::OutOfMemory);
    }
    kernel::mm::OwnedPage backing = libk::move(allocation).value();
    auto* const page = libk::construct_at(
        reinterpret_cast<PageHeader*>(backing.bytes()),
        libk::move(backing));
    auto* const slots = reinterpret_cast<Slot*>(
        reinterpret_cast<usize>(page) + slot_offset);
    for (usize index = slots_per_page; index > 0; --index) {
        Slot* const slot = libk::construct_at(&slots[index - 1]);
        slot->page = page;
        slot->next_free = page->free_head;
        page->free_head = slot;
        ++page->free_count;
    }
    return libk::expected(page);
}

auto GrantGraph::find(GrantKey key) noexcept -> Node* {
    return const_cast<Node*>(
        static_cast<const GrantGraph*>(this)->find(key));
}

auto GrantGraph::find(GrantKey key) const noexcept -> const Node* {
    if (!key.valid()) {
        return nullptr;
    }
    for (const PageHeader* page = pages_; page != nullptr; page = page->next) {
        const auto* const first = reinterpret_cast<const Slot*>(
            reinterpret_cast<usize>(page) + slot_offset);
        const usize first_address = reinterpret_cast<usize>(first);
        const usize bytes = slots_per_page * sizeof(Slot);
        if (key.slot < first_address || key.slot - first_address >= bytes) {
            continue;
        }
        const usize displacement = key.slot - first_address;
        if (displacement % sizeof(Slot) != 0) {
            return nullptr;
        }
        const Slot& slot = first[displacement / sizeof(Slot)];
        return slot.occupied
                && slot.generation == key.generation
                && !slot.node()->reclaiming
            ? slot.node()
            : nullptr;
    }
    return nullptr;
}

auto GrantGraph::key_of(const Node& node) noexcept -> GrantKey {
    return GrantKey{
        reinterpret_cast<usize>(node.slot),
        node.slot->generation,
    };
}

auto GrantGraph::ref(GrantKey key) noexcept
    -> libk::Expected<GrantRef, GrantError> {
    kernel::sync::IrqLockGuard guard{lock_};
    Node* const node = find(key);
    return node != nullptr
        ? try_ref(*node)
        : libk::Expected<GrantRef, GrantError>{
              libk::unexpected(GrantError::InvalidKey)};
}

auto GrantGraph::try_ref(Node& node) noexcept
    -> libk::Expected<GrantRef, GrantError> {
    KASSERT(!node.reclaiming);
    if (node.state != GrantState::Live) {
        return libk::unexpected(GrantError::InvalidState);
    }
    if (node.refs == libk::numeric_limits<usize>::max()) {
        return libk::unexpected(GrantError::QuotaExceeded);
    }
    ++node.refs;
    return libk::expected(GrantRef{
        *this, key_of(node)});
}

auto GrantGraph::acquire(GrantKey key) noexcept
    -> libk::Expected<GrantLease, GrantError> {
    kernel::sync::IrqLockGuard guard{lock_};
    Node* const node = find(key);
    return node != nullptr
        ? try_acquire(*node)
        : libk::Expected<GrantLease, GrantError>{
              libk::unexpected(GrantError::InvalidKey)};
}

auto GrantGraph::try_acquire(Node& node) noexcept
    -> libk::Expected<GrantLease, GrantError> {
    if (node.state != GrantState::Live) {
        return libk::unexpected(GrantError::InvalidState);
    }
    if (node.operations == libk::numeric_limits<usize>::max()) {
        return libk::unexpected(GrantError::QuotaExceeded);
    }
    ++node.operations;
    return libk::expected(GrantLease{
        *this, &node, node.slot->generation});
}

auto GrantGraph::attach(
    const GrantLease& source,
    GrantAttachment& attachment) noexcept
    -> libk::Expected<void, GrantError> {
    if (source.graph_ != this
        || attachment.graph_ != nullptr
        || attachment.node_ != nullptr
        || attachment.ops_ == nullptr
        || attachment.ops_->invalidate == nullptr
        || attachment.ops_->released == nullptr
        || static_cast<GrantAttachment::State>(
            attachment.state_.load<libk::MemoryOrder::Relaxed>())
            != GrantAttachment::State::Idle) {
        return libk::unexpected(GrantError::InvalidState);
    }
    auto* const node = static_cast<Node*>(source.node_);
    kernel::sync::IrqLockGuard guard{lock_};
    if (node->slot->generation != source.generation_
        || node->state != GrantState::Live
        || node->reclaiming) {
        return libk::unexpected(GrantError::InvalidState);
    }
    attachment.graph_ = this;
    attachment.node_ = node;
    attachment.generation_ = source.generation_;
    attachment.state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(GrantAttachment::State::Attached));
    node->attachments.push_back(attachment);
    return libk::expected();
}

void GrantGraph::drop_ref(GrantKey key) noexcept {
    KASSERT(key.valid());
    reclaim(key, true);
}

void GrantGraph::drop_lease(void* raw, u64 generation) noexcept {
    KASSERT(raw != nullptr);
    auto& node = *static_cast<Node*>(raw);
    object::ObjectRef target{};
    GrantRevoke* completion{};
    bool reclaimable{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(node.slot->generation == generation);
        KASSERT(node.operations != 0);
        --node.operations;
        if (node.state == GrantState::Revoking
            && node.operations == 0
            && node.attachments.empty()) {
            node.state = GrantState::Revoked;
            completion = libk::exchange(node.revoke, nullptr);
            KASSERT(completion != nullptr);
            target = libk::move(node.target);
        }
        reclaimable = node.refs == 0
            && node.operations == 0
            && node.attachments.empty()
            && node.children.empty()
            && !node.allocation;
    }
    if (completion != nullptr) {
        target.reset();
    }
    if (reclaimable) {
        reclaim(key_of(node), false);
    }
    if (completion != nullptr) {
        completion->acknowledge();
    }
}

auto GrantGraph::detach(GrantAttachment& attachment) noexcept -> bool {
    object::ObjectRef target{};
    GrantRevoke* completion{};
    void* reclaimable{};
    u64 reclaim_generation{};
    bool quiescent{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (attachment.graph_ != this || attachment.node_ == nullptr) {
            return false;
        }
        auto& node = *static_cast<Node*>(attachment.node_);
        KASSERT(node.slot->generation == attachment.generation_);
        const u64 node_generation = attachment.generation_;
        const auto current = static_cast<GrantAttachment::State>(
            attachment.state_.load<libk::MemoryOrder::Relaxed>());
        KASSERT(current == GrantAttachment::State::Attached
            || current == GrantAttachment::State::Invalidating);
        node.attachments.erase(attachment);
        attachment.graph_ = nullptr;
        attachment.node_ = nullptr;
        attachment.generation_ = 0;
        attachment.state_.store<libk::MemoryOrder::SeqCst>(
            static_cast<u8>(GrantAttachment::State::Detached));
        quiescent = attachment.work_.load<libk::MemoryOrder::SeqCst>() == 0;

        if (node.state == GrantState::Revoking
            && node.operations == 0
            && node.attachments.empty()) {
            node.state = GrantState::Revoked;
            completion = libk::exchange(node.revoke, nullptr);
            KASSERT(completion != nullptr);
            target = libk::move(node.target);
        }
        if (node.refs == 0
            && node.operations == 0
            && node.attachments.empty()
            && node.children.empty()
            && !node.allocation) {
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
    }
    return quiescent;
}

void GrantGraph::reclaim(
    GrantKey initial,
    bool drop_reference) noexcept {
    GrantKey current = initial;
    bool drop = drop_reference;
    while (current.valid()) {
        object::ObjectRef target{};
        kernel::resource::Refund refund{};
        Node* parent{};
        GrantKey parent_key{};
        Slot* slot{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            Node* const found = find(current);
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
                || node.operations != 0
                || !node.attachments.empty()
                || !node.children.empty()
                || node.allocation
                || node.reclaiming) {
                return;
            }
            node.reclaiming = true;
            parent = node.parent;
            if (parent != nullptr) {
                parent_key = key_of(*parent);
                parent->children.erase(node);
            }
            target = libk::move(node.target);
            slot = node.slot;
        }

        target.reset();
        refund = slot->node()->sponsorship.detach();

        {
            kernel::sync::IrqLockGuard guard{lock_};
            libk::destroy_at(slot->node());
            KASSERT(slot->occupied);
            slot->occupied = false;
            slot->next_free = slot->page->free_head;
            slot->page->free_head = slot;
            ++slot->page->free_count;
            KASSERT(slot->page->live_count != 0);
            --slot->page->live_count;
            KASSERT(live_nodes_ != 0);
            --live_nodes_;
        }
        refund.complete();
        // A non-empty child list itself prevents parent reclamation; it does
        // not need a second synthetic reference count.
        current = parent_key;
        drop = false;
    }
}

auto GrantGraph::state(GrantKey key) const noexcept
    -> libk::Expected<GrantState, GrantError> {
    kernel::sync::IrqLockGuard guard{lock_};
    const Node* const node = find(key);
    return node != nullptr
        ? libk::expected(node->state)
        : libk::Expected<GrantState, GrantError>{
              libk::unexpected(GrantError::InvalidKey)};
}

auto GrantGraph::live_count() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return live_nodes_;
}

auto GrantGraph::node_charge() noexcept -> kernel::resource::Budget {
    return kernel::resource::Budget{.memory = sizeof(Slot)};
}

auto GrantGraph::revoke_descendants(
    GrantKey source,
    GrantRevoke& completion) noexcept
    -> libk::Expected<void, GrantError> {
    return revoke(source, completion, false);
}

auto GrantGraph::invalidate(
    GrantKey source,
    GrantRevoke& completion) noexcept
    -> libk::Expected<void, GrantError> {
    return revoke(source, completion, true);
}

auto GrantGraph::revoke(
    GrantKey source_key,
    GrantRevoke& completion,
    bool include_source) noexcept -> libk::Expected<void, GrantError> {
    Node* source{};
    GrantRef source_hold{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        source = find(source_key);
        if (source == nullptr) {
            return libk::unexpected(GrantError::InvalidKey);
        }
        if (source->state != GrantState::Live || completion.initialized()) {
            return libk::unexpected(GrantError::InvalidState);
        }
        auto held = try_ref(*source);
        if (!held) {
            return libk::unexpected(held.error());
        }
        source_hold = libk::move(held).value();

        usize pending{};
        for (PageHeader* page = pages_; page != nullptr; page = page->next) {
            auto* const slots = reinterpret_cast<Slot*>(
                reinterpret_cast<usize>(page) + slot_offset);
            for (usize index = 0; index < slots_per_page; ++index) {
                if (!slots[index].occupied) {
                    continue;
                }
                Node& node = *slots[index].node();
                const bool selected = (&node == source && include_source)
                    || descendant_of(node, *source);
                if (!selected || node.state == GrantState::Revoked) {
                    continue;
                }
                if (node.state == GrantState::Revoking) {
                    return libk::unexpected(GrantError::RevocationConflict);
                }
                if (node.operations != 0 || !node.attachments.empty()) {
                    ++pending;
                }
            }
        }

        // The extra acknowledgement is a target-release barrier.  Revoked
        // tombstones may remain referenced by old CSpace slots, but completion
        // cannot become visible until their hidden ObjectRef holds are gone.
        KASSERT(pending != libk::numeric_limits<usize>::max());
        completion.initialize(pending + 1);
        for (PageHeader* page = pages_; page != nullptr; page = page->next) {
            auto* const slots = reinterpret_cast<Slot*>(
                reinterpret_cast<usize>(page) + slot_offset);
            for (usize index = 0; index < slots_per_page; ++index) {
                if (!slots[index].occupied) {
                    continue;
                }
                Node& node = *slots[index].node();
                const bool selected = (&node == source && include_source)
                    || descendant_of(node, *source);
                if (!selected || node.state == GrantState::Revoked) {
                    continue;
                }
                // Capability holders become stale identities at the permanent
                // revoke linearization point. They no longer own node storage.
                node.refs = 0;
                if (node.operations == 0 && node.attachments.empty()) {
                    node.state = GrantState::Revoked;
                } else {
                    node.state = GrantState::Revoking;
                    node.revoke = &completion;
                }
            }
        }
    }

    // A permanent revocation invalidates authority independently of tombstone
    // lifetime.  Drop target holds outside the graph lock so ObjectPool
    // retirement/reclaim notification cannot invert the lock order.
    for (;;) {
        object::ObjectRef target{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            for (PageHeader* page = pages_;
                 page != nullptr && !target;
                 page = page->next) {
                auto* const slots = reinterpret_cast<Slot*>(
                    reinterpret_cast<usize>(page) + slot_offset);
                for (usize index = 0; index < slots_per_page; ++index) {
                    if (!slots[index].occupied) {
                        continue;
                    }
                    Node& node = *slots[index].node();
                    const bool selected = (&node == source && include_source)
                        || descendant_of(node, *source);
                    if (!selected || node.state != GrantState::Revoked
                        || !node.target) {
                        continue;
                    }
                    target = libk::move(node.target);
                    break;
                }
            }
        }
        if (!target) {
            break;
        }
        target.reset();
    }

    // Reclaim quiescent revoked leaves before publishing completion. Parent
    // nodes naturally become eligible as their child lists empty.
    for (;;) {
        GrantKey reclaimable{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            for (PageHeader* page = pages_;
                 page != nullptr && !reclaimable.valid();
                 page = page->next) {
                auto* const slots = reinterpret_cast<Slot*>(
                    reinterpret_cast<usize>(page) + slot_offset);
                for (usize index = 0; index < slots_per_page; ++index) {
                    if (!slots[index].occupied) {
                        continue;
                    }
                    Node& node = *slots[index].node();
                    if (node.state != GrantState::Revoked
                        || node.refs != 0 || node.operations != 0
                        || !node.attachments.empty()
                        || !node.children.empty() || node.target
                        || node.allocation
                        || node.reclaiming) {
                        continue;
                    }
                    reclaimable = key_of(node);
                    break;
                }
            }
        }
        if (!reclaimable.valid()) {
            break;
        }
        reclaim(reclaimable, false);
    }
    completion.acknowledge();

    for (;;) {
        GrantAttachment* target{};
        GrantWork work{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            for (PageHeader* page = pages_;
                 page != nullptr && target == nullptr;
                 page = page->next) {
                auto* const slots = reinterpret_cast<Slot*>(
                    reinterpret_cast<usize>(page) + slot_offset);
                for (usize index = 0; index < slots_per_page; ++index) {
                    if (!slots[index].occupied) {
                        continue;
                    }
                    Node& node = *slots[index].node();
                    if (node.state != GrantState::Revoking
                        || node.revoke != &completion) {
                        continue;
                    }
                    for (GrantAttachment& attachment : node.attachments) {
                        if (static_cast<GrantAttachment::State>(
                                attachment.state_.load<
                                    libk::MemoryOrder::Relaxed>())
                            != GrantAttachment::State::Attached) {
                            continue;
                        }
                        KASSERT(attachment.work_.load<
                            libk::MemoryOrder::Relaxed>()
                            != libk::numeric_limits<usize>::max());
                        static_cast<void>(attachment.work_.fetch_add<
                            libk::MemoryOrder::Relaxed>(1));
                        attachment.state_.store<libk::MemoryOrder::Release>(
                            static_cast<u8>(
                                GrantAttachment::State::Invalidating));
                        target = &attachment;
                        work = GrantWork{attachment};
                        break;
                    }
                    if (target != nullptr) {
                        break;
                    }
                }
            }
        }
        if (target == nullptr) {
            break;
        }
        target->ops_->invalidate(
            target->context_, libk::move(work), GrantInvalidation::Revoke);
    }
    return libk::expected();
}

void GrantGraph::commit_allocation(
    kernel::resource::Allocation& allocation) noexcept {
    KASSERT(allocation.graph_ == this && allocation.root_.valid());
    KASSERT(allocation.pool_ != nullptr);
    if (allocation.target_.kind() == object::ObjectKind::ResourcePool) {
        auto child = allocation.target_.pin<kernel::resource::ResourcePool>();
        KASSERT(child);
        child.value()->bind_parent(allocation);
    }
    allocation.pool_->commit(allocation);
}

void GrantGraph::abort_allocation(
    kernel::resource::Allocation& allocation) noexcept {
    GrantKey root{};
    kernel::resource::ResourcePool* pool{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(allocation.graph_ == this && allocation.root_.valid());
        KASSERT(allocation.state_
            == kernel::resource::AllocationState::Pending);
        root = allocation.root_;
        pool = allocation.pool_;
        KASSERT(pool != nullptr);
        allocation.state_ = kernel::resource::AllocationState::Revoking;
    }

    // No user capability has been published while AllocationTxn is live.
    // Therefore this lineage has no legitimate operation or attachment and
    // permanent revocation must complete synchronously.
    const auto revoked = invalidate(root, allocation.revoke_);
    KASSERT(revoked && allocation.revoke_.complete());

    object::ObjectRef target{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Node* const node = find(root);
        KASSERT(node != nullptr && &node->allocation == &allocation);
        allocation.state_ = kernel::resource::AllocationState::Retiring;
        target = libk::move(allocation.target_);
    }
    if (target.kind() == object::ObjectKind::ResourcePool) {
        auto child = target.pin<kernel::resource::ResourcePool>();
        KASSERT(child);
        KASSERT(child.value()->close() == kernel::resource::PoolState::Closed);
    }
    KASSERT(target && target.retire());
    target.reset();

    pool->detach(allocation);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Node* const node = find(root);
        KASSERT(node != nullptr && &node->allocation == &allocation);
        KASSERT(allocation.pool_ == nullptr && !allocation.target_);
        allocation.graph_ = nullptr;
        allocation.root_ = {};
        allocation.state_ = kernel::resource::AllocationState::Empty;
    }
    reclaim(root, false);
}

void GrantGraph::revoke_allocation(
    kernel::resource::Allocation& allocation) noexcept {
    KASSERT(allocation.graph_ == this && allocation.root_.valid());
    const auto revoked = invalidate(allocation.root_, allocation.revoke_);
    KASSERT(revoked);
    if (allocation.revoke_.complete() || !allocation.revoke_.arm()) {
        allocation.ready();
    }
}

void GrantGraph::stop_allocation(
    kernel::resource::Allocation& allocation) noexcept {
    KASSERT(allocation.graph_ == this && allocation.root_.valid());
    KASSERT(allocation.pool_ != nullptr && allocation.target_);
    KASSERT(allocation.state_
        == kernel::resource::AllocationState::Stopping);

    switch (allocation.target_.kind()) {
    case object::ObjectKind::Thread: {
        auto thread = allocation.target_.pin<kernel::Thread>();
        KASSERT(thread);
        KASSERT(!allocation.stop_.started());
        allocation.stop_.start(thread.value().get());
        return;
    }
    case object::ObjectKind::Vproc: {
        auto vproc = allocation.target_.pin<kernel::Vproc>();
        KASSERT(vproc);
        KASSERT(!allocation.stop_.started());
        allocation.stop_.start(vproc.value().get());
        return;
    }
    case object::ObjectKind::ResourcePool: {
        auto child = allocation.target_.pin<kernel::resource::ResourcePool>();
        KASSERT(child);
        if (child.value()->close() == kernel::resource::PoolState::Closed
            && allocation.state_
                == kernel::resource::AllocationState::Stopping) {
            allocation.target_ready();
        }
        return;
    }
    case object::ObjectKind::SchedulingContext: {
        auto context = allocation.target_.pin<kernel::sched::SchedulingContext>();
        KASSERT(context);
        if (context.value()->binding() == nullptr) {
            allocation.target_ready();
            return;
        }
        KASSERT(!allocation.stop_.started());
        const execution::Target target = context.value()->binding()->target();
        if (Thread* const thread = target.thread()) {
            allocation.stop_.start(*thread);
        } else {
            allocation.stop_.start(*target.vproc());
        }
        return;
    }
    case object::ObjectKind::Invalid:
    case object::ObjectKind::Count:
        break;
    case object::ObjectKind::SchedulingDomain:
    case object::ObjectKind::CSpace:
    case object::ObjectKind::MemoryObject:
    case object::ObjectKind::VSpace:
    case object::ObjectKind::Notification:
    case object::ObjectKind::Tunnel:
        allocation.target_ready();
        return;
    }
    KASSERT(false);
}

void GrantGraph::retire_allocation(
    kernel::resource::Allocation& allocation) noexcept {
    KASSERT(allocation.graph_ == this && allocation.root_.valid());
    KASSERT(allocation.pool_ != nullptr && allocation.target_);

    if (allocation.target_.kind() == object::ObjectKind::ResourcePool) {
        auto child = allocation.target_.pin<kernel::resource::ResourcePool>();
        KASSERT(child);
        if (child.value()->close() != kernel::resource::PoolState::Closed) {
            return;
        }
        child.value()->unbind_parent(allocation);
    }
    if (allocation.stop_.started()) {
        KASSERT(allocation.stop_.started() && allocation.stop_.complete());
    }
    // Stopping has already drained every execution relation in this pool.
    // A false pre-retire result here means a target kind is missing an
    // explicit stop/dependency adapter; waiting silently would deadlock the
    // pool in Retiring with no event capable of re-driving it.
    const object::ObjectId target = allocation.target_.id();
    KASSERT_EVENT(
        allocation.target_.retire(),
        (diag::FatalEvent{
            .facility = diag::Facility::Capability,
            .id = diag::EventId{0x30000001},
            .arguments = {
                static_cast<usize>(target.kind),
                target.slot,
                static_cast<usize>(target.generation),
            },
            .argument_count = 3,
        }));
    allocation.target_.reset();

    const GrantKey root = allocation.root_;
    kernel::resource::ResourcePool* const pool = allocation.pool_;
    pool->detach(allocation);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Node* const node = find(root);
        KASSERT(node != nullptr && &node->allocation == &allocation);
        KASSERT(allocation.pool_ == nullptr && !allocation.target_);
        allocation.graph_ = nullptr;
        allocation.root_ = {};
        allocation.independent_close_ = false;
        allocation.state_ = kernel::resource::AllocationState::Empty;
    }
    reclaim(root, false);
}

auto GrantGraph::descendant_of(
    const Node& node,
    const Node& root) noexcept -> bool {
    for (const Node* current = node.parent;
         current != nullptr;
         current = current->parent) {
        if (current == &root) {
            return true;
        }
    }
    return false;
}

void GrantGraph::release_page(PageHeader& page) noexcept {
    KASSERT(page.live_count == 0);
    auto* const slots = reinterpret_cast<Slot*>(
        reinterpret_cast<usize>(&page) + slot_offset);
    for (usize index = 0; index < slots_per_page; ++index) {
        KASSERT(!slots[index].occupied);
        libk::destroy_at(&slots[index]);
    }
    kernel::mm::OwnedPage backing = libk::move(page.backing);
    libk::destroy_at(&page);
    KASSERT(page_count_ != 0);
    --page_count_;
    backing.reset();
}

} // namespace kernel::cap
