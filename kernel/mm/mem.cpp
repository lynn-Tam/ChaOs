#include <expected>
#include <mm/mem.hpp>
#include <task/thread.hpp>

#include <variant>

#include <mm/pager.hpp>
#include <object/ref.hpp>

#include <base/types.hpp>
#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/mem.h>
#include <libk/memory.hpp>
#include <limits>
#include <mm/pmm.hpp>
#include <sync.hpp>
#include <utility>

namespace mm {

auto WaitClaim::publish() noexcept -> bool {
    if (!*this) return false;
    auto d = std::exchange(d_, {});
    d.publish(d.owner, d.result);
    return true;
}

auto WaitQueue::attach(WaitRelation& relation, void* owner_context, WaitRelation::Publish delivery) noexcept
    -> bool {
    if (relation.attached() || relation.generation == std::numeric_limits<u64>::max() ||
        delivery == nullptr) {
        return false;
    }
    if (relation.arm && !relation.arm(owner_context)) return false;
    relation.owner = owner_context;
    relation.publish = delivery;
    relation.request = this;
    ++relation.generation;
    relation.state_.store<libk::MemoryOrder::Release>(static_cast<u8>(WaitPhase::Attached));
    waiters.push_back(relation);
    return true;
}

auto WaitQueue::detach(WaitRelation& relation, u64 expected_generation) noexcept -> bool {
    if (!relation.attached() || relation.request != this || relation.generation != expected_generation ||
        relation.state() != WaitPhase::Attached || !relation.hook_.is_linked()) {
        return false;
    }
    waiters.erase(relation);
    relation.owner = nullptr;
    relation.publish = nullptr;
    relation.request = nullptr;
    relation.state_.store<libk::MemoryOrder::Release>(static_cast<u8>(WaitPhase::Detached));
    return true;
}

auto WaitQueue::take() noexcept -> List {
    for (auto& r : waiters) r.state_.store<libk::MemoryOrder::Release>(static_cast<u8>(WaitPhase::Publishing));
    return std::move(waiters);
}

auto WaitQueue::finish(List& batch, WaitRc rc) noexcept -> WaitClaim {
    libk_assert(!batch.empty());
    auto& r = batch.front();
    batch.erase(r);
    libk_assert(r.state() == WaitPhase::Publishing);
    WaitClaim claim{r.owner, r.publish, rc};
    r.owner = nullptr;
    r.publish = nullptr;
    r.request = nullptr;
    r.state_.store<libk::MemoryOrder::Release>(static_cast<u8>(WaitPhase::Detached));
    return claim;
}

PageReq::PageReq(Thread& thread, CpuRegistry& cpus) noexcept
    : thread_(thread), cpus_(cpus),
      done_(Completion::bind<PageReq, &PageReq::release, &PageReq::cancel>(*this)) {
    relation.arm = &PageReq::arm;
}

auto PageReq::arm(void* p) noexcept -> bool {
    auto& req = *static_cast<PageReq*>(p);
    return req.thread_.begin_wait(req.done_, req.cpus_);
}

void PageReq::publish(void* p, WaitRc rc) noexcept {
    auto& req = *static_cast<PageReq*>(p);
    req.result_ = rc;
    req.done_.signal();
}

auto PageReq::wait(Mem* mem) noexcept -> WaitRc {
    // Only the owning thread drains/cancels its wait. A producer may signal
    // before this assignment, but cannot release the operation pin.
    if (!done_.attached()) {
        libk_assert(!mem);
        return thread_.stop_requested() ? WaitRc::Canceled : WaitRc::Ready;
    }
    mem_ = mem;
    if (!mem_) done_.signal();
    thread_.block();
    return thread_.stop_requested() ? WaitRc::Canceled : result_;
}

void PageReq::release() noexcept {
    libk_assert(!relation.attached());
    if (auto* mem = std::exchange(mem_, nullptr)) mem->release_fault();
}

auto PageReq::cancel() noexcept -> bool {
    if (!mem_) return false;
    const bool removed = mem_->cancel_fault(relation, relation.generation);
    if (!removed) return false;
    mem_ = nullptr;
    result_ = WaitRc::Canceled;
    return true;
}

auto Mem::populate(Thread& thread, CpuRegistry& cpus, usize page) noexcept -> std::expected<void, MemErr> {
    for (;;) {
        PageReq req{thread, cpus};
        auto result = materialize(page, &req.relation, &req, &PageReq::publish);
        const bool pending = !result && result.error() == MemErr::Pending;
        const auto rc = req.wait(pending ? this : nullptr);
        if (rc != WaitRc::Ready && rc != WaitRc::Dirty) return std::unexpected(MemErr::BackingFailed);
        if (result) return {};
        if (!pending) return std::unexpected(result.error());
    }
}

[[nodiscard]] auto validate_extents(Pmm& pmm, usize logical_pages, libk::Span<const Extent> extents,
                                    BackingKind kind, BootOwnership ownership,
                                    const PageGroup& owned) noexcept -> std::expected<void, MemErr> {
    if (extents.empty()) {
        return std::unexpected(MemErr::InvalidRange);
    }

    usize previous_end{};
    usize owned_pages{};
    for (usize index = 0; index < extents.size(); ++index) {
        const Extent& extent = extents[index];
        const auto object_end = extent.object.limit();
        if (!object_end || *object_end > logical_pages || extent.object.base() < previous_end ||
            !extent.physical.valid() || extent.physical.page_count() != extent.object.size()) {
            return std::unexpected(MemErr::InvalidRange);
        }
        if (!valid_perms(extent.access)) {
            return std::unexpected(MemErr::InvalidAccess);
        }
        const auto attr = pmm.attr_of(extent.physical);
        const bool ram = static_cast<bool>(pmm.direct_map().map(extent.physical.base().base(), extent.physical.byte_size()));
        if (!attr) return std::unexpected(MemErr::NotBacked);
        if (!ram && extent.access.contains(Perm::Execute)) {
            return std::unexpected(MemErr::InvalidMemoryType);
        }
        previous_end = *object_end;

        for (usize prior = 0; prior < index; ++prior) {
            if (!extent.physical.intersects(extents[prior].physical)) {
                continue;
            }
            return std::unexpected(MemErr::InvalidRange);
        }

        if (kind == BackingKind::Boot && !ram)
            return std::unexpected(MemErr::OwnershipMismatch);
        for (const Page page : extent.physical) {
            if (kind == BackingKind::Boot && ownership == BootOwnership::Owned) {
                if (!owned || !owned.contains(page)) return std::unexpected(MemErr::OwnershipMismatch);
                ++owned_pages;
            } else {
                // Borrowed RAM must stay reserved, outside reusable PMM states.
                const auto state = pmm.state_of(page);
                if (!state && !ram) continue; // External inventory has no allocator descriptor.
                if (!state || *state != PageState::Reserved)
                    return std::unexpected(MemErr::OwnershipMismatch);
            }
        }
    }

    if (kind == BackingKind::Boot) {
        if (ownership == BootOwnership::Owned && (!owned || owned.page_count() != owned_pages)) {
            return std::unexpected(MemErr::OwnershipMismatch);
        }
        if (ownership == BootOwnership::Borrowed && owned) {
            return std::unexpected(MemErr::OwnershipMismatch);
        }
    } else if (owned) {
        return std::unexpected(MemErr::OwnershipMismatch);
    }
    return {};
}

class Anon final : private libk::noncopyable_nonmovable {

    struct Node final {
        explicit Node(usize page_index) noexcept : index(page_index) {}

        usize index{};
        OwnedPage resident{};
        ContentState state{ContentState::Busy};
        libk::IntrusiveTreeHook tree_hook{};
        resource::Sponsorship resident_sponsorship{};
    };

    struct Compare final {
        [[nodiscard]] constexpr auto operator()(const Node& lhs, const Node& rhs) const noexcept -> bool {
            return lhs.index < rhs.index;
        }
        [[nodiscard]] constexpr auto operator()(usize lhs, const Node& rhs) const noexcept -> bool {
            return lhs < rhs.index;
        }
        [[nodiscard]] constexpr auto operator()(const Node& lhs, usize rhs) const noexcept -> bool {
            return lhs.index < rhs;
        }
    };

    using Tree = libk::IntrusiveTree<Node, &Node::tree_hook, Compare>;

  public:
    Anon(Pmm& pmm, Perms access, resource::Sponsorship* sponsor) noexcept
        : pmm_(&pmm), access_(access), rows_(pmm, {~usize{}, ~usize{}}, sponsor), sponsor_(sponsor) {}

    ~Anon() noexcept { reset(); }

    [[nodiscard]] auto query(usize page_index) const noexcept -> ContentState {
        sync::Lock guard{tree_lock_};
        const Node* const node = tree_.find(page_index);
        return node != nullptr ? node->state : ContentState::Zero;
    }

    [[nodiscard]] auto materialize(usize page_index) noexcept -> std::expected<Frame, MemErr> {
        for (;;) {
            {
                sync::Lock guard{tree_lock_};
                const Node* const existing = tree_.find(page_index);
                if (existing != nullptr) {
                    return page_of(*existing);
                }
            }

            auto claimed = rows_.create(page_index);
            if (!claimed) {
                return std::unexpected(claimed.error() == SlabErr::ResourceExhausted
                                           ? MemErr::ResourceExhausted
                                           : MemErr::OutOfMemory);
            }
            Node* const candidate = claimed.value();
            bool inserted{};
            {
                sync::Lock guard{tree_lock_};
                if (tree_.find(page_index) == nullptr) {
                    tree_.insert(*candidate);
                    inserted = true;
                }
            }
            if (!inserted) {
                rows_.destroy(*candidate);
                continue;
            }

            resource::Reservation charge{};
            auto reserved = reserve_page();
            if (!reserved) {
                rollback(*candidate);
                return std::unexpected(reserved.error());
            }
            charge = std::move(reserved).value();
            auto allocated = pmm_->allocate_page();
            if (!allocated) {
                rollback(*candidate);
                return std::unexpected(MemErr::OutOfMemory);
            }
            OwnedPage resident = std::move(allocated).value();
            memset(resident.bytes(), 0, page_size);
            const Page page = resident.page();
            {
                sync::Lock guard{tree_lock_};
                libk_assert(candidate->state == ContentState::Busy);
                candidate->resident = std::move(resident);
                if (charge) {
                    candidate->resident_sponsorship.commit(std::move(charge));
                }
                candidate->state = ContentState::Resident;
            }
            return (Frame{
                .page = page,
                .access = access_,
                .type = MemoryType::Normal,
            });
        }
    }

    [[nodiscard]] auto begin_transfer(usize page_index) noexcept -> std::expected<OwnedPage, MemErr> {
        sync::Lock guard{tree_lock_};
        Node* const node = tree_.find(page_index);
        if (node == nullptr || node->state != ContentState::Resident || !node->resident) {
            return std::unexpected(node == nullptr ? MemErr::NotBacked : MemErr::OwnershipMismatch);
        }
        node->state = ContentState::Busy;
        return (std::move(node->resident));
    }

    [[nodiscard]] auto restore_transfer(usize page_index, OwnedPage&& page) noexcept
        -> std::expected<void, MemErr> {
        if (!page) {
            return std::unexpected(MemErr::OwnershipMismatch);
        }
        sync::Lock guard{tree_lock_};
        Node* const node = tree_.find(page_index);
        if (node == nullptr || node->state != ContentState::Busy || node->resident) {
            return std::unexpected(MemErr::OwnershipMismatch);
        }
        node->resident = std::move(page);
        node->state = ContentState::Resident;
        return {};
    }

    [[nodiscard]] auto commit_transfer(usize page_index) noexcept -> std::expected<void, MemErr> {
        Node* node{};
        {
            sync::Lock guard{tree_lock_};
            node = tree_.find(page_index);
            if (node == nullptr || node->state != ContentState::Busy || node->resident) {
                return std::unexpected(MemErr::OwnershipMismatch);
            }
            tree_.erase(*node);
        }
        auto refund = node->resident_sponsorship.detach();
        rows_.destroy(*node);
        refund.complete();
        return {};
    }

    void reset() noexcept {
        for (;;) {
            Node* node{};
            {
                sync::Lock guard{tree_lock_};
                node = tree_.minimum();
                if (node != nullptr) {
                    tree_.erase(*node);
                }
            }
            if (node == nullptr) {
                break;
            }
            auto resident_refund = node->resident_sponsorship.detach();
            node->resident.reset();
            resident_refund.complete();
            rows_.destroy(*node);
        }
    }

  private:
    [[nodiscard]] auto page_of(const Node& node) const noexcept -> std::expected<Frame, MemErr> {
        switch (node.state) {
        case ContentState::Resident:
            return (Frame{
                .page = node.resident.page(),
                .access = access_,
                .type = MemoryType::Normal,
            });
        case ContentState::Busy:
            return std::unexpected(MemErr::Busy);
        case ContentState::Failed:
            return std::unexpected(MemErr::BackingFailed);
        case ContentState::Zero:
            break;
        }
        return std::unexpected(MemErr::NotBacked);
    }

    void rollback(Node& node) noexcept {
        {
            sync::Lock guard{tree_lock_};
            libk_assert(tree_.find(node.index) == &node);
            tree_.erase(node);
        }
        rows_.destroy(node);
    }

    [[nodiscard]] auto reserve_page() const noexcept -> std::expected<resource::Reservation, MemErr> {
        if (sponsor_ == nullptr) {
            return (resource::Reservation{});
        }
        auto reserved = sponsor_->reserve(resource::budget{
            .memory = page_size,
        });
        if (!reserved) {
            return std::unexpected(reserved.error() == resource::errc::exhausted ? MemErr::ResourceExhausted
                                                                                 : MemErr::InvalidState);
        }
        return (std::move(reserved).value());
    }

    Pmm* pmm_{};
    Perms access_{};
    mutable sync::Spin tree_lock_{};
    Tree tree_{};
    Slab<Node, false, false> rows_;
    resource::Sponsorship* sponsor_{};
};

class Paged final : private libk::noncopyable_nonmovable {
    struct Node {
        explicit Node(usize index) noexcept : index(index) {}
        usize index{};
        Node* next{};
        OwnedPage resident{};
        Pager::Request request{};
        WaitQueue waiters{};
        u64 dirty_epoch{}, usage_epoch{};
        bool failed{}, write_failed{};
        resource::Sponsorship resident_sponsorship{};
    };

  public:
    Paged(Mem& owner, Pmm& pmm, Pager& pager, Perms access, bool private_content,
          resource::Sponsorship* sponsor) noexcept
        : owner_(&owner), pager_(&pager), access_(access), private_content_(private_content),
          rows_(pmm, {~usize{}, ~usize{}}, sponsor), sponsor_(sponsor) {}
    ~Paged() noexcept { reset(); }
    [[nodiscard]] auto init() noexcept -> std::expected<void, MemErr> {
        if (pager_->state() != Pager::State::Open) return std::unexpected(MemErr::AttachmentState);
        return {};
    }
    void stop() noexcept {
        pager_->cancel(*owner_);
        Node* first;
        {
            sync::Lock guard{tree_lock_};
            first = nodes_;
        }
        for (auto* n = first; n; n = n->next) {
            {
                sync::Lock guard{tree_lock_};
                if (n->resident) continue;
                n->failed = true;
            }
            publish_waiters(*n, WaitRc::Failed);
        }
    }

    void publish_waiters(Node& n, WaitRc rc) noexcept {
        WaitQueue::List batch;
        {
            sync::Lock guard{tree_lock_};
            batch = n.waiters.take();
        }
        while (!batch.empty()) {
            WaitClaim claim;
            {
                sync::Lock guard{tree_lock_};
                claim = WaitQueue::finish(batch, rc);
            }
            libk_assert(claim.publish());
        }
    }

    [[nodiscard]] auto query(usize index) const noexcept -> ContentState {
        sync::Lock guard{tree_lock_};
        auto* n = find_locked(index);
        if (!n) return ContentState::Zero;
        if (n->resident) return ContentState::Resident;
        if (n->failed) return ContentState::Failed;
        return pager_->active(n->request) ? ContentState::Busy : ContentState::Zero;
    }

    [[nodiscard]] auto materialize(usize index, WaitRelation* relation, void* context,
                                   WaitRelation::Publish publish) noexcept -> std::expected<Frame, MemErr> {
        bool queued{};
        {
            sync::Lock guard{tree_lock_};
            if (!owner_->work_open_.load<libk::MemoryOrder::Acquire>())
                return std::unexpected(MemErr::InvalidState);
            auto* n = find_locked(index);
            if (!n) {
                auto allocated = rows_.create(index);
                if (!allocated)
                    return std::unexpected(allocated.error() == SlabErr::ResourceExhausted
                                               ? MemErr::ResourceExhausted
                                               : MemErr::OutOfMemory);
                n = *allocated;
                n->request.mem = owner_;
                n->next = nodes_;
                nodes_ = n;
            }
            if (n->resident) {
                return Frame{.page = n->resident.page(), .access = access_, .type = MemoryType::Normal};
            }
            if (n->failed) return std::unexpected(MemErr::BackingFailed);
            if (relation && !n->waiters.attach(*relation, context, publish))
                return std::unexpected(MemErr::Busy);
            if (!pager_->active(n->request)) {
                queued = pager_->enqueue(
                    n->request,
                    {.kind = Pager::Kind::PageIn, .page_index = index, .first = index, .count = 1});
                if (!queued) {
                    n->failed = true;
                    if (relation) libk_assert(n->waiters.detach(*relation, relation->generation));
                    return std::unexpected(MemErr::BackingFailed);
                }
            }
        }
        if (queued) pager_->signal();
        return std::unexpected(MemErr::Pending);
    }

    [[nodiscard]] auto cancel_fault(WaitRelation& relation, u64 generation) noexcept -> bool {
        sync::Lock guard{tree_lock_};
        return relation.request && relation.request->detach(relation, generation);
    }

    [[nodiscard]] auto supply(Pager& pager, u64 id, OwnedPage&& page) noexcept
        -> std::expected<void, MemErr> {
        if (&pager != pager_ || !page) return std::unexpected(MemErr::OwnershipMismatch);
        auto charge = reserve_page();
        if (!charge) return std::unexpected(charge.error());
        auto reply = pager.reply(id, owner_);
        Node* n;
        if (!reply || reply->req().kind != Pager::Kind::PageIn)
            return std::unexpected(MemErr::OwnershipMismatch);
        {
            sync::Lock guard{tree_lock_};
            n = find_locked(reply->req().page_index);
            if (!owner_->work_open_.load<libk::MemoryOrder::Acquire>() || n->failed || n->resident)
                return std::unexpected(MemErr::OwnershipMismatch);
            libk_assert(&n->request == reply->request());
            n->resident = std::move(page);
            if (*charge) n->resident_sponsorship.commit(std::move(*charge));
            libk_assert(reply->commit());
        }
        publish_waiters(*n, WaitRc::Ready);
        return {};
    }

    [[nodiscard]] auto finish(Pager& pager, u64 id, bool fail) noexcept -> std::expected<void, MemErr> {
        if (&pager != pager_) return std::unexpected(MemErr::OwnershipMismatch);
        auto reply = pager.reply(id, owner_);
        if (!reply) return std::unexpected(MemErr::OwnershipMismatch);
        if (!fail && reply->req().kind == Pager::Kind::PageIn) return std::unexpected(MemErr::InvalidState);
        complete(*reply, fail);
        return {};
    }

    void complete(Pager::Reply& reply, bool fail) noexcept {
        Node* n;
        const bool page_in = reply.req().kind == Pager::Kind::PageIn;
        {
            sync::Lock guard{tree_lock_};
            n = find_locked(reply.req().page_index);
            libk_assert(n && &n->request == reply.request());
            if (reply.req().kind == Pager::Kind::PageIn) {
                libk_assert(fail && !n->resident);
                n->failed = true;
            } else if (fail) n->write_failed = true;
            else if (n->dirty_epoch == reply.req().dirty_epoch) n->dirty_epoch = 0;
            libk_assert(reply.commit());
        }
        if (page_in) publish_waiters(*n, WaitRc::Failed);
    }

    [[nodiscard]] auto observe_usage(usize index, bool, bool dirty) noexcept -> std::expected<void, MemErr> {
        sync::Lock guard{tree_lock_};
        auto* n = find_locked(index);
        if (!n) return std::unexpected(MemErr::NotBacked);
        if (!n->resident) return std::unexpected(n->failed ? MemErr::BackingFailed : MemErr::Pending);
        if (dirty && n->usage_epoch == std::numeric_limits<u64>::max())
            return std::unexpected(MemErr::GenerationExhausted);
        if (dirty) n->dirty_epoch = ++n->usage_epoch;
        return {};
    }
    [[nodiscard]] auto writeback(usize index) noexcept -> std::expected<void, MemErr> {
        {
            sync::Lock guard{tree_lock_};
            auto* n = find_locked(index);
            if (!n) return std::unexpected(MemErr::NotBacked);
            if (private_content_ || !n->resident || !n->dirty_epoch || n->write_failed ||
                pager_->active(n->request))
                return std::unexpected(MemErr::InvalidState);
            if (!writeback_locked(*n)) return std::unexpected(MemErr::BackingFailed);
        }
        pager_->signal();
        return {};
    }

    auto trim(ObjectRange range) noexcept -> std::expected<void, MemErr> {
        {
            sync::Lock guard{tree_lock_};
            for (auto* n = nodes_; n; n = n->next) {
                if (!range.contains(n->index)) continue;
                if (pager_->active(n->request)) return std::unexpected(MemErr::Busy);
                if (n->dirty_epoch) return std::unexpected(MemErr::Dirty);
            }
        }
        for (auto* n = nodes_; n; n = n->next) {
            if (!range.contains(n->index)) continue;
            OwnedPage page;
            resource::Refund refund;
            {
                sync::Lock guard{tree_lock_};
                page = std::move(n->resident);
                refund = n->resident_sponsorship.detach();
                n->failed = false;
            }
            page.reset();
            refund.complete();
        }
        return {};
    }
    void reset() noexcept {
        while (nodes_) {
            auto* n = nodes_;
            nodes_ = n->next;
            libk_assert(!n->request.pager && n->waiters.waiters.empty());
            auto refund = n->resident_sponsorship.detach();
            n->resident.reset();
            refund.complete();
            rows_.destroy(*n);
        }
    }

  private:
    [[nodiscard]] auto writeback_locked(Node& n) noexcept -> bool {
        if (pager_->enqueue(n.request, {.kind = Pager::Kind::Writeback,
                                        .page_index = n.index,
                                        .first = n.index,
                                        .count = 1,
                                        .dirty_epoch = n.dirty_epoch}))
            return true;
        n.write_failed = true;
        return false;
    }
    [[nodiscard]] auto find_locked(usize index) const noexcept -> Node* {
        for (auto* n = nodes_; n; n = n->next)
            if (n->index == index) return n;
        return nullptr;
    }
    [[nodiscard]] auto reserve_page() const noexcept -> std::expected<resource::Reservation, MemErr> {
        if (sponsor_ == nullptr) {
            return (resource::Reservation{});
        }
        auto reserved = sponsor_->reserve(resource::budget{.memory = page_size});
        if (!reserved) {
            return std::unexpected(reserved.error() == resource::errc::exhausted ? MemErr::ResourceExhausted
                                                                                 : MemErr::InvalidState);
        }
        return (std::move(reserved).value());
    }

    Mem* owner_{};
    Pager* pager_{};
    Perms access_{};
    bool private_content_{};
    mutable sync::Spin tree_lock_{};
    Node* nodes_{};
    Slab<Node, false, false> rows_;
    resource::Sponsorship* sponsor_{};
};

class Extents final : private libk::noncopyable_nonmovable {
    struct Row {
        explicit Row(Extent e) noexcept : extent(e) {}
        Extent extent;
        Row* next{};
    };

  public:
    Extents(Pmm& pmm, resource::Sponsorship* sponsor, BackingKind kind) noexcept
        : kind(kind), pmm_(&pmm), rows_(pmm, {~usize{}, ~usize{}}, sponsor) {}
    ~Extents() noexcept { reset(); }
    const BackingKind kind;
    auto initialize(libk::Span<const Extent> extents, BootOwnership ownership = BootOwnership::Borrowed,
                    PageGroup&& pages = {}) noexcept -> std::expected<void, MemErr> {
        libk_assert(!head_);
        for (const auto& e : extents) {
            auto row = rows_.create(e);
            if (!row) {
                reset();
                return std::unexpected(row.error() == SlabErr::ResourceExhausted ? MemErr::ResourceExhausted
                                                                                 : MemErr::OutOfMemory);
            }
            (*row)->next = head_;
            head_ = *row;
        }
        if (ownership == BootOwnership::Owned) owned_ = std::move(pages);
        return {};
    }
    auto query(usize index) const noexcept -> ContentState {
        return find(index) ? ContentState::Resident : ContentState::Failed;
    }
    auto materialize(usize index) const noexcept -> std::expected<Frame, MemErr> {
        const auto* e = find(index);
        if (!e) return std::unexpected(MemErr::NotBacked);
        const auto frame = e->physical.base().checked_add(index - e->object.base());
        libk_assert(frame);
        const bool ram = static_cast<bool>(pmm_->direct_map().map(frame->base(), page_size));
        return Frame{Page{*frame}, e->access, ram ? MemoryType::Normal : MemoryType::Device};
    }
    void reset() noexcept {
        owned_.reset();
        while (head_) {
            auto* row = std::exchange(head_, head_->next);
            rows_.destroy(*row);
        }
    }

  private:
    const Extent* find(usize index) const noexcept {
        for (auto* row = head_; row; row = row->next)
            if (row->extent.object.contains(index)) return &row->extent;
        return nullptr;
    }
    Pmm* pmm_{};
    Slab<Row, false> rows_;
    Row* head_{};
    PageGroup owned_{};
};

// The active storage type stays fixed until all operations and mappings drain.
// It occupies the same sponsored physical page as the former backend tables.
struct Mem::Store {
    std::variant<Anon, Paged, Extents> data;

    template <class T, class... A>
    explicit Store(std::in_place_type_t<T> tag, A&&... args) noexcept : data(tag, std::forward<A>(args)...) {}
};

void MemWork::Drop::operator()(Data& d) const noexcept {
    auto* pin = d.pin;
    d.attachment->drop_work();
    if (pin) pin->drop_page();
}

MemLink::~MemLink() noexcept {
    const State current = static_cast<State>(state_.load<libk::MemoryOrder::Acquire>());
    libk_assert(current == State::Idle || current == State::Detached);
    libk_assert(owner_ == nullptr);
    libk_assert(work_.load<libk::MemoryOrder::Acquire>() == 0);
}

auto MemLink::attached() const noexcept -> bool {
    const State current = static_cast<State>(state_.load<libk::MemoryOrder::Acquire>());
    return current == State::Attached || current == State::Invalidating;
}

auto MemLink::busy() const noexcept -> bool { return work_.load<libk::MemoryOrder::Acquire>() != 0; }

auto MemLink::detach() noexcept -> bool {
    Mem* const owner = owner_;
    if (owner == nullptr) {
        return static_cast<State>(state_.load<libk::MemoryOrder::Acquire>()) == State::Detached && !busy();
    }
    return owner->detach(*this);
}

void MemLink::drop_work() noexcept {
    // detach() publishes Detached and then observes work_; this side removes
    // the last work pin and then observes Detached. Sequential consistency is
    // intentional: it forbids both sides from observing the other's old value
    // and thereby losing the final released() notification.
    const usize previous = work_.fetch_sub<libk::MemoryOrder::SeqCst>(1);
    libk_assert(previous != 0);
    if (previous == 1 && static_cast<State>(state_.load<libk::MemoryOrder::SeqCst>()) == State::Detached) {
        libk_assert(ops_ != nullptr && ops_->released != nullptr);
        ops_->released(context_);
    }
}

void PageHold::Drop::operator()(Data& d) const noexcept { d.owner->drop_page(); }

PageTransfer::PageTransfer(PageTransfer&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), index_(other.index_), page_(std::move(other.page_)) {}

auto PageTransfer::operator=(PageTransfer&& other) noexcept -> PageTransfer& {
    if (this != &other) {
        abort();
        owner_ = std::exchange(other.owner_, nullptr);
        index_ = other.index_;
        page_ = std::move(other.page_);
    }
    return *this;
}

PageTransfer::~PageTransfer() noexcept { abort(); }

void PageTransfer::commit() noexcept {
    Mem* const owner = std::exchange(owner_, nullptr);
    if (owner != nullptr) {
        owner->finish_transfer(index_, {}, true);
    }
    page_.reset();
}

void PageTransfer::abort() noexcept {
    Mem* const owner = std::exchange(owner_, nullptr);
    if (owner != nullptr) {
        owner->finish_transfer(index_, std::move(page_), false);
    }
    page_.reset();
}

Mem::Mem(Pmm& pmm, usize byte_size) noexcept : pmm_(&pmm) {
    if (byte_size != 0 && byte_size % page_size == 0) {
        logical_pages_ = byte_size / page_size;
    }
}

void Mem::bind_sponsor(resource::Sponsorship& sponsor) noexcept {
    libk_assert(sponsor_ == nullptr && sponsor);
    libk_assert(state_ == MemState::Building && store_ == nullptr);
    sponsor_ = &sponsor;
}

auto Mem::reserve_dynamic(resource::budget charge) noexcept -> std::expected<resource::Reservation, MemErr> {
    if (sponsor_ == nullptr) {
        return (resource::Reservation{});
    }
    auto reserved = sponsor_->reserve(charge);
    if (!reserved) {
        return std::unexpected(reserved.error() == resource::errc::exhausted ? MemErr::ResourceExhausted
                                                                             : MemErr::InvalidState);
    }
    return (std::move(reserved).value());
}

Mem::~Mem() noexcept {
    if (state_ == MemState::Building || state_ == MemState::Live) {
        retire();
    }
    libk_assert(state_ == MemState::Retired && !cleanup_);
    libk_assert(!releasing_);
    libk_assert(store_ == nullptr);
    libk_assert(!backing_page_);
    libk_assert(!backing_sponsorship_);
    libk_assert(operations_ == 0);
    libk_assert(attachments_.empty());
}

auto Mem::init_anon(AnonCfg config) noexcept -> std::expected<void, MemErr> {
    return initialize_backing(BackingKind::Anonymous, {}, config, BootOwnership::Borrowed, {}, nullptr, {},
                              {});
}

auto Mem::init_phys(libk::Span<const Extent> extents) noexcept -> std::expected<void, MemErr> {
    return initialize_backing(BackingKind::Physical, extents, {}, BootOwnership::Borrowed, {}, nullptr, {},
                              {});
}

auto Mem::init_boot(libk::Span<const Extent> extents, BootOwnership ownership, PageGroup&& owned) noexcept
    -> std::expected<void, MemErr> {
    return initialize_backing(BackingKind::Boot, extents, {}, ownership, std::move(owned), nullptr, {}, {});
}

auto Mem::init_paged(object::ref<>&& pager, Perms access, bool private_content) noexcept
    -> std::expected<void, MemErr> {
    auto pinned = pager.as<Pager>();
    if (!pinned) {
        return std::unexpected(MemErr::InvalidState);
    }
    return initialize_backing(BackingKind::Pager, {}, {}, BootOwnership::Borrowed, {}, &pinned.value().get(),
                              access, std::move(pager), private_content);
}

auto Mem::initialize_backing(BackingKind kind, libk::Span<const Extent> extents, AnonCfg anonymous,
                             BootOwnership boot_ownership, PageGroup&& boot_pages, Pager* pager,
                             Perms pager_access, object::ref<>&& pager_ref, bool private_content) noexcept
    -> std::expected<void, MemErr> {
    if (state_ != MemState::Building || logical_pages_ == 0) {
        fail_build();
        return std::unexpected(MemErr::InvalidSize);
    }
    if (kind == BackingKind::Anonymous) {
        if (!valid_perms(anonymous.access)) {
            fail_build();
            return std::unexpected(MemErr::InvalidAccess);
        }
    } else if (kind == BackingKind::Pager) {
        if (pager == nullptr || !valid_perms(pager_access) ||
            (pager_ref && pager_ref.kind() != object::ObjectKind::Pager)) {
            fail_build();
            return std::unexpected(MemErr::InvalidState);
        }
    } else {
        auto validated = validate_extents(*pmm_, logical_pages_, extents, kind, boot_ownership, boot_pages);
        if (!validated) {
            fail_build();
            return validated;
        }
    }

    auto backing_charge = reserve_dynamic(resource::budget{
        .memory = page_size,
    });
    if (!backing_charge) {
        fail_build();
        return std::unexpected(backing_charge.error());
    }
    auto allocated = pmm_->allocate_page();
    if (!allocated) {
        fail_build();
        return std::unexpected(MemErr::OutOfMemory);
    }
    OwnedPage storage = std::move(allocated).value();

    static_assert(sizeof(Store) <= page_size);
    Store* store{};
    std::expected<void, MemErr> initialized = {};
    switch (kind) {
    case BackingKind::Anonymous:
        store = std::construct_at(reinterpret_cast<Store*>(storage.bytes()), std::in_place_type<Anon>, *pmm_,
                                  anonymous.access, sponsor_);
        break;
    case BackingKind::Physical:
    case BackingKind::Boot:
        store = std::construct_at(reinterpret_cast<Store*>(storage.bytes()), std::in_place_type<Extents>,
                                  *pmm_, sponsor_, kind);
        initialized =
            std::get_if<Extents>(&store->data)->initialize(extents, boot_ownership, std::move(boot_pages));
        break;
    case BackingKind::Pager:
        store = std::construct_at(reinterpret_cast<Store*>(storage.bytes()), std::in_place_type<Paged>, *this,
                                  *pmm_, *pager, pager_access, private_content, sponsor_);
        initialized = std::get_if<Paged>(&store->data)->init();
        break;
    }

    if (!initialized) {
        std::destroy_at(store);
        storage.reset();
        const MemErr error = initialized.error();
        fail_build();
        return std::unexpected(error);
    }

    store_ = store;
    backing_page_ = std::move(storage);
    if (backing_charge.value()) {
        backing_sponsorship_.commit(std::move(backing_charge).value());
    }
    state_ = MemState::Live;
    if (kind == BackingKind::Anonymous || kind == BackingKind::Pager) {
        access_ = kind == BackingKind::Anonymous ? anonymous.access : pager_access;
        if (kind == BackingKind::Pager) {
            pager_ref_ = std::move(pager_ref);
        }
    } else {
        u8 access_bits{};
        for (const Extent& extent : extents) {
            access_bits |= extent.access.raw();
        }
        access_ = Perms::from_raw(access_bits);
    }

    if (kind != BackingKind::Anonymous && kind != BackingKind::Pager) {
        for (const Extent& extent : extents) {
            if (extent.access.contains(Perm::Execute)) {
                seal_ = SealState::Executable;
                content_epoch_ = ContentEpoch{1};
                break;
            }
        }
    }

    if (kind == BackingKind::Anonymous && anonymous.eager) {
        for (usize index = 0; index < logical_pages_; ++index) {
            auto page = materialize(index);
            if (!page) {
                const MemErr error = page.error();
                retire();
                return std::unexpected(error);
            }
        }
    }
    return {};
}

auto Mem::kind() const noexcept -> BackingKind {
    sync::Lock guard{lock_};
    libk_assert(store_ != nullptr);
    if (std::get_if<Anon>(&store_->data)) return BackingKind::Anonymous;
    if (std::get_if<Paged>(&store_->data)) return BackingKind::Pager;
    return std::get_if<Extents>(&store_->data)->kind;
}

auto Mem::state() const noexcept -> MemState {
    sync::Lock guard{lock_};
    return state_;
}

auto Mem::seal_state() const noexcept -> SealState {
    sync::Lock guard{lock_};
    return seal_;
}

auto Mem::content_epoch() const noexcept -> ContentEpoch {
    sync::Lock guard{lock_};
    return content_epoch_;
}

auto Mem::seal() noexcept -> std::expected<void, MemErr> {
    sync::Lock guard{lock_};
    if (state_ != MemState::Live || seal_ != SealState::Loadable) {
        return std::unexpected(MemErr::InvalidState);
    }
    seal_ = SealState::Sealing;
    for (const MemLink& attachment : attachments_) {
        if (attachment.access_.contains(Perm::Write)) {
            seal_ = SealState::Loadable;
            return std::unexpected(MemErr::Busy);
        }
    }
    libk_assert(content_epoch_.raw != std::numeric_limits<u64>::max());
    content_epoch_ = ContentEpoch{content_epoch_.raw + 1};
    seal_ = SealState::Executable;
    return {};
}

auto Mem::query(usize page_index) const noexcept -> std::expected<ContentState, MemErr> {
    const Store* store{};
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live) {
            return std::unexpected(MemErr::InvalidState);
        }
        if (page_index >= logical_pages_) {
            return std::unexpected(MemErr::InvalidRange);
        }
        libk_assert(operations_ != std::numeric_limits<usize>::max());
        ++const_cast<Mem*>(this)->operations_;
        store = store_;
    }
    Pin hold{*const_cast<Mem*>(this)};
    const auto result = [&] {
        if (auto* data = std::get_if<Anon>(&store->data)) return data->query(page_index);
        if (auto* data = std::get_if<Paged>(&store->data)) return data->query(page_index);
        return std::get_if<Extents>(&store->data)->query(page_index);
    }();
    return (result);
}

auto Mem::materialize(usize page_index) noexcept -> std::expected<PageHold, MemErr> {
    return materialize_impl(page_index, nullptr, nullptr, nullptr);
}

auto Mem::materialize(usize page_index, WaitRelation* relation, void* owner,
                      WaitRelation::Publish publish) noexcept -> std::expected<PageHold, MemErr> {
    return materialize_impl(page_index, relation, owner, publish);
}

auto Mem::materialize_impl(usize page_index, WaitRelation* relation, void* owner,
                           WaitRelation::Publish publish) noexcept -> std::expected<PageHold, MemErr> {
    Store* store{};
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live) {
            return std::unexpected(MemErr::InvalidState);
        }
        if (page_index >= logical_pages_) {
            return std::unexpected(MemErr::InvalidRange);
        }
        libk_assert(operations_ != std::numeric_limits<usize>::max());
        if (trimming_) {
            if (!relation) return std::unexpected(MemErr::Busy);
            if (!trim_waiters_.attach(*relation, owner, publish)) return std::unexpected(MemErr::Busy);
            ++operations_;
            return std::unexpected(MemErr::Pending);
        }
        ++operations_;
        store = store_;
    }
    Pin hold{*this};

    auto result = [&]() noexcept -> std::expected<Frame, MemErr> {
        if (auto* data = std::get_if<Paged>(&store->data))
            return data->materialize(page_index, relation, owner, publish);
        if (auto* data = std::get_if<Anon>(&store->data)) return data->materialize(page_index);
        return std::get_if<Extents>(&store->data)->materialize(page_index);
    }();
    const bool retained = relation && !result && result.error() == MemErr::Pending;
    bool live{};
    {
        sync::Lock guard{lock_};
        live = state_ == MemState::Live;
    }
    if (retained) {
        hold.release(); // The attached fault relation now owns this operation.
        return std::unexpected(MemErr::Pending);
    }
    if (!result) {
        return std::unexpected(live ? result.error() : MemErr::InvalidState);
    }
    if (!live) {
        return std::unexpected(MemErr::InvalidState);
    }
    hold.release(); // The installed PTE or caller now owns this operation pin.
    return (PageHold{*this, result.value()});
}

auto Mem::begin_transfer(usize page_index) noexcept -> std::expected<PageTransfer, MemErr> {
    Anon* backing{};
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || !attachments_.empty() || operations_ != 0 ||
            page_index >= logical_pages_ || store_ == nullptr) {
            return std::unexpected((!attachments_.empty() || operations_ != 0) ? MemErr::Busy
                                                                               : MemErr::InvalidState);
        }
        libk_assert(operations_ != std::numeric_limits<usize>::max());
        ++operations_;
        backing = std::get_if<Anon>(&store_->data);
    }
    Pin hold{*this};
    auto page = backing ? backing->begin_transfer(page_index)
                        : std::expected<OwnedPage, MemErr>{std::unexpected(MemErr::OwnershipMismatch)};
    if (!page) {
        return std::unexpected(page.error());
    }
    hold.release(); // The move-only transfer owns the source operation.
    return (PageTransfer{*this, page_index, std::move(page).value()});
}

template <class F> auto Mem::paged(F&& fn, MemErr error) noexcept {
    using Result = decltype(fn(std::declval<Paged&>()));
    Paged* backing{};
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || trimming_ || !store_ ||
            !(backing = std::get_if<Paged>(&store_->data)))
            return Result{std::unexpected(error)};
        ++operations_;
    }
    Pin hold{*this};
    return std::forward<F>(fn)(*backing);
}

auto Mem::supply(Pager& pager, u64 id, OwnedPage&& page) noexcept -> std::expected<void, MemErr> {
    return paged([&](Paged& p) noexcept { return p.supply(pager, id, std::move(page)); });
}

auto Mem::supply(Pager& pager, PageTransfer&& transfer, u64 id) noexcept -> std::expected<void, MemErr> {
    if (!transfer || !transfer.page().valid()) return std::unexpected(MemErr::OwnershipMismatch);
    const auto result = supply(pager, id, std::move(transfer.page_));
    if (result) transfer.commit();
    else transfer.abort();
    return result;
}

auto Mem::observe_usage(usize page_index, bool accessed, bool dirty) noexcept -> std::expected<void, MemErr> {
    Paged* backing{};
    {
        sync::Lock guard{lock_};
        if ((state_ != MemState::Live && state_ != MemState::Stopping) || page_index >= logical_pages_ ||
            store_ == nullptr) {
            return std::unexpected(MemErr::InvalidState);
        }
        ++operations_;
        backing = std::get_if<Paged>(&store_->data);
    }
    Pin hold{*this};
    auto result = backing != nullptr ? backing->observe_usage(page_index, accessed, dirty)
                                     : std::expected<void, MemErr>{};
    return result;
}

auto Mem::trim(ObjectRange range, WaitRelation& waiter, void* ctx, WaitRelation::Publish publish) noexcept
    -> std::expected<void, MemErr> {
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || !store_ || !std::get_if<Paged>(&store_->data))
            return std::unexpected(MemErr::InvalidState);
        if (range.empty() || !ObjectRange{0, logical_pages_}.contains(range))
            return std::unexpected(MemErr::InvalidRange);
        // No recursive wait for this Pager's own outstanding requests.
        if (trimming_ || trim_walk_ || request_pins_.load<libk::MemoryOrder::Acquire>())
            return std::unexpected(MemErr::Busy);
        if (!trim_waiters_.attach(waiter, ctx, publish)) return std::unexpected(MemErr::Busy);
        operations_ += 2; // The trim and this admitted waiter own separate pins.
        trimming_ = range;
        trim_walk_ = true;
    }
    invalidate(range);
    {
        sync::Lock guard{lock_};
        trim_walk_ = false;
    }
    finish_trim();
    return std::unexpected(MemErr::Pending);
}

auto Mem::trim(Thread& thread, CpuRegistry& cpus, ObjectRange range) noexcept -> std::expected<void, MemErr> {
    PageReq req{thread, cpus};
    auto started = trim(range, req.relation, &req, &PageReq::publish);
    const bool pending = !started && started.error() == MemErr::Pending;
    const auto rc = req.wait(pending ? this : nullptr);
    if (!pending) return started;
    if (rc == WaitRc::Dirty) return std::unexpected(MemErr::Dirty);
    if (rc != WaitRc::Ready) return std::unexpected(MemErr::BackingFailed);
    return {};
}

void Mem::finish_trim() noexcept {
    Paged* backing;
    ObjectRange range;
    {
        sync::Lock guard{lock_};
        if (!trimming_ || trim_walk_ || operations_ != 1 + trim_waiters_.waiters.size() ||
            request_pins_.load<libk::MemoryOrder::Acquire>())
            return;
        trim_walk_ = true;
        range = *trimming_;
        backing = std::get_if<Paged>(&store_->data);
    }
    auto result = backing->trim(range);
    WaitQueue::List batch;
    bool closing;
    {
        sync::Lock guard{lock_};
        batch = trim_waiters_.take();
        trimming_.reset();
        for (auto& a : attachments_)
            a.state_.store<libk::MemoryOrder::Release>(static_cast<u8>(MemLink::State::Attached));
        // Completion must expose the final state before any waiter can resume.
        trim_walk_ = false;
        closing = state_ == MemState::Stopping;
        if (closing) ++operations_;
    }
    if (closing) stop();
    while (!batch.empty()) {
        WaitClaim claim;
        {
            sync::Lock guard{lock_};
            claim = WaitQueue::finish(batch, result ? WaitRc::Ready : WaitRc::Dirty);
        }
        libk_assert(claim.publish());
    }
    drop_page(); // This pin retains storage through all detached callbacks.
}

auto Mem::writeback(usize index) noexcept -> std::expected<void, MemErr> {
    return paged([&](Paged& p) noexcept { return p.writeback(index); }, MemErr::NotBacked);
}

auto Mem::pager_finish(Pager& pager, u64 id, bool fail) noexcept -> std::expected<void, MemErr> {
    return paged([&](Paged& p) noexcept { return p.finish(pager, id, fail); });
}

auto Mem::write(usize offset, libk::Span<const byte> input) noexcept -> std::expected<void, MemErr> {
    const usize within = offset & (page_size - 1);
    if (input.empty() || input.size() > page_size - within || offset >= size() ||
        input.size() > size() - offset)
        return std::unexpected(MemErr::InvalidRange);
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || std::get_if<Anon>(&store_->data) == nullptr ||
            seal_ != SealState::Loadable || !access_.contains(Perm::Write))
            return std::unexpected(MemErr::InvalidAccess);
        if (!attachments_.empty() || operations_ != 0) return std::unexpected(MemErr::Busy);
    }
    auto lease = materialize(offset / page_size);
    if (!lease) return std::unexpected(lease.error());
    {
        sync::Lock guard{lock_};
        // Allocation happened without the lock. Recheck publication and
        // sealing before the bounded copy; our lease is the sole operation.
        if (state_ != MemState::Live || seal_ != SealState::Loadable)
            return std::unexpected(MemErr::InvalidAccess);
        if (!attachments_.empty() || operations_ != 1) return std::unexpected(MemErr::Busy);
        memcpy(pmm_->bytes(lease.value().page().page) + within, input.data(), input.size());
    }
    return {};
}

auto Mem::read(usize offset, libk::Span<byte> output) noexcept -> std::expected<void, MemErr> {
    const auto end = libk::checked_add(offset, output.size());
    if (!end || *end > size()) {
        return std::unexpected(MemErr::InvalidRange);
    }
    if (!access_.contains(Perm::Read)) {
        return std::unexpected(MemErr::InvalidAccess);
    }

    usize copied{};
    while (copied < output.size()) {
        const usize position = offset + copied;
        const usize page_index = position / page_size;
        const usize page_offset = position & (page_size - 1);
        auto lease = materialize(page_index);
        if (!lease) {
            return std::unexpected(lease.error());
        }
        if (lease->page().type != MemoryType::Normal)
            return std::unexpected(MemErr::InvalidMemoryType);
        const usize available = page_size - page_offset;
        const usize remaining = output.size() - copied;
        const usize amount = remaining < available ? remaining : available;
        const byte* const source = pmm_->bytes(lease.value().page().page) + page_offset;
        memcpy(output.data() + copied, source, amount);
        copied += amount;
    }
    return {};
}

auto Mem::attach(MemLink& attachment, Perms access) noexcept -> std::expected<void, MemErr> {
    sync::Lock guard{lock_};
    if (state_ != MemState::Live || trimming_ || trim_walk_ || !valid_perms(access) ||
        !access_.contains(access)) {
        return std::unexpected(MemErr::InvalidState);
    }
    if ((access.contains(Perm::Execute) && seal_ != SealState::Executable) ||
        (access.contains(Perm::Write) && seal_ != SealState::Loadable)) {
        return std::unexpected(MemErr::InvalidAccess);
    }
    libk_assert(!access.contains(Perm::Execute) || content_epoch_.raw != 0);
    if (attachment.owner_ != nullptr ||
        static_cast<MemLink::State>(attachment.state_.load<libk::MemoryOrder::Relaxed>()) !=
            MemLink::State::Idle ||
        attachment.ops_ == nullptr || attachment.ops_->invalidate == nullptr ||
        attachment.ops_->released == nullptr) {
        return std::unexpected(MemErr::AttachmentState);
    }
    attachment.owner_ = this;
    attachment.access_ = access;
    attachment.state_.store<libk::MemoryOrder::Release>(static_cast<u8>(MemLink::State::Attached));
    attachments_.push_back(attachment);
    return {};
}

auto Mem::attachment_count() const noexcept -> usize {
    sync::Lock guard{lock_};
    usize count{};
    for ([[maybe_unused]] const MemLink& attachment : attachments_) {
        ++count;
    }
    return count;
}

auto Mem::detach(MemLink& attachment) noexcept -> bool {
    bool quiescent{};
    {
        sync::Lock guard{lock_};
        if (attachment.owner_ != this) {
            return false;
        }
        const auto current =
            static_cast<MemLink::State>(attachment.state_.load<libk::MemoryOrder::Relaxed>());
        libk_assert(current == MemLink::State::Attached || current == MemLink::State::Invalidating);
        attachments_.erase(attachment);
        attachment.owner_ = nullptr;
        attachment.state_.store<libk::MemoryOrder::SeqCst>(static_cast<u8>(MemLink::State::Detached));
        quiescent = attachment.work_.load<libk::MemoryOrder::SeqCst>() == 0;
    }
    finish_retire();
    return quiescent;
}

// The cleanup token retains object storage until backing work, mappings, and
// leases have drained. The retirement walk itself owns an operation pin: a
// synchronous withdrawal callback must not finish cleanup beneath this walk.
void Mem::retire(object::cleanup&& cleanup) noexcept {
    bool retired{};
    {
        sync::Lock guard{lock_};
        retired = state_ == MemState::Retired;
        if (!retired) {
            if (cleanup) {
                libk_assert(!cleanup_);
                cleanup_ = std::move(cleanup);
            }
            if (state_ == MemState::Stopping) return;
            libk_assert(state_ == MemState::Building || state_ == MemState::Live);
            state_ = MemState::Stopping;
            work_open_.store<libk::MemoryOrder::Release>(false);
            if (trimming_ || trim_walk_) return;
            ++operations_;
        }
    }
    if (retired) {
        if (cleanup) cleanup.complete();
        return;
    }

    stop();
}

// Both retirement and live trim publish one work obligation per actual link.
// Only trim needs an additional Mem pin: retirement already retains its storage.
void Mem::invalidate(ObjectRange range) noexcept {
    for (;;) {
        MemLink* link{};
        MemWork work;
        {
            sync::Lock guard{lock_};
            for (auto& a : attachments_) {
                if (static_cast<MemLink::State>(a.state_.load<libk::MemoryOrder::Relaxed>()) !=
                    MemLink::State::Attached)
                    continue;
                a.state_.store<libk::MemoryOrder::Release>(static_cast<u8>(MemLink::State::Invalidating));
                (void)a.work_.fetch_add<libk::MemoryOrder::Relaxed>(1);
                if (!range.empty()) ++operations_;
                link = &a;
                work = MemWork{a, range.empty() ? nullptr : this, range};
                break;
            }
        }
        if (!link) return;
        link->ops_->invalidate(link->context_, std::move(work));
    }
}

void Mem::stop() noexcept {
    Pin hold{*this};

    invalidate({});
    Paged* stopped{};
    {
        sync::Lock guard{lock_};
        stopped = store_ ? std::get_if<Paged>(&store_->data) : nullptr;
    }
    if (stopped) stopped->stop();
}

void Mem::drop_page() noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(operations_ != 0);
        --operations_;
        if (!trimming_ && !trim_walk_) {
            guard.restore();
            finish_retire();
            return;
        }
    }
    finish_trim();
}

void Mem::release_fault() noexcept { drop_page(); }

auto Mem::cancel_fault(WaitRelation& relation, u64 generation) noexcept -> bool {
    {
        sync::Lock guard{lock_};
        if (relation.request == &trim_waiters_) {
            if (!trim_waiters_.detach(relation, generation)) return false;
            guard.restore();
            drop_page();
            return true;
        }
    }
    Paged* backing{};
    {
        sync::Lock guard{lock_};
        if (store_ == nullptr || state_ == MemState::Retired ||
            std::get_if<Paged>(&store_->data) == nullptr) {
            return false;
        }
        backing = std::get_if<Paged>(&store_->data);
    }
    if (!backing->cancel_fault(relation, generation)) {
        return false;
    }
    drop_page();
    return true;
}

void Mem::request_pin() noexcept { (void)request_pins_.fetch_add<libk::MemoryOrder::AcqRel>(1); }
void Mem::request_drop() noexcept {
    // An active trim owns a pin, so request retirement cannot destroy it here.
    bool trimming;
    {
        sync::Lock guard{lock_};
        trimming = trimming_.has_value();
    }
    finish_retire(true);
    if (trimming) finish_trim();
}
void Mem::cancel_request(Pager::Reply& reply) noexcept {
    Paged* p;
    {
        sync::Lock guard{lock_};
        libk_assert(store_ && reply.mem() == this);
        p = std::get_if<Paged>(&store_->data);
    }
    libk_assert(p);
    p->complete(reply, true);
}

void Mem::finish_transfer(usize page_index, OwnedPage&& page, bool commit) noexcept {
    Anon* backing{};
    {
        sync::Lock guard{lock_};
        libk_assert(operations_ != 0 && store_ != nullptr);
        backing = std::get_if<Anon>(&store_->data);
    }
    Pin hold{*this};
    libk_assert(backing != nullptr);
    const auto result = commit ? backing->commit_transfer(page_index)
                               : backing->restore_transfer(page_index, std::move(page));
    libk_assert(result);
}

void Mem::finish_retire(bool drop_request) noexcept {
    Store* store{};
    OwnedPage storage{};
    object::cleanup cleanup{};
    {
        sync::Lock guard{lock_};
        if (drop_request) libk_assert(request_pins_.fetch_sub<libk::MemoryOrder::AcqRel>(1));
        if (state_ != MemState::Stopping || operations_ != 0 ||
            request_pins_.load<libk::MemoryOrder::Acquire>() != 0 || !attachments_.empty() || releasing_) {
            return;
        }
        releasing_ = true;
        store = std::exchange(store_, nullptr);
        storage = std::move(backing_page_);
    }

    if (store) std::destroy_at(store);
    auto backing_refund = backing_sponsorship_.detach();
    storage.reset();
    backing_refund.complete();

    {
        sync::Lock guard{lock_};
        libk_assert(releasing_);
        releasing_ = false;
        state_ = MemState::Retired;
        cleanup = std::move(cleanup_);
    }

    if (cleanup) cleanup.complete();
}

void Mem::fail_build() noexcept {
    libk_assert(state_ == MemState::Building);
    libk_assert(store_ == nullptr);
    work_open_.store<libk::MemoryOrder::Release>(false);
    state_ = MemState::Retired;
}

} // namespace mm
