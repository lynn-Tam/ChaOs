#include <expected>
#include <array>
#include <cap/grant.hpp>

#include <object/ref.hpp>

#include <cap/cap.hpp>
#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <console.hpp>
#include <limits>
#include <libk/memory.hpp>
#include <libk/inplace_vector.hpp>
#include <utility>
#include <sync.hpp>

namespace cap {

auto Graph::Node::id() const noexcept -> u64 { return slot->id.value; }

Attachment::~Attachment() noexcept {
    libk_assert(graph_ == nullptr && node_ == nullptr);
}

auto Attachment::attached() const noexcept -> bool { return link_.attached(); }
auto Attachment::busy() const noexcept -> bool { return link_.busy(); }
void Attachment::reset() noexcept {
    libk_assert(graph_ == nullptr && node_ == nullptr);
    link_.reset();
}
auto Attachment::detach() noexcept -> bool {
    return graph_ ? graph_->detach(*this) : link_.detach();
}
auto Attachment::acquire() const noexcept -> std::expected<GrantPin, GrantError> {
    if (!graph_) return std::unexpected(GrantError::InvalidState);
    if (!node_) return std::unexpected(GrantError::InvalidState);
    return graph_->try_acquire(*static_cast<Graph::Node*>(node_)->slot, generation_);
}

auto Attachment::record(u32 kind) const noexcept -> LinkRecord {
    return {link_.id().value, link_.target_id().value, id().value, 0, 0, 0,
        kind, link_.phase(), 0, 0};
}

auto Attachment::id() const noexcept -> GrantId {
    if (!graph_) return {};
    sync::Lock guard{graph_->lock_};
    return node_ ? static_cast<const Graph::Node*>(node_)->slot->id : GrantId{};
}

Graph::Graph(mm::Pmm& pmm, ::WorkQueue& work) noexcept
    : Graph(pmm, work, QuotaLimit{}) {}

Graph::Graph(mm::Pmm& pmm, ::WorkQueue& work, QuotaLimit quota) noexcept
    : pmm_(&pmm),
      quota_(quota), executor_(work), job_(Work::Fn::bind<&Graph::run_work>(*this)) {}

auto GrantPin::key() const noexcept -> GrantKey {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    const auto& node = *static_cast<const Graph::Node*>(d.node);
    libk_assert(node.slot->generation.load(std::memory_order_acquire)
        == d.gen);
    return Graph::key_of(node);
}

auto GrantPin::id() const noexcept -> GrantId {
    return static_cast<const Graph::Node*>(h_.get().node)->slot->id;
}

auto GrantPin::graph() const noexcept -> Graph& {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return *d.graph;
}

auto GrantPin::kind() const noexcept -> ObjKind {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return static_cast<const Graph::Node*>(d.node)->target.kind();
}

auto GrantPin::get() const noexcept -> void* {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    // Revocation and reclamation cannot move Node::target until operations=0.
    return static_cast<const Graph::Node*>(d.node)->target.get();
}

auto GrantPin::target_live() const noexcept -> bool {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return static_cast<const Graph::Node*>(d.node)->target.live();
}

auto GrantPin::target_id() const noexcept -> object::Id {
    return static_cast<const Graph::Node*>(h_.get().node)->target.id();
}

auto GrantPin::ceiling() const noexcept -> View {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    return static_cast<const Graph::Node*>(d.node)->ceiling;
}

auto GrantPin::target() const noexcept -> const object::ref<>& {
    return static_cast<const Graph::Node*>(h_.get().node)->target;
}

auto GrantPin::clone_target() const noexcept
    -> std::expected<object::ref<>, object::error> {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    const auto& target = static_cast<const Graph::Node*>(d.node)->target;
    if (!target.live()) return std::unexpected(object::error::closed);
    return target;
}

auto GrantPin::attach(Attachment& attachment) const noexcept
    -> std::expected<void, GrantError> {
    const auto& d = h_.get();
    if (d.graph == nullptr) {
        return std::unexpected(GrantError::InvalidKey);
    }
    return d.graph->attach(*this, attachment);
}

auto GrantPin::mint(resource::Reservation&& usage, View ceiling) const noexcept
    -> std::expected<GrantRef, GrantError> {
    const auto& d = h_.get();
    if (!d.graph) return std::unexpected(GrantError::InvalidKey);
    auto* parent = static_cast<Graph::Node*>(d.node);
    if (!validate_ceiling(parent->target.kind(), ceiling))
        return std::unexpected(GrantError::RightsViolation);
    return d.graph->create(std::move(usage), object::ref<>{parent->target}, ceiling, parent);
}

void GrantPin::Drop::operator()(Data& d) const noexcept {
    d.graph->drop_pin(d.node, d.gen);
}

auto GrantRef::key() const noexcept -> GrantKey {
    const auto& d = h_.get();
    libk_assert(d.graph != nullptr);
    libk_assert(d.slot != nullptr && d.gen != 0);
    return GrantKey{reinterpret_cast<usize>(d.slot), d.gen};
}

auto GrantRef::id() const noexcept -> GrantId { return h_.get().id; }
auto GrantRef::target_id() const noexcept -> object::Id { return h_.get().target; }

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
    return d.graph->try_ref(*static_cast<Graph::Slot*>(d.slot), d.gen);
}

auto GrantRef::acquire() const noexcept
    -> std::expected<GrantPin, GrantError> {
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
    resource::Reservation&& usage,
    object::ref<> target,
    View ceiling) noexcept -> std::expected<GrantRef, GrantError> {
    if (!target || !validate_ceiling(target.kind(), ceiling)) {
        return std::unexpected(GrantError::RightsViolation);
    }
    return create(
        std::move(usage), std::move(target), ceiling, nullptr);
}

auto Graph::derive(
    const GrantPin& source,
    object::ref<> target,
    View ceiling) noexcept -> std::expected<GrantRef, GrantError> {
    return derive({}, source, std::move(target), ceiling);
}

auto Graph::derive(
    resource::Reservation&& usage,
    const GrantPin& source,
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
        std::move(usage), std::move(target), ceiling, parent);
}

auto Graph::create(
    resource::Reservation&& usage,
    object::ref<> target,
    View ceiling,
    Node* parent) noexcept -> std::expected<GrantRef, GrantError> {
    if (!target.live()) return std::unexpected(GrantError::InvalidState);
    const auto target_id = target.id();
    const auto target_key = target.key();
    if (parent && parent->depth >= max_depth) return std::unexpected(GrantError::QuotaExceeded);
    auto claimed = claim_slot();
    if (!claimed) {
        return std::unexpected(claimed.error());
    }
    Slot* const slot = claimed.value();
    const auto id = slot->id;
    const auto generation = slot->generation.load(std::memory_order_relaxed);
    Node* const node = libk::construct_at(
        slot->node(),
        *this,
        *slot,
        std::move(target),
        ceiling,
        parent,
        std::move(usage));

    object::ref<> returned{};
    bool rejected{};
    {
        sync::Lock guard{lock_};
        if (parent && !find(key_of(*parent))) {
            rejected = true;
        } else rejected = !node->lifetime.attach(node->target, [&] {
            node->refs = 1;
            if (parent != nullptr) {
                parent->children.insert(*node);
                changed(*parent);
            }
            // claim_slot() reserves storage but does not publish a Node.
            // Graph scans may dereference the payload only after construction
            // and parent linkage are complete under this lock.
            slot->occupied.store(true, std::memory_order_release);
            ids_.insert(*node);
            node->validated.store(node->root->epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
            // Stale GrantRefs may still name this stable Slot. Open it only
            // after the replacement payload and ancestry are fully installed.
            slot->operations.store(0, std::memory_order_release);
        });
    }
    if (rejected) {
        returned = std::move(node->target);
        auto release = node->allocation.detach();
        libk::destroy_at(node);
        {
            sync::Lock guard{lock_};
            slot->state.store(
                GrantState::Revoked, std::memory_order_release);
            slot->occupied.store(false, std::memory_order_release);
            static_cast<void>(storage_.release(*slot, false));
        }
        returned.reset();
        release.complete();
        return std::unexpected(GrantError::InvalidState);
    }
    if (parent) { object::changed(target_key); kick_work(); }
    return GrantRef{*this, slot, generation, id, target_id};
}

auto Graph::claim_slot() noexcept -> std::expected<Slot*, GrantError> {
    for (;;) {
        {
            sync::Lock guard{lock_};
            if (storage_.exhausted()) return std::unexpected(GrantError::GenerationExhausted);
            if (storage_.live() >= quota_.nodes) return std::unexpected(GrantError::QuotaExceeded);
            if (auto entry = storage_.claim(); entry.slot) {
                auto* slot = entry.slot;
                slot->id = object::new_id<Graph>();
                if (!slot->id.valid()) {
                    storage_.release(*slot, false);
                    return std::unexpected(GrantError::GenerationExhausted);
                }
                libk_assert(!slot->occupied.load(std::memory_order_relaxed));
                libk_assert(operation_count(slot->operations.load(std::memory_order_relaxed)) == 0);
                slot->generation.store(entry.generation, std::memory_order_relaxed);
                slot->state.store(GrantState::Live, std::memory_order_relaxed);
                slot->operations.store(operation_closed, std::memory_order_relaxed);
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
    for (auto* current = node; current; current = current->parent) {
        if (current->slot->state.load(std::memory_order_acquire) != GrantState::Live
            || admission_closed(current->slot->operations.load(std::memory_order_acquire))) return nullptr;
    }
    return node;
}

auto Graph::key_of(const Node& node) noexcept -> GrantKey {
    return GrantKey{
        reinterpret_cast<usize>(node.slot),
        node.slot->generation.load(std::memory_order_relaxed),
    };
}

auto Graph::ref(GrantKey key) noexcept -> std::expected<GrantRef, GrantError> {
    sync::Lock guard{lock_};
    auto* node = locate(key);
    return node ? try_ref(*node->slot, key.generation)
        : std::expected<GrantRef, GrantError>{std::unexpected(GrantError::InvalidKey)};
}

auto GrantPin::clone() const noexcept -> std::expected<GrantRef, GrantError> {
    const auto& d = h_.get();
    if (!d.graph) return std::unexpected(GrantError::InvalidKey);
    return d.graph->try_ref(*static_cast<Graph::Node*>(d.node)->slot, d.gen);
}

auto Graph::try_ref(Slot& slot, u64 generation) noexcept -> std::expected<GrantRef, GrantError> {
    sync::Lock guard{slot.lock};
    if (!slot.occupied.load(std::memory_order_acquire) || slot.generation.load() != generation
        || admission_closed(slot.operations.load())) return std::unexpected(GrantError::InvalidKey);
    auto& node = *slot.node();
    if (!valid(node)) return std::unexpected(GrantError::InvalidState);
    if (node.refs == std::numeric_limits<usize>::max()) return std::unexpected(GrantError::QuotaExceeded);
    ++node.refs;
    return GrantRef{*this, &slot, generation, slot.id, node.target.id()};
}

auto Graph::valid(Node& node) noexcept -> bool {
    const auto epoch = node.root->epoch.load(std::memory_order_acquire);
    if (epoch != std::numeric_limits<u64>::max()
        && node.validated.load(std::memory_order_acquire) == epoch
        && node.root->epoch.load(std::memory_order_acquire) == epoch) return true;
    // The leaf operation (or Slot lock) and children ownership keep this
    // immutable chain alive. Monotonic closure needs one pass, not a retry.
    for (auto* ancestor = &node; ancestor; ancestor = ancestor->parent)
        if (admission_closed(ancestor->slot->operations.load(std::memory_order_acquire))) return false;
    if (epoch == node.root->epoch.load(std::memory_order_acquire))
        node.validated.store(epoch, std::memory_order_release);
    return true;
}

auto Graph::acquire(GrantKey key) noexcept
    -> std::expected<GrantPin, GrantError> {
    sync::Lock guard{lock_};
    Node* const node = find(key);
    if (!node) return std::unexpected(GrantError::InvalidKey);
    return acquire_locked(*node->slot, key.generation);
}

auto Graph::observe(GrantKey seed_key, GrantId selected, usize depth) noexcept -> std::expected<Sample, GrantError> {
    sync::Lock guard{lock_};
    auto* seed = locate(seed_key);
    auto* node = selected.valid() ? ids_.find(selected.value) : seed;
    auto* ancestor = node;
    while (ancestor && seed && ancestor->depth > seed->depth) ancestor = ancestor->parent;
    if (!seed || !node || ancestor != seed || static_cast<usize>(node->depth - seed->depth) > depth) return std::unexpected(GrantError::InvalidKey);
    ++node->observers;
    return Sample{node->events, Source::Pin::bind<&Node::publisher>(*node)};
}

auto Graph::try_acquire(Slot& slot, u64 generation) noexcept
    -> std::expected<GrantPin, GrantError> {
    auto operations = slot.operations.load(std::memory_order_relaxed);
    for (;;) {
        if (admission_closed(operations)) return std::unexpected(GrantError::InvalidState);
        if (operation_count(operations) == operation_closed - 1)
            return std::unexpected(GrantError::QuotaExceeded);
        if (slot.operations.compare_exchange_weak(operations, operations + 1,
                std::memory_order_acquire, std::memory_order_relaxed)) break;
    }
    // Slots outlive stale keys; only this operation protects the current Node.
    // A CAS that crossed reuse can pin the new generation, but never use it.
    if (slot.generation.load(std::memory_order_relaxed) != generation) {
        release_operation(slot);
        return std::unexpected(GrantError::InvalidState);
    }
    auto& node = *slot.node();
    auto use = node.target.admit();
    if (!use) {
        release_operation(slot);
        return std::unexpected(GrantError::InvalidState);
    }
    if (valid(node)) return GrantPin{*this, &node, generation, std::move(use)};
    use.reset();
    release_operation(slot);
    return std::unexpected(GrantError::InvalidState);
}

auto Graph::acquire_locked(Slot& slot, u64 generation) noexcept
    -> std::expected<GrantPin, GrantError> {
    libk_assert(lock_.held());
    if (slot.generation.load(std::memory_order_relaxed) != generation
        || !slot.occupied.load(std::memory_order_relaxed)
        || !find(key_of(*slot.node())))
        return std::unexpected(GrantError::InvalidState);
    auto operations = slot.operations.load(std::memory_order_relaxed);
    if (operation_count(operations) == operation_closed - 1)
        return std::unexpected(GrantError::QuotaExceeded);
    auto use = slot.node()->target.admit();
    if (!use) return std::unexpected(GrantError::InvalidState);
    for (;;) {
        if (slot.operations.compare_exchange_weak(operations, operations + 1,
                std::memory_order_acquire, std::memory_order_relaxed)) break;
    }
    slot.node()->validated.store(slot.node()->root->epoch.load(std::memory_order_relaxed), std::memory_order_release);
    return GrantPin{*this, slot.node(), generation, std::move(use)};
}

auto Graph::attach(const GrantPin& source, Attachment& attachment) noexcept
    -> std::expected<void, GrantError> {
    std::array<const GrantPin*, 1> grants{&source};
    return admission(grants).attach(source, attachment, {}, {});
}

auto Graph::Admission::attach(std::span<const Attach> items,
    std::span<const object::ref<>*> gates, libk::delegate<void() noexcept> commit) const noexcept
    -> std::expected<void, GrantError> {
    if (items.empty() || items.size() > max_attachments)
        return std::unexpected(GrantError::InvalidState);
    std::array<object::ref<>, max_attachments> targets;
    std::array<object::Link::Prepared, max_attachments> prepared;
    for (usize i = 0; i < items.size(); ++i) {
        const auto [grant, attachment, target] = items[i];
        if (!attachment || !attachment->ops_ || bool(grant) == bool(target)
            || (grant && (!*grant || &grant->graph() != graph_)) || (target && !*target))
            return std::unexpected(GrantError::InvalidState);
        for (usize j = 0; j < i; ++j) if (items[j].attachment == attachment)
            return std::unexpected(GrantError::InvalidState);
        auto storage = grant ? grant->clone_target() : std::expected<object::ref<>, object::error>{*target};
        if (!storage) return std::unexpected(GrantError::InvalidState);
        targets[i] = std::move(*storage);
        auto made = attachment->link_.prepare(targets[i], attachment->ops_->observe);
        if (!made) return std::unexpected(GrantError::InvalidState);
        prepared[i] = std::move(*made);
    }
    if (!graph_) {
        libk::inplace_vector<const object::ref<>*, 2 * max_attachments> all;
        for (auto* target : gates) if (!target || !*target || !all.try_emplace_back(target))
            return std::unexpected(GrantError::InvalidState);
        for (usize i = 0; i < items.size(); ++i) {
            if (items[i].grant || !all.try_emplace_back(&targets[i])) return std::unexpected(GrantError::InvalidState);
        }
        if (!object::ref<>::publish(all.span(), [&] {
            for (usize i = 0; i < items.size(); ++i) items[i].attachment->link_.install(prepared[i]);
            if (commit) commit();
        })) return std::unexpected(GrantError::InvalidState);
        return {};
    }
    return graph_->attach_commit(items, std::span{prepared}.first(items.size()), gates, commit, grants_);
}

auto Graph::publish(std::span<const GrantPin*> grants, libk::delegate<bool() noexcept> commit) noexcept -> bool {
    std::array<sync::Spin*, IPC_MAX_CAPS * 2> locks;
    if (grants.size() > locks.size()) return false;
    for (usize i = 0; i < grants.size(); ++i) {
        if (!grants[i] || !*grants[i] || &grants[i]->graph() != this) return false;
        locks[i] = &static_cast<Node*>(grants[i]->h_.get().node)->slot->lock;
    }
    sync::Set local{std::span{locks}.first(grants.size())};
    for (auto* grant : grants) {
        auto& node = *static_cast<Node*>(grant->h_.get().node);
        if (admission_closed(node.slot->operations.load()) || !valid(node)) return false;
    }
    return commit();
}

auto Graph::attach_commit(std::span<const Attach> items, std::span<object::Link::Prepared> prepared,
    std::span<const object::ref<>*> gates, libk::delegate<void() noexcept> commit,
    std::span<const GrantPin*> grants) noexcept -> std::expected<void, GrantError> {
    libk_assert(items.size() <= max_attachments && items.size() == prepared.size());
    std::array<sync::Spin*, max_attachments * 3> locks;
    usize lock_count{};
    for (const auto& item : items) if (item.grant)
        locks[lock_count++] = &static_cast<Node*>(item.grant->h_.get().node)->slot->lock;
    for (auto* grant : grants) {
        if (!grant || !*grant || &grant->graph() != this || lock_count == locks.size())
            return std::unexpected(GrantError::InvalidState);
        locks[lock_count++] = &static_cast<Node*>(grant->h_.get().node)->slot->lock;
    }
    sync::Set local{std::span{locks}.first(lock_count)};
    libk::inplace_vector<const object::ref<>*, 5 * max_attachments> all;
    auto add = [&](const object::ref<>* target) noexcept {
        return target && *target && (std::ranges::any_of(all, [&](auto* existing) { return existing->id() == target->id(); })
            || all.try_emplace_back(target));
    };
    for (auto* gate : gates) if (!add(gate)) return std::unexpected(GrantError::InvalidState);
    for (auto* grant : grants) {
        if (!grant || !*grant || &grant->graph() != this) return std::unexpected(GrantError::InvalidState);
        auto* node = static_cast<Node*>(grant->h_.get().node);
        if (!valid(*node) || !add(&node->target)) return std::unexpected(GrantError::InvalidState);
    }
    std::array<Node*, max_attachments> nodes{};
    for (usize i = 0; i < items.size(); ++i) {
        const auto [grant, attachment, target] = items[i];
        if (!attachment || !attachment->ops_ || bool(grant) == bool(target)
            || (grant && (!*grant || &grant->graph() != this)) || (target && !*target))
            return std::unexpected(GrantError::InvalidState);
        auto* node = grant ? static_cast<Node*>(grant->h_.get().node) : nullptr;
        if ((node && (admission_closed(node->slot->operations.load()) || !valid(*node))) || attachment->graph_ || attachment->node_ || !attachment->ops_->invalidate
            || !attachment->ops_->released || attachment->link_.attached() || attachment->link_.busy()
            || !add(node ? &node->target : target)) return std::unexpected(GrantError::InvalidState);
        for (usize j = 0; j < i; ++j) if (items[j].attachment == attachment)
            return std::unexpected(GrantError::InvalidState);
        nodes[i] = node;
    }
    if (!object::ref<>::publish(all.span(), [&] {
        for (usize i = 0; i < items.size(); ++i) {
            auto& attachment = *items[i].attachment;
            attachment.link_.install(prepared[i]);
            if (nodes[i]) {
                attachment.graph_ = this; attachment.node_ = nodes[i];
                attachment.generation_ = items[i].grant->h_.get().gen;
                nodes[i]->attachments.push_back(attachment);
                nodes[i]->retaining += attachment.ops_->retain;
            }
        }
        if (commit) commit();
    })) return std::unexpected(GrantError::InvalidState);
    return {};
}

void Graph::drop_ref(void* raw, u64 generation) noexcept {
    auto& slot = *static_cast<Slot*>(raw);
    {
        sync::Lock guard{slot.lock};
        if (!slot.occupied.load() || slot.generation.load() != generation || !slot.node()->refs) return;
        if (slot.node()->refs > 1) { --slot.node()->refs; return; }
        // Keep the last reference until topology reclamation owns the slot.
    }
    reclaim(GrantKey{reinterpret_cast<usize>(raw), generation}, true);
}

void Graph::drop_pin(void* raw, u64 generation) noexcept {
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
        // Stable Slot protection also guards closed-operation handoff. Set
        // work retention before unlocking; final topology reclaim takes this
        // same Slot lock before it may destroy or reuse the Node.
        sync::Lock guard{slot.lock};
        libk_assert(slot.occupied.load(std::memory_order_acquire));
        const usize previous =
            slot.operations.fetch_sub(1, std::memory_order_acq_rel);
        libk_assert(admission_closed(previous)
            && operation_count(previous) != 0);
        if (operation_count(previous) == 1) {
            slot.work_retained.store(true, std::memory_order_release);
            enqueue(slot);
            schedule = true;
        }
    }
    if (schedule) kick_work();
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

void Graph::invalidate_object(void* context, object::LinkWork&& work) noexcept {
    auto& node = *static_cast<Node*>(context);
    auto& graph = *node.graph;
    {
        sync::Lock guard{graph.lock_};
        node.object_work = std::move(work);
        graph.close_node(node);
    }
    graph.kick_work();
}

void Graph::release_object(void* context, object::LinkWork&& done) noexcept {
    auto& node = *static_cast<Node*>(context);
    auto& graph = *node.graph;
    {
        sync::Lock guard{graph.lock_};
        done.reset();
        node.slot->work_retained.store(true, std::memory_order_release);
        graph.enqueue(*node.slot);
    }
    graph.kick_work();
}

void Graph::service_slot(Slot& slot) noexcept {
    Source::Publication changed;
    object::ref<> target;
    object::LinkWork work, object_work;
    Attachment* attachment{};
    GrantKey key{};
    {
        sync::Lock guard{lock_};
        if (!slot.occupied.load()) { slot.work_retained.store(false); return; }
        auto& node = *slot.node();
        key = key_of(node);
        if (std::exchange(node.dirty, false)) changed = node.events.prepare(0);
        if (slot.state.load() == GrantState::Live && admission_closed(slot.operations.load())) close_node(node);
        if (slot.state.load() == GrantState::Revoking) {
            if (auto* child = node.scan) {
                node.scan = node.children.next(*child);
                close_node(*child);
                if (child->slot->state.load() == GrantState::Revoked) acknowledge(*child);
                enqueue(slot); // One actual child per bounded worker step.
            }
            sync::Lock local{slot.lock};
            if (auto* candidate = node.attach_scan) {
                auto next = ++node.attachments.iterator_to(*candidate);
                node.attach_scan = next == node.attachments.end() ? nullptr : &*next;
                work = candidate->link_.claim();
                if (work) attachment = candidate;
                if (node.attach_scan) enqueue(slot);
            }
            if (!operation_count(slot.operations.load()) && node.attachments.empty()) {
                if (node.lifetime.attached()) (void)node.lifetime.detach();
                object_work = std::move(node.object_work);
                if (!node.lifetime.busy()) target = std::move(node.target);
            }
        }
    }
    changed.publish();
    object_work.reset();
    if (target) object::changed(target.key());
    target.reset();
    if (attachment) {
        attachment->link_.invalidate(std::move(work));
        enqueue(slot);
    }
    Source::Publication publication;
    bool terminal{};
    {
        sync::Lock guard{lock_};
        auto& node = *slot.node();
        sync::Lock local{slot.lock};
        if (slot.state.load() == GrantState::Revoking && !operation_count(slot.operations.load())
            && node.attachments.empty() && !node.lifetime.busy() && !node.target && !node.pending && !node.scan) {
            slot.state.store(GrantState::Revoked, std::memory_order_release);
            acknowledge(node);
            publication = node.events.prepare(STATE_DRAINED, true);
            terminal = true;
        }
    }
    publication.publish();
    if (terminal) slot.node()->events.close();
    // The worker's real slot hold covers all lock-external delivery above.
    bool reclaimable{};
    {
        sync::Lock guard{lock_};
        sync::Lock local{slot.lock};
        sync::Lock queued{work_lock_};
        if (!slot.work_hook.is_linked()) {
            slot.work_retained.store(false, std::memory_order_release);
            auto& node = *slot.node();
            reclaimable = !node.refs && !node.observers && node.children.empty()
                && !operation_count(slot.operations.load()) && node.attachments.empty() && !node.lifetime.busy();
        }
    }
    if (reclaimable) reclaim(key, false);
}

auto Graph::detach(Attachment& attachment) noexcept -> bool {
    bool schedule{}, quiescent{};
    {
        auto& node = *static_cast<Node*>(attachment.node_);
        sync::Lock guard{node.slot->lock};
        libk_assert(attachment.graph_ == this && node.slot->generation.load() == attachment.generation_);
        if (node.attach_scan == &attachment) {
            auto next = ++node.attachments.iterator_to(attachment);
            node.attach_scan = next == node.attachments.end() ? nullptr : &*next;
        }
        node.retaining -= attachment.ops_->retain;
        node.attachments.erase(attachment);
        attachment.graph_ = nullptr; attachment.node_ = nullptr; attachment.generation_ = 0;
        quiescent = attachment.link_.detach();
        // A retaining relation is a real owner. Its last removal hands the
        // node to the cold reclaimer, never calls topology code under this lock.
        if (node.slot->state.load() != GrantState::Live || !node.refs) {
            node.slot->work_retained.store(true, std::memory_order_release);
            enqueue(*node.slot); schedule = true;
        }
    }
    if (schedule) kick_work();
    return quiescent;
}

void Graph::reclaim(
    GrantKey initial,
    bool drop_reference) noexcept {
    GrantKey current = initial;
    bool drop = drop_reference;
    while (current.valid()) {
        object::ref<> target{};
        resource::Release release{};
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
            sync::Lock local{node.slot->lock};
            if (drop) {
                // A permanent revoke invalidates all outstanding GrantRef
                // tokens. Their later destruction is a stale-key no-op.
                if (node.refs == 0) {
                    return;
                }
                --node.refs;
            }
            if (!node.refs && node.parent && node.parent->slot->state.load() == GrantState::Revoking
                && node.slot->state.load() == GrantState::Live) {
                local.restore();
                close_node(node);
                guard.restore();
                kick_work();
                return;
            }
            if (!node.refs && node.children.empty() && node.slot->state.load() == GrantState::Live
                && !node.events.idle() && !node.retaining) {
                local.restore(); close_node(node); guard.restore(); kick_work(); return;
            }
            if (node.refs != 0 || node.observers || !node.events.idle() || !node.children.empty()
                || node.slot->work_retained.load(std::memory_order_acquire)) return;
            if (node.slot->state.load() == GrantState::Revoking) {
                node.slot->work_retained.store(true); enqueue(*node.slot); orphaned = true;
            } else if (!node.attachments.empty()) {
                if (node.retaining) return;
                // No holder can use this branch again. Drain weak observers
                // through the same worker as explicit revocation, without a waiter.
                local.restore(); close_node(node);
                orphaned = true;
            } else {
                const usize operations = node.slot->operations.fetch_or(operation_closed, std::memory_order_acq_rel);
                if (operation_count(operations) != 0) return;
                if (node.lifetime.attached() && !node.lifetime.detach()) return;
                if (node.lifetime.busy()) return;
                parent = node.parent;
                if (parent != nullptr) {
                    parent_key = key_of(*parent);
                    if (parent->scan == &node) {
                        parent->scan = parent->children.next(node);
                    }
                    acknowledge(node);
                    parent->children.erase(node);
                    changed(*parent);
                }
                target = std::move(node.target);
                slot = node.slot;
                // Claim finalization before dropping the lock. Releases and
                // target release can reenter the graph; stale holders must
                // not unlink this node or claim its storage a second time.
                slot->state.store(GrantState::Revoked, std::memory_order_release);
                slot->occupied.store(false, std::memory_order_release);
                ids_.erase(node);
            }
        }

        if (orphaned) { kick_work(); return; }
        if (parent) kick_work();
        if (parent && target) object::changed(target.key());
        target.reset();
        release = slot->node()->allocation.detach();

        {
            sync::Lock guard{lock_};
            libk::destroy_at(slot->node());
            libk_assert(!slot->occupied.load(std::memory_order_acquire));
            static_cast<void>(storage_.release(*slot, false));
        }
        release.complete();
        // A non-empty child list itself prevents parent reclamation; it does
        // not need a second synthetic reference count.
        current = parent_key;
        drop = false;
    }
}

auto Graph::view(GrantKey key, View slot) const noexcept -> std::optional<CapView> {
    sync::Lock guard{lock_};
    const auto* node = find(key);
    if (!node || !node->target.live()) return std::nullopt;
    const auto view = compose(node->target.kind(), node->ceiling, slot);
    return view ? std::optional{describe(node->target.kind(), *view)} : std::nullopt;
}

auto Graph::inspect(GrantKey seed_key, GrantQuery query, usize depth, GrantRecords& out) const noexcept -> status_t {
    sync::Lock guard{lock_};
    auto* seed = locate(seed_key);
    auto* selected = query.selected ? ids_.find(query.selected) : seed;
    if (!seed || !selected || selected->depth < seed->depth || static_cast<usize>(selected->depth - seed->depth) > depth)
        return STATUS_NOT_FOUND;
    auto* ancestor = selected;
    while (ancestor && ancestor->depth > seed->depth) ancestor = ancestor->parent;
    if (ancestor != seed) return STATUS_NOT_FOUND;
    if (selected->version == std::numeric_limits<u64>::max() || (query.epoch && query.epoch != selected->version))
        return STATUS_STALE;
    out = {};
    out.version = 1; out.size = sizeof(out); out.seed = seed->id(); out.selected = selected->id(); out.epoch = selected->version;
    auto record = [&](const Node& node) noexcept {
        return GrantRecord{node.id(), &node == seed || !node.parent ? 0 : node.parent->id(),
            node.lifetime.target_id().value, static_cast<u32>(node.slot->state.load()),
            static_cast<u32>(&node == seed && node.parent ? OBS_BOUNDARY : 0), describe(node.kind, node.ceiling)};
    };
    out.root = record(*selected);
    if (static_cast<usize>(selected->depth - seed->depth) == depth) {
        if (!selected->children.empty()) out.flags |= OBS_TRUNCATED;
        return STATUS_OK;
    }
    auto* child = query.last == std::numeric_limits<u64>::max() ? nullptr : selected->children.lower_bound(query.last + 1);
    while (child && out.count < GRANT_RECORD_PAGE) {
        out.records[out.count++] = record(*child);
        out.last = child->id(); child = selected->children.next(*child);
    }
    out.more = child != nullptr;
    if (!out.more) out.last = 0;
    return STATUS_OK;
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

auto Graph::node_usage() noexcept -> resource::Capacity {
    return resource::Capacity{.memory = sizeof(Slot)};
}

void Graph::changed(Node& node) noexcept {
    libk_assert(lock_.held());
    if (node.version != std::numeric_limits<u64>::max()) ++node.version;
    if (!node.observers) { node.events.update(0); return; }
    node.dirty = true;
    node.slot->work_retained.store(true); enqueue(*node.slot);
}

void Graph::close_node(Node& node) noexcept {
    libk_assert(lock_.held());
    sync::Lock local{node.slot->lock};
    if (node.slot->state.load() == GrantState::Revoked) return;
    if (node.slot->state.load() == GrantState::Live) {
        const bool cut = !node.parent || node.parent->slot->state.load() == GrantState::Live;
        node.refs = 0;
        node.scan = node.children.minimum();
        node.attach_scan = node.attachments.empty() ? nullptr : &node.attachments.front();
        changed(node);
        if (node.parent) changed(*node.parent);
        node.pending = node.children.size();
        node.slot->operations.fetch_or(operation_closed, std::memory_order_acq_rel);
        node.slot->state.store(GrantState::Revoking, std::memory_order_release);
        // Publish closure before the epoch cut. A cached admission crossing
        // this window precedes the cut; its leaf operation joins subtree drain.
        if (cut) {
            const auto epoch = node.root->epoch.load(std::memory_order_relaxed);
            if (epoch != std::numeric_limits<u64>::max())
                node.root->epoch.store(epoch + 1, std::memory_order_release);
        }
    }
    // Repeated close can carry a newly arrived invalidation ticket. Even a
    // retained in-flight step may have passed the state it must now consume.
    node.slot->work_retained.store(true, std::memory_order_release);
    enqueue(*node.slot);
}
void Graph::acknowledge(Node& node) noexcept {
    libk_assert(lock_.held());
    auto* parent = node.parent;
    if (!parent || node.acknowledged || parent->slot->state.load() != GrantState::Revoking) return;
    node.acknowledged = true;
    libk_assert(parent->pending); --parent->pending;
    parent->slot->work_retained.store(true, std::memory_order_release);
    enqueue(*parent->slot);
}
void Graph::Node::release_wait() noexcept {
    auto* owner = graph;
    bool schedule;
    {
        sync::Lock guard{owner->lock_};
        libk_assert(observers);
        sync::Lock local{slot->lock};
        schedule = --observers == 0 && (slot->state.load() != GrantState::Live
            || (!refs && children.empty() && !retaining));
        if (schedule) { slot->work_retained.store(true); owner->enqueue(*slot); }
    }
    if (schedule) owner->kick_work(); // No node access after handing back its last hold.
}

void Graph::Node::publisher(bool retain) noexcept {
    if (!retain) { release_wait(); return; }
    sync::Lock guard{graph->lock_}; ++observers;
}
auto Graph::close(GrantKey key, Source::Context* context, GrantId selected) noexcept -> WaitResult {
    WaitResult result{STATUS_PENDING, STATE_CLOSING, 1};
    object::ref<> target;
    {
        sync::Lock guard{lock_};
        auto* node = locate(key);
        if (selected.valid()) {
            auto* child = ids_.find(selected.value);
            auto* parent = child;
            while (parent && node && parent->depth > node->depth) parent = parent->parent;
            if (!node || !child || parent != node) return {STATUS_NOT_FOUND, 0, 0};
            node = child;
        }
        if (!node || node->slot->state.load() == GrantState::Revoked) return {STATUS_OK, STATE_DRAINED, 1};
        if (node->slot->state.load() == GrantState::Live) target = node->target;
        close_node(*node);
        if (context) {
            ++node->observers;
            result = node->events.await(*context, {.mask = STATE_DRAINED}, {}, {},
                Source::Action::bind<&Node::release_wait>(*node));
            if (result.status != STATUS_PENDING) --node->observers;
        }
    }
    if (target) object::changed(target.key());
    kick_work();
    return result;
}
auto Graph::status(GrantKey key) const noexcept -> WaitResult {
    sync::Lock guard{lock_};
    const auto* node = locate(key);
    if (!node || node->slot->state.load() == GrantState::Revoked) return {STATUS_OK, STATE_DRAINED, 1};
    const bool closing = node->slot->state.load() == GrantState::Revoking;
    return {STATUS_OK, closing ? STATE_CLOSING : STATE_OPEN, closing ? 1u : 0u};
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
