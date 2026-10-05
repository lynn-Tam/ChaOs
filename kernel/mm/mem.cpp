#include <expected>
#include <mm/mem.hpp>
#include <task/thread.hpp>

#include <variant>
#include <libk/unique_handle.hpp>

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
        if (!valid_perms(extent.perms)) {
            return std::unexpected(MemErr::InvalidAccess);
        }
        const bool ram = pmm.is_ram(extent.physical);
        if (!pmm.covers(extent.physical)) return std::unexpected(MemErr::NotBacked);
        if (!ram && extent.perms.contains(Perm::Execute)) {
            return std::unexpected(MemErr::NotRam);
        }
        previous_end = *object_end;

        for (usize prior = 0; prior < index; ++prior) {
            if (!extent.physical.intersects(extents[prior].physical)) {
                continue;
            }
            return std::unexpected(MemErr::InvalidRange);
        }

        if (owned && !ram)
            return std::unexpected(MemErr::OwnershipMismatch);
        for (const Page page : extent.physical) {
            if (owned) {
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

    if (owned && owned.page_count() != owned_pages) return std::unexpected(MemErr::OwnershipMismatch);
    return {};
}

// The charge follows the real resident owner. Member order releases the page
// before refunding its reusable capacity, including partial initialization.
template<class Extra>
Cache<Extra>::Cache(Pmm& pmm, Perms perms, Pager* pager, bool priv) noexcept
    : pmm_(&pmm), perms_(perms), rows_(pmm) {
    if constexpr (is_paged) cfg_ = {pager, priv};
}

template<class Extra>
Cache<Extra>::Cache(Cache&& other) noexcept
    : pmm_(other.pmm_), perms_(other.perms_), nodes_(std::move(other.nodes_)),
      rows_(std::move(other.rows_)), cfg_(other.cfg_) {}

template<class Extra>
Cache<Extra>::~Cache() noexcept {
    while (auto* n = nodes_.minimum()) { nodes_.erase(*n); rows_.destroy(*n); }
}

template<class Extra>
auto Cache<Extra>::make(const object::ref<>& payer, usize i) noexcept -> std::expected<Row, MemErr> {
    auto n = rows_.create(payer, i);
    if (!n) return std::unexpected(n.error() == SlabErr::ResourceExhausted
        ? MemErr::ResourceExhausted : MemErr::OutOfMemory);
    return Row{*n, Drop{this}};
}

template<class Extra>
auto Cache<Extra>::charge_page(const object::ref<>& payer) noexcept -> std::expected<resource::Charge, MemErr> {
    auto reserved = resource::acquire(payer, {.memory = page_size});
    if (!reserved) return std::unexpected(reserved.error() == resource::errc::exhausted
        ? MemErr::ResourceExhausted : MemErr::InvalidState);
    return std::move(*reserved);
}

template<class Extra>
auto Cache<Extra>::query(usize i) const noexcept -> ContentState {
    sync::Lock guard{tree_lock_};
    const auto* n = nodes_.find(i);
    if (!n) return ContentState::Zero;
    if (n->resident) return ContentState::Resident;
    if constexpr (is_paged) {
        if (n->failed) return ContentState::Failed;
        return cfg_.pager->active(n->request) ? ContentState::Busy : ContentState::Zero;
    } else return ContentState::Busy;
}

template<class Extra>
auto Cache<Extra>::allocate(const object::ref<>& payer, usize i) noexcept -> std::expected<Frame, MemErr>
    requires (!is_paged) {
    {
        sync::Lock guard{tree_lock_};
        if (auto* n = nodes_.find(i))
            return n->resident ? std::expected<Frame, MemErr>{Frame{n->resident.page(), perms_}}
                               : std::unexpected(MemErr::Busy);
    }
    auto row = make(payer, i);
    if (!row) return std::unexpected(row.error());
    auto fee = charge_page(payer);
    if (!fee) return std::unexpected(fee.error());
    auto page = pmm_->allocate_page();
    if (!page) return std::unexpected(MemErr::OutOfMemory);
    memset(page->bytes(), 0, page_size);
    auto* n = row->get();
    n->resident = std::move(*page);
    n->charge = std::move(*fee);
    sync::Lock guard{tree_lock_};
    if (auto* old = nodes_.find(i))
        return old->resident ? std::expected<Frame, MemErr>{Frame{old->resident.page(), perms_}}
                             : std::unexpected(MemErr::Busy);
    nodes_.insert(*n);
    (void)row->release();
    return Frame{n->resident.page(), perms_};
}

template<class Extra>
auto Cache<Extra>::begin_transfer(usize i) noexcept -> std::expected<OwnedPage, MemErr>
    requires (!is_paged) {
    sync::Lock guard{tree_lock_};
    auto* n = nodes_.find(i);
    if (!n || !n->resident)
        return std::unexpected(!n ? MemErr::NotBacked : MemErr::OwnershipMismatch);
    return std::move(n->resident);
}

template<class Extra>
auto Cache<Extra>::restore_transfer(usize i, OwnedPage&& page) noexcept -> std::expected<void, MemErr>
    requires (!is_paged) {
    sync::Lock guard{tree_lock_};
    auto* n = nodes_.find(i);
    if (!page || !n || n->resident) return std::unexpected(MemErr::OwnershipMismatch);
    n->resident = std::move(page);
    return {};
}

template<class Extra>
auto Cache<Extra>::commit_transfer(usize i) noexcept -> std::expected<void, MemErr>
    requires (!is_paged) {
    Node* n;
    {
        sync::Lock guard{tree_lock_};
        n = nodes_.find(i);
        if (!n || n->resident) return std::unexpected(MemErr::OwnershipMismatch);
        nodes_.erase(*n);
    }
    rows_.destroy(*n);
    return {};
}

template<class Extra>
void Cache<Extra>::stop(Mem& owner) noexcept
    requires is_paged {
    cfg_.pager->cancel(owner);
    Node* first;
    {
        sync::Lock guard{tree_lock_};
        first = nodes_.minimum();
    }
    for (auto* n = first; n; n = nodes_.next(*n)) {
        {
            sync::Lock guard{tree_lock_};
            if (n->resident) continue;
            n->failed = true;
        }
        publish_waiters(*n, WaitRc::Failed);
    }
}

template<class Extra>
void Cache<Extra>::publish_waiters(Node& n, WaitRc rc) noexcept
    requires is_paged {
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

template<class Extra>
auto Cache<Extra>::materialize(Mem& owner, usize index, WaitRelation* relation, void* context,
                               WaitRelation::Publish publish) noexcept -> std::expected<Frame, MemErr> {
    if constexpr (!is_paged) return allocate(owner.payer_, index);
    else {
        bool queued{};
        Row row;
        for (;;) {
            sync::Lock guard{tree_lock_};
            if (!owner.work_open_.load<libk::MemoryOrder::Acquire>())
                return std::unexpected(MemErr::InvalidState);
            auto* n = nodes_.find(index);
            if (!n) {
                if (!row) {
                    guard.restore();
                    auto made = make(owner.payer_, index);
                    if (!made) return std::unexpected(made.error());
                    row = std::move(*made);
                    continue;
                }
                n = row.release();
                n->request.mem = &owner;
                nodes_.insert(*n);
            }
            if (n->resident) {
                return Frame{.page = n->resident.page(), .perms = perms_};
            }
            if (n->failed) return std::unexpected(MemErr::BackingFailed);
            if (relation && !n->waiters.attach(*relation, context, publish))
                return std::unexpected(MemErr::Busy);
            if (!cfg_.pager->active(n->request)) {
                queued = cfg_.pager->enqueue(
                    n->request,
                    {.kind = Pager::Kind::PageIn, .page_index = index, .first = index, .count = 1});
                if (!queued) {
                    n->failed = true;
                    if (relation) libk_assert(n->waiters.detach(*relation, relation->generation));
                    return std::unexpected(MemErr::BackingFailed);
                }
            }
            break;
        }
        if (queued) cfg_.pager->signal();
        return std::unexpected(MemErr::Pending);

    }
}

template<class Extra>
auto Cache<Extra>::cancel_fault(WaitRelation& relation, u64 generation) noexcept -> bool
    requires is_paged {
    sync::Lock guard{tree_lock_};
    return relation.request && relation.request->detach(relation, generation);
}

template<class Extra>
auto Cache<Extra>::supply(Mem& owner, Pager& pager, u64 id, OwnedPage&& page) noexcept
    -> std::expected<void, MemErr>
    requires is_paged {
    if (&pager != cfg_.pager || !page) return std::unexpected(MemErr::OwnershipMismatch);
    auto fee = charge_page(owner.payer_);
    if (!fee) return std::unexpected(fee.error());
    auto reply = pager.reply(id, &owner);
    Node* n;
    if (!reply || reply->req().kind != Pager::Kind::PageIn)
        return std::unexpected(MemErr::OwnershipMismatch);
    {
        sync::Lock guard{tree_lock_};
        n = nodes_.find(reply->req().page_index);
        if (!owner.work_open_.load<libk::MemoryOrder::Acquire>() || n->failed || n->resident)
            return std::unexpected(MemErr::OwnershipMismatch);
        libk_assert(&n->request == reply->request());
        n->resident = std::move(page);
        n->charge = std::move(*fee);
        libk_assert(reply->commit());
    }
    publish_waiters(*n, WaitRc::Ready);
    return {};
}

template<class Extra>
auto Cache<Extra>::finish(Mem& owner, Pager& pager, u64 id, bool fail) noexcept -> std::expected<void, MemErr>
    requires is_paged {
    if (&pager != cfg_.pager) return std::unexpected(MemErr::OwnershipMismatch);
    auto reply = pager.reply(id, &owner);
    if (!reply) return std::unexpected(MemErr::OwnershipMismatch);
    if (!fail && reply->req().kind == Pager::Kind::PageIn) return std::unexpected(MemErr::InvalidState);
    complete(*reply, fail);
    return {};
}

template<class Extra>
void Cache<Extra>::complete(Pager::Reply& reply, bool fail) noexcept
    requires is_paged {
    Node* n;
    const bool page_in = reply.req().kind == Pager::Kind::PageIn;
    {
        sync::Lock guard{tree_lock_};
        n = nodes_.find(reply.req().page_index);
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

template<class Extra>
auto Cache<Extra>::observe_usage(usize index, bool, bool dirty) noexcept -> std::expected<void, MemErr>
    requires is_paged {
    sync::Lock guard{tree_lock_};
    auto* n = nodes_.find(index);
    if (!n) return std::unexpected(MemErr::NotBacked);
    if (!n->resident) return std::unexpected(n->failed ? MemErr::BackingFailed : MemErr::Pending);
    if (dirty && n->usage_epoch == std::numeric_limits<u64>::max())
        return std::unexpected(MemErr::GenerationExhausted);
    if (dirty) n->dirty_epoch = ++n->usage_epoch;
    return {};
}

template<class Extra>
auto Cache<Extra>::writeback(usize index) noexcept -> std::expected<void, MemErr>
    requires is_paged {
    {
        sync::Lock guard{tree_lock_};
        auto* n = nodes_.find(index);
        if (!n) return std::unexpected(MemErr::NotBacked);
        if (cfg_.priv || !n->resident || !n->dirty_epoch || n->write_failed ||
            cfg_.pager->active(n->request))
            return std::unexpected(MemErr::InvalidState);
        if (!writeback_locked(*n)) return std::unexpected(MemErr::BackingFailed);
    }
    cfg_.pager->signal();
    return {};
}

template<class Extra>
auto Cache<Extra>::trim(ObjectRange range) noexcept -> std::expected<void, MemErr>
    requires is_paged {
    {
        sync::Lock guard{tree_lock_};
        for (auto* n = nodes_.minimum(); n; n = nodes_.next(*n)) {
            if (!range.contains(n->index)) continue;
            if (cfg_.pager->active(n->request)) return std::unexpected(MemErr::Busy);
            if (n->dirty_epoch) return std::unexpected(MemErr::Dirty);
        }
    }
    for (auto* n = nodes_.minimum(); n; n = nodes_.next(*n)) {
        if (!range.contains(n->index)) continue;
        OwnedPage page;
        resource::Charge charge;
        {
            sync::Lock guard{tree_lock_};
            page = std::move(n->resident);
            charge = std::move(n->charge);
            n->failed = false;
        }
        page.reset();
        charge.reset();
    }
    return {};
}

template<class Extra>
auto Cache<Extra>::writeback_locked(Node& n) noexcept -> bool
    requires is_paged {
    if (cfg_.pager->enqueue(n.request, {.kind = Pager::Kind::Writeback,
                                    .page_index = n.index,
                                    .first = n.index,
                                    .count = 1,
                                    .dirty_epoch = n.dirty_epoch}))
        return true;
    n.write_failed = true;
    return false;
}

Extents::Extents(Pmm& pmm) noexcept : rows_(pmm) {}
Extents::Extents(Extents&& other) noexcept
    : rows_(std::move(other.rows_)), head_(std::exchange(other.head_, nullptr)),
      owned_(std::move(other.owned_)) {}
Extents::~Extents() noexcept { reset(); }

auto Extents::initialize(const object::ref<>& payer, libk::Span<const Extent> extents,
                         PageGroup&& pages) noexcept -> std::expected<void, MemErr> {
    libk_assert(!head_);
    for (const auto& e : extents) {
        auto row = rows_.create(payer, e);
        if (!row) {
            reset();
            return std::unexpected(row.error() == SlabErr::ResourceExhausted ? MemErr::ResourceExhausted
                                                                             : MemErr::OutOfMemory);
        }
        (*row)->next = head_;
        head_ = *row;
    }
    owned_ = std::move(pages);
    return {};
}

auto Extents::query(usize index) const noexcept -> ContentState {
    return find(index) ? ContentState::Resident : ContentState::Failed;
}

auto Extents::materialize(usize index) const noexcept -> std::expected<Frame, MemErr> {
    const auto* e = find(index);
    if (!e) return std::unexpected(MemErr::NotBacked);
    const auto frame = e->physical.base().checked_add(index - e->object.base());
    libk_assert(frame);
    return Frame{Page{*frame}, e->perms};
}

void Extents::reset() noexcept {
    owned_.reset();
    while (head_) {
        auto* row = std::exchange(head_, head_->next);
        rows_.destroy(*row);
    }
}

auto Extents::find(usize index) const noexcept -> const Extent* {
    for (auto* row = head_; row; row = row->next)
        if (row->extent.object.contains(index)) return &row->extent;
    return nullptr;
}
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

auto Mem::prepare(const object::ref<>& payer, Pmm& pmm, usize bytes, Config config) noexcept
    -> std::expected<Data, MemErr> {
    if (!bytes || bytes % page_size) return std::unexpected(MemErr::InvalidSize);
    Data d{pmm, bytes / page_size};
    if (payer) {
        auto ref = payer.clone();
        if (!ref) return std::unexpected(MemErr::InvalidState);
        d.payer_ = std::move(*ref);
    }
    auto ready = std::visit([&]<class C>(C& c) noexcept -> std::expected<void, MemErr> {
        if constexpr (std::same_as<C, AnonCfg>) {
            if (!valid_perms(c.perms)) return std::unexpected(MemErr::InvalidAccess);
            d.perms_ = c.perms;
            d.store_.emplace(std::in_place_type<Anon>, pmm, c.perms);
            if (c.eager) {
                auto& cache = std::get<Anon>(*d.store_);
                for (usize i = 0; i < d.pages_; ++i) {
                    auto page = cache.allocate(d.payer_, i);
                    if (!page) return std::unexpected(page.error());
                }
            }
        } else if constexpr (std::same_as<C, PagedCfg>) {
            auto pager = c.pager.template as<Pager>();
            if (!pager || !valid_perms(c.perms)) return std::unexpected(MemErr::InvalidState);
            if (pager->get().state() != Pager::State::Open) return std::unexpected(MemErr::AttachmentState);
            d.perms_ = c.perms;
            d.pager_ = std::move(c.pager);
            d.store_.emplace(std::in_place_type<Paged>, pmm, c.perms, &pager->get(), c.priv);
        } else {
            auto checked = validate_extents(pmm, d.pages_, c.extents, c.owned);
            if (!checked) return checked;
            d.store_.emplace(std::in_place_type<Extents>, pmm);
            auto ready = std::get<Extents>(*d.store_).initialize(d.payer_, c.extents, std::move(c.owned));
            if (!ready) return ready;
            u8 bits{};
            for (const auto& e : c.extents) bits |= e.perms.raw();
            d.perms_ = Perms::from_raw(bits);
        }
        return {};
    }, config);
    if (!ready) return std::unexpected(ready.error());
    return d;
}

Mem::Mem(Data&& d) noexcept
    : payer_(std::move(d.payer_)), pmm_(d.pmm_), logical_pages_(d.pages_),
      store_(std::move(d.store_)), perms_(d.perms_), pager_ref_(std::move(d.pager_)) {
    libk_assert(store_ && logical_pages_);
    if (std::holds_alternative<Extents>(*store_) && perms_.contains(Perm::Execute)) {
        seal_ = SealState::Executable;
        content_epoch_ = {1};
    }
}

Mem::~Mem() noexcept {
    if (state_ == MemState::Live) retire();
    libk_assert(state_ == MemState::Retired && !cleanup_ && !releasing_);
    libk_assert(!store_ && !operations_ && attachments_.empty());
}

auto Mem::kind() const noexcept -> BackingKind {
    sync::Lock guard{lock_};
    libk_assert(store_.has_value());
    if (std::get_if<Anon>(&*store_)) return BackingKind::Anonymous;
    if (std::get_if<Paged>(&*store_)) return BackingKind::Pager;
    return BackingKind::Physical;
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
        if (attachment.perms_.contains(Perm::Write)) {
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
        store = &*store_;
    }
    Pin hold{*const_cast<Mem*>(this)};
    const auto result = [&] {
        if (auto* data = std::get_if<Anon>(store)) return data->query(page_index);
        if (auto* data = std::get_if<Paged>(store)) return data->query(page_index);
        return std::get_if<Extents>(store)->query(page_index);
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
        store = &*store_;
    }
    Pin hold{*this};

    auto result = [&]() noexcept -> std::expected<Frame, MemErr> {
        if (auto* data = std::get_if<Paged>(store))
            return data->materialize(*this, page_index, relation, owner, publish);
        if (auto* data = std::get_if<Anon>(store)) return data->materialize(*this, page_index, relation, owner, publish);
        return std::get_if<Extents>(store)->materialize(page_index);
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
            page_index >= logical_pages_ || !store_) {
            return std::unexpected((!attachments_.empty() || operations_ != 0) ? MemErr::Busy
                                                                               : MemErr::InvalidState);
        }
        libk_assert(operations_ != std::numeric_limits<usize>::max());
        ++operations_;
        backing = std::get_if<Anon>(&*store_);
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
            !(backing = std::get_if<Paged>(&*store_)))
            return Result{std::unexpected(error)};
        ++operations_;
    }
    Pin hold{*this};
    return std::forward<F>(fn)(*backing);
}

auto Mem::supply(Pager& pager, u64 id, OwnedPage&& page) noexcept -> std::expected<void, MemErr> {
    return paged([&](Paged& p) noexcept { return p.supply(*this, pager, id, std::move(page)); });
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
            !store_) {
            return std::unexpected(MemErr::InvalidState);
        }
        ++operations_;
        backing = std::get_if<Paged>(&*store_);
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
        if (state_ != MemState::Live || !store_ || !std::get_if<Paged>(&*store_))
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
        backing = std::get_if<Paged>(&*store_);
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
    return paged([&](Paged& p) noexcept { return p.finish(*this, pager, id, fail); });
}

auto Mem::write(usize offset, libk::Span<const byte> input) noexcept -> std::expected<void, MemErr> {
    const usize within = offset & (page_size - 1);
    if (input.empty() || input.size() > page_size - within || offset >= size() ||
        input.size() > size() - offset)
        return std::unexpected(MemErr::InvalidRange);
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || std::get_if<Anon>(&*store_) == nullptr ||
            seal_ != SealState::Loadable || !perms_.contains(Perm::Write))
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
        memcpy(pmm_->bytes(lease.value().page()) + within, input.data(), input.size());
    }
    return {};
}

auto Mem::read(usize offset, libk::Span<byte> output) noexcept -> std::expected<void, MemErr> {
    const auto end = libk::checked_add(offset, output.size());
    if (!end || *end > size()) {
        return std::unexpected(MemErr::InvalidRange);
    }
    if (!perms_.contains(Perm::Read)) {
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
        if (!pmm_->is_ram(lease->page()))
            return std::unexpected(MemErr::NotRam);
        const usize available = page_size - page_offset;
        const usize remaining = output.size() - copied;
        const usize amount = remaining < available ? remaining : available;
        const byte* const source = pmm_->bytes(lease.value().page()) + page_offset;
        memcpy(output.data() + copied, source, amount);
        copied += amount;
    }
    return {};
}

auto Mem::attach(MemLink& attachment, Perms perms) noexcept -> std::expected<void, MemErr> {
    sync::Lock guard{lock_};
    if (state_ != MemState::Live || trimming_ || trim_walk_ || !valid_perms(perms) ||
        !perms_.contains(perms)) {
        return std::unexpected(MemErr::InvalidState);
    }
    if ((perms.contains(Perm::Execute) && seal_ != SealState::Executable) ||
        (perms.contains(Perm::Write) && seal_ != SealState::Loadable)) {
        return std::unexpected(MemErr::InvalidAccess);
    }
    libk_assert(!perms.contains(Perm::Execute) || content_epoch_.raw != 0);
    if (attachment.owner_ != nullptr ||
        static_cast<MemLink::State>(attachment.state_.load<libk::MemoryOrder::Relaxed>()) !=
            MemLink::State::Idle ||
        attachment.ops_ == nullptr || attachment.ops_->invalidate == nullptr ||
        attachment.ops_->released == nullptr) {
        return std::unexpected(MemErr::AttachmentState);
    }
    attachment.owner_ = this;
    attachment.perms_ = perms;
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
            libk_assert(state_ == MemState::Live);
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
        stopped = store_ ? std::get_if<Paged>(&*store_) : nullptr;
    }
    if (stopped) stopped->stop(*this);
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
        if (!store_ || state_ == MemState::Retired ||
            std::get_if<Paged>(&*store_) == nullptr) {
            return false;
        }
        backing = std::get_if<Paged>(&*store_);
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
        p = std::get_if<Paged>(&*store_);
    }
    libk_assert(p);
    p->complete(reply, true);
}

void Mem::finish_transfer(usize page_index, OwnedPage&& page, bool commit) noexcept {
    Anon* backing{};
    {
        sync::Lock guard{lock_};
        libk_assert(operations_ != 0 && store_.has_value());
        backing = std::get_if<Anon>(&*store_);
    }
    Pin hold{*this};
    libk_assert(backing != nullptr);
    const auto result = commit ? backing->commit_transfer(page_index)
                               : backing->restore_transfer(page_index, std::move(page));
    libk_assert(result);
}

void Mem::finish_retire(bool drop_request) noexcept {
    object::cleanup cleanup{};
    {
        sync::Lock guard{lock_};
        if (drop_request) libk_assert(request_pins_.fetch_sub<libk::MemoryOrder::AcqRel>(1));
        if (state_ != MemState::Stopping || operations_ != 0 ||
            request_pins_.load<libk::MemoryOrder::Acquire>() != 0 || !attachments_.empty() || releasing_) {
            return;
        }
        releasing_ = true;
    }

    store_.reset();

    {
        sync::Lock guard{lock_};
        libk_assert(releasing_);
        releasing_ = false;
        state_ = MemState::Retired;
        cleanup = std::move(cleanup_);
    }

    if (cleanup) cleanup.complete();
}

} // namespace mm
