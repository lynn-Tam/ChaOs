#include <cap/grant_graph.hpp>

#include <cap/policy.hpp>
#include <core/debug.hpp>
#include <libk/limits.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
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
    : pmm_(&pmm), quota_(quota), revoke_waits_(pmm) {}

GrantRevokeWait::GrantRevokeWait(GrantGraph& graph) noexcept
    : graph_(&graph),
      completion_(kernel::sync::Completion::Notifier::bind<
          &GrantRevokeWait::ready>(*this)),
      relation_(kernel::WaitRelation::bind<
          GrantRevokeWait,
          &GrantRevokeWait::complete,
          &GrantRevokeWait::resume,
          &GrantRevokeWait::cancel>(*this)) {}

void GrantRevokeWait::ready() noexcept {
    KASSERT(completion_.complete());
    relation_.notify();
}

void GrantRevokeWait::resume(arch::TrapContext&) noexcept {
    KASSERT(completion_.complete());
    GrantGraph* const graph = graph_;
    KASSERT(graph != nullptr);
    graph->destroy_revoke_wait(*this);
}

void GrantRevokeWait::cancel() noexcept {
    KASSERT(!completion_.initialized());
    GrantGraph* const graph = graph_;
    KASSERT(graph != nullptr);
    graph->destroy_revoke_wait(*this);
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
        }
    }
    return libk::expected(made.value().object);
}

void GrantGraph::destroy_revoke_wait(GrantRevokeWait& operation) noexcept {
    revoke_waits_.destroy(operation);
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
    object::ObjectRef&& target,
    GrantCeiling ceiling,
    RegionDerivation proof) const noexcept
    -> libk::Expected<GrantRef, GrantError> {
    if (graph_ == nullptr) {
        return libk::unexpected(GrantError::InvalidKey);
    }
    return graph_->derive_region(
        *this, libk::move(target), ceiling, proof);
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
      node_(libk::exchange(other.node_, nullptr)),
      generation_(libk::exchange(other.generation_, u64{})) {}

auto GrantRef::operator=(GrantRef&& other) noexcept -> GrantRef& {
    if (this != &other) {
        reset();
        graph_ = libk::exchange(other.graph_, nullptr);
        node_ = libk::exchange(other.node_, nullptr);
        generation_ = libk::exchange(other.generation_, u64{});
    }
    return *this;
}

GrantRef::~GrantRef() noexcept {
    reset();
}

auto GrantRef::key() const noexcept -> GrantKey {
    KASSERT(graph_ != nullptr);
    const auto& node = *static_cast<const GrantGraph::Node*>(node_);
    KASSERT(node.slot->generation == generation_);
    return GrantGraph::key_of(node);
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
    void* const node = libk::exchange(node_, nullptr);
    const u64 generation = libk::exchange(generation_, u64{});
    if (graph != nullptr) {
        graph->drop_ref(node, generation);
    }
}

void GrantRevoke::initialize(usize pending) noexcept {
    completion_.initialize(pending);
}

void GrantRevoke::acknowledge() noexcept {
    completion_.acknowledge();
}

GrantGraph::~GrantGraph() noexcept {
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
    if (!target || !validate_ceiling(target.kind(), ceiling)) {
        return libk::unexpected(GrantError::RightsViolation);
    }
    return create(libk::move(target), ceiling, nullptr);
}

auto GrantGraph::derive(
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
    return create(libk::move(target), ceiling, parent);
}

auto GrantGraph::derive_region(
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
    return create(libk::move(target), ceiling, parent);
}

auto GrantGraph::create(
    object::ObjectRef&& target,
    GrantCeiling ceiling,
    Node* parent) noexcept -> libk::Expected<GrantRef, GrantError> {
    auto claimed = claim_slot();
    if (!claimed) {
        return libk::unexpected(claimed.error());
    }
    Slot* const slot = claimed.value();
    Node* const node = libk::construct_at(
        slot->node(), *slot, libk::move(target), ceiling, parent);

    object::ObjectRef returned{};
    bool rejected{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (parent != nullptr && (parent->reclaiming
            || parent->state != GrantState::Live)) {
            returned = libk::move(node->target);
            libk::destroy_at(node);
            slot->occupied = false;
            slot->next_free = slot->page->free_head;
            slot->page->free_head = slot;
            ++slot->page->free_count;
            --slot->page->live_count;
            --live_nodes_;
            rejected = true;
        } else {
            node->refs = 1;
            if (parent != nullptr) {
                KASSERT(parent->refs != libk::numeric_limits<usize>::max());
                ++parent->refs;
                parent->children.push_back(*node);
            }
        }
    }
    if (rejected) {
        returned.reset();
        return libk::unexpected(GrantError::InvalidState);
    }
    return libk::expected(GrantRef{*this, node, slot->generation});
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
        *this, &node, node.slot->generation});
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

void GrantGraph::drop_ref(void* raw, u64 generation) noexcept {
    KASSERT(raw != nullptr);
    reclaim(raw, generation, true);
}

void GrantGraph::drop_lease(void* raw, u64 generation) noexcept {
    KASSERT(raw != nullptr);
    auto& node = *static_cast<Node*>(raw);
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
        }
        reclaimable = node.refs == 0
            && node.operations == 0
            && node.attachments.empty()
            && node.children.empty();
    }
    if (completion != nullptr) {
        completion->acknowledge();
    }
    if (reclaimable) {
        reclaim(raw, generation, false);
    }
}

auto GrantGraph::detach(GrantAttachment& attachment) noexcept -> bool {
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
        }
        if (node.refs == 0
            && node.operations == 0
            && node.attachments.empty()
            && node.children.empty()) {
            reclaimable = &node;
            reclaim_generation = node_generation;
        }
    }
    if (completion != nullptr) {
        completion->acknowledge();
    }
    if (reclaimable != nullptr) {
        reclaim(reclaimable, reclaim_generation, false);
    }
    return quiescent;
}

void GrantGraph::reclaim(
    void* initial,
    u64 generation,
    bool drop_reference) noexcept {
    void* current = initial;
    u64 current_generation = generation;
    bool drop = drop_reference;
    while (current != nullptr) {
        object::ObjectRef target{};
        Node* parent{};
        u64 parent_generation{};
        Slot* slot{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            auto* const candidate = reinterpret_cast<Slot*>(
                reinterpret_cast<byte*>(current)
                - __builtin_offsetof(Slot, storage));
            if (!candidate->occupied
                || candidate->generation != current_generation) {
                return;
            }
            Node& node = *candidate->node();
            if (drop) {
                KASSERT(node.refs != 0);
                --node.refs;
            }
            if (node.refs != 0
                || node.operations != 0
                || !node.attachments.empty()
                || !node.children.empty()
                || node.reclaiming) {
                return;
            }
            node.reclaiming = true;
            parent = node.parent;
            if (parent != nullptr) {
                parent_generation = parent->slot->generation;
                parent->children.erase(node);
            }
            target = libk::move(node.target);
            slot = node.slot;
        }

        target.reset();

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
        // The child's structural parent reference remains counted until the
        // next iteration, so parent storage stays stable across target cleanup.
        current = parent;
        current_generation = parent_generation;
        drop = parent != nullptr;
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
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Node* const source = find(source_key);
        if (source == nullptr) {
            return libk::unexpected(GrantError::InvalidKey);
        }
        if (source->state != GrantState::Live || completion.initialized()) {
            return libk::unexpected(GrantError::InvalidState);
        }

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

        completion.initialize(pending);
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
                if (node.operations == 0 && node.attachments.empty()) {
                    node.state = GrantState::Revoked;
                } else {
                    node.state = GrantState::Revoking;
                    node.revoke = &completion;
                }
            }
        }
    }

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
