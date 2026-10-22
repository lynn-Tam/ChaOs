#include <algorithm>
#include <cpu/cpu.hpp>
#include <expected>
#include <mm/mem.hpp>
#include <task/thread.hpp>

#include <variant>
#include <libk/unique_handle.hpp>

#include <mm/pager.hpp>
#include <object/ref.hpp>

#include <libk/types.hpp>
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

[[nodiscard]] auto validate_extents(Pmm& pmm, usize logical_pages, std::span<const Extent> extents,
                                    const PageGroup& owned, const Phys::Claim& external) noexcept -> std::expected<void, MemErr> {
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
        if (external) {
            if (ram || owned || !external.covers(extent.physical))
                return std::unexpected(MemErr::OwnershipMismatch);
        } else if (!pmm.covers(extent.physical)) return std::unexpected(MemErr::NotBacked);
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

// The usage follows the real resident owner. Member order releases the page
// before reallocation its reusable capacity, including partial initialization.
template<class Extra>
Cache<Extra>::Cache(Pmm& pmm, Perms perms, Pager* pager, bool priv, usize capacity) noexcept
    : pmm_(&pmm), perms_(perms), rows_(pmm, decltype(rows_)::quota_for(capacity)) {
    if constexpr (is_paged) cfg_ = {pager, priv};
}

template<class Extra>
Cache<Extra>::Cache(Cache&& other) noexcept
    : pmm_(other.pmm_), perms_(other.perms_), nodes_(std::move(other.nodes_)),
      rows_(std::move(other.rows_)), cfg_(other.cfg_) {}

template<class Extra>
Cache<Extra>::~Cache() noexcept {
    while (!drain()) {}
}

template<class Extra>
auto Cache<Extra>::drain() noexcept -> bool {
    usize remaining = 8;
    while (remaining) {
        Node* node;
        {
            sync::Lock guard{tree_lock_};
            node = nodes_.minimum();
            if (!node) break;
        }
        if constexpr (is_paged) {
            node->completion.close();
            if (!node->completion.drained()) return false;
        }
        { sync::Lock guard{tree_lock_}; nodes_.erase(*node); }
        rows_.destroy(*node);
        --remaining;
    }
    { sync::Lock guard{tree_lock_}; if (!nodes_.empty()) return false; }
    return rows_.drain(remaining);
}

template<class Extra>
auto Cache<Extra>::make(const object::ref<>& quota, usize i) noexcept -> std::expected<Row, MemErr> {
    auto n = rows_.create(quota, i);
    if (!n) return std::unexpected((n.error() == SlabErr::ResourceExhausted || n.error() == SlabErr::QuotaExceeded)
        ? MemErr::ResourceExhausted : n.error() == SlabErr::GenerationExhausted
        ? MemErr::GenerationExhausted : MemErr::OutOfMemory);
    return Row{*n, Drop{this}};
}

template<class Extra>
auto Cache<Extra>::charge_page(const object::ref<>& quota) noexcept -> std::expected<resource::Usage, MemErr> {
    auto reserved = resource::acquire(quota, {.memory = page_size});
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
auto Cache<Extra>::allocate(const object::ref<>& quota, usize i) noexcept -> std::expected<Frame, MemErr>
    requires (!is_paged) {
    {
        sync::Lock guard{tree_lock_};
        if (auto* n = nodes_.find(i))
            return n->resident ? std::expected<Frame, MemErr>{Frame{n->resident.page(), perms_, &n->pins}}
                               : std::unexpected(MemErr::Busy);
    }
    auto row = make(quota, i);
    if (!row) return std::unexpected(row.error());
    auto fee = charge_page(quota);
    if (!fee) return std::unexpected(fee.error());
    auto page = pmm_->allocate_page();
    if (!page) return std::unexpected(MemErr::OutOfMemory);
    memset(page->bytes(), 0, page_size);
    auto* n = row->get();
    n->resident = std::move(*page);
    n->usage = std::move(*fee);
    sync::Lock guard{tree_lock_};
    if (auto* old = nodes_.find(i))
        return old->resident ? std::expected<Frame, MemErr>{Frame{old->resident.page(), perms_, &old->pins}}
                             : std::unexpected(MemErr::Busy);
    nodes_.insert(*n);
    (void)row->release();
    return Frame{n->resident.page(), perms_, &n->pins};
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
auto Cache<Extra>::stop(Mem&) noexcept -> bool
    requires is_paged {
    for (usize step = 0; step < 8; ++step) {
        Node* node;
        {
            sync::Lock guard{tree_lock_};
            node = nodes_.lower_bound(cfg_.stop);
            if (!node) return true;
            cfg_.stop = node->index + 1;
            if (!node->resident) node->failed = true;
        }
        // Retirement retains the cache and excludes trim; this indexed node
        // survives both the request cancellation and Source callback.
        cfg_.pager->cancel(node->request);
        node->completion.close();
    }
    sync::Lock guard{tree_lock_};
    return !nodes_.lower_bound(cfg_.stop);
}

template<class Extra>
auto Cache<Extra>::materialize(Mem& owner, usize index, Source::Context* context) noexcept -> std::expected<Frame, MemErr> {
    if constexpr (!is_paged) return allocate(owner.quota_, index);
    else {
        bool queued{};
        Row row;
        for (;;) {
            sync::Lock guard{tree_lock_};
            if (!owner.work_open_.load(std::memory_order_acquire))
                return std::unexpected(MemErr::InvalidState);
            auto* n = nodes_.find(index);
            if (!n) {
                if (!row) {
                    guard.restore();
                    auto made = make(owner.quota_, index);
                    if (!made) return std::unexpected(made.error());
                    row = std::move(*made);
                    row.get()->completion.bind_work(*owner.work_, owner.job_.key(), Source::Drained::bind<&Mem::post>(owner));
                    continue;
                }
                n = row.release();
                n->request.mem = &owner;
                nodes_.insert(*n);
            }
            if (n->resident) {
                return Frame{.page = n->resident.page(), .perms = perms_, .pins = &n->pins};
            }
            if (n->failed) return std::unexpected(MemErr::BackingFailed);
            if (!cfg_.pager->active(n->request)) {
                queued = cfg_.pager->enqueue(
                    n->request,
                    {.kind = Pager::Kind::PageIn, .page_index = index, .first = index, .count = 1});
                if (!queued) {
                    n->failed = true;
                    return std::unexpected(MemErr::BackingFailed);
                }
            }
            if (context) {
                const auto ready = n->completion.await(*context, {.mask = 1}, {}, {},
                    Source::Action::bind<&Mem::drop_page>(owner));
                if (ready.status != STATUS_PENDING)
                    return std::unexpected(ready.status == STATUS_CLOSED ? MemErr::BackingFailed : MemErr::Busy);
            }
            break;
        }
        if (queued) cfg_.pager->signal();
        return std::unexpected(MemErr::Pending);

    }
}

template<class Extra>
auto Cache<Extra>::supply(Mem& owner, Pager& pager, u64 id, OwnedPage&& page,
    cap::Graph::Admission admission, std::span<const object::ref<>*> gates) noexcept
    -> std::expected<void, MemErr>
    requires is_paged {
    if (&pager != cfg_.pager || !page) return std::unexpected(MemErr::OwnershipMismatch);
    auto fee = charge_page(owner.quota_);
    if (!fee) return std::unexpected(fee.error());
    auto reply = pager.reply(id, &owner);
    Node* n;
    Source::Publication publication;
    if (!reply || reply->req().kind != Pager::Kind::PageIn)
        return std::unexpected(MemErr::OwnershipMismatch);
    {
        sync::Lock guard{tree_lock_};
        n = nodes_.find(reply->req().page_index);
        if (!owner.work_open_.load(std::memory_order_acquire) || n->failed || n->resident)
            return std::unexpected(MemErr::OwnershipMismatch);
        libk_assert(&n->request == reply->request());
        auto install = [&] noexcept {
            n->resident = std::move(page);
            n->usage = std::move(*fee);
        };
        if (!reply->commit(admission, gates, Source::Action::bind(install)))
            return std::unexpected(MemErr::InvalidState);
        publication = n->completion.prepare(1, true);
    }

    return {};
}

template<class Extra>
auto Cache<Extra>::finish(Mem& owner, Pager& pager, u64 id, bool fail,
    cap::Graph::Admission admission, std::span<const object::ref<>*> gates) noexcept -> std::expected<void, MemErr>
    requires is_paged {
    if (&pager != cfg_.pager) return std::unexpected(MemErr::OwnershipMismatch);
    auto reply = pager.reply(id, &owner);
    if (!reply) return std::unexpected(MemErr::OwnershipMismatch);
    if (!fail && reply->req().kind == Pager::Kind::PageIn) return std::unexpected(MemErr::InvalidState);
    return complete(*reply, fail, admission, gates);
}

template<class Extra>
auto Cache<Extra>::complete(Pager::Reply& reply, bool fail, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates) noexcept -> std::expected<void, MemErr>
    requires is_paged {
    Node* n;
    Source::Publication publication;
    const bool page_in = reply.req().kind == Pager::Kind::PageIn;
    {
        sync::Lock guard{tree_lock_};
        n = nodes_.find(reply.req().page_index);
        libk_assert(n && &n->request == reply.request());
        auto install = [&] noexcept {
            if (page_in) { libk_assert(fail && !n->resident); n->failed = true; }
            else if (fail) n->write_failed = true;
            else if (n->dirty_epoch == reply.req().dirty_epoch) n->dirty_epoch = 0;
        };
        if (!reply.commit(admission, gates, Source::Action::bind(install), fail))
            return std::unexpected(MemErr::InvalidState);
        if (page_in) publication = n->completion.prepare(1, true);
    }
    return {};
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
auto Cache<Extra>::trim(ObjectRange range, usize& cursor, bool& removing) noexcept
    -> std::expected<bool, MemErr> requires is_paged {
    for (usize step = 0; step < 8; ++step) {
        Node* n;
        {
            sync::Lock guard{tree_lock_};
            n = nodes_.lower_bound(cursor);
            if (!n || !range.contains(n->index)) {
                if (removing) return true;
                removing = true; cursor = range.base();
                return false;
            }
            if (!removing) {
                // Validate the whole range before deleting any page. A wired
                // registration survives trim; transient loans finish normally.
                if (n->pins.wired.load(std::memory_order_acquire)) return std::unexpected(MemErr::Busy);
                if (n->pins.uses.load(std::memory_order_acquire)) return std::unexpected(MemErr::Pending);
                if (cfg_.pager->active(n->request)) return std::unexpected(MemErr::Busy);
                if (n->dirty_epoch) return std::unexpected(MemErr::Dirty);
                cursor = n->index + 1;
                continue;
            }
            cursor = n->index + 1;
        }
        n->completion.close();
        if (!n->completion.drained()) { cursor = n->index; return false; }
        { sync::Lock guard{tree_lock_}; nodes_.erase(*n); }
        // Keep prepaid metadata pages for future faults in this live backing.
        rows_.destroy(*n);
    }
    return false;
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
      owned_(std::move(other.owned_)), external_(std::move(other.external_)) {}
Extents::~Extents() noexcept { reset(); }

auto Extents::initialize(const object::ref<>& quota, std::span<const Extent> extents,
                         PageGroup&& pages, Phys::Claim&& external) noexcept -> std::expected<void, MemErr> {
    libk_assert(!head_);
    for (const auto& e : extents) {
        auto row = rows_.create(quota, e);
        if (!row) {
            reset();
            return std::unexpected(row.error() == SlabErr::ResourceExhausted ? MemErr::ResourceExhausted
                                                                             : MemErr::OutOfMemory);
        }
        (*row)->next = head_;
        head_ = *row;
    }
    owned_ = std::move(pages);
    external_ = std::move(external);
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
    while (!drain()) {}
}

auto Extents::drain() noexcept -> bool {
    usize remaining = 8;
    while (owned_.page_count() && remaining) {
        --remaining;
        auto page = owned_.take(); libk_assert(page); page->reset();
    }
    while (head_ && remaining) {
        --remaining;
        auto* row = std::exchange(head_, head_->next);
        rows_.destroy(*row);
    }
    return !head_ && !owned_.page_count() && rows_.drain(remaining);
}

auto Extents::find(usize index) const noexcept -> const Extent* {
    for (auto* row = head_; row; row = row->next)
        if (row->extent.object.contains(index)) return &row->extent;
    return nullptr;
}
void MemWork::Drop::operator()(Data& d) const noexcept {
    auto* lease = d.lease;
    d.attachment->drop_work(d.final_release);
    if (lease) lease->drop_work();
}

MemLink::~MemLink() noexcept {
    const State current = static_cast<State>(state_.load(std::memory_order_acquire));
    libk_assert(current == State::Idle || current == State::Detached);
    libk_assert(owner_ == nullptr);
    libk_assert(work_.load(std::memory_order_acquire) == 0);
}

auto MemLink::attached() const noexcept -> bool {
    const State current = static_cast<State>(state_.load(std::memory_order_acquire));
    return current == State::Attached || current == State::Invalidating;
}

auto MemLink::busy() const noexcept -> bool { return work_.load(std::memory_order_acquire) != 0; }

auto MemLink::detach() noexcept -> bool {
    Mem* const owner = owner_;
    if (owner == nullptr) {
        return static_cast<State>(state_.load(std::memory_order_acquire)) == State::Detached && !busy();
    }
    return owner->detach(*this);
}

void MemLink::drop_work(bool final_release) noexcept {
    if (final_release) {
        {
            sync::Lock guard{lock_};
            libk_assert(work_.load() == 1 && static_cast<State>(state_.load()) == State::Releasing);
            state_.store(static_cast<u8>(State::Detached), std::memory_order_relaxed);
        }
        work_.store(0, std::memory_order_release); // Last access: storage may now be reused.
        return;
    }
    {
        sync::Lock guard{lock_};
        const auto count = work_.load(std::memory_order_relaxed);
        libk_assert(count);
        if (count != 1 || static_cast<State>(state_.load()) != State::Detached) {
            work_.store(count - 1, std::memory_order_release);
            return;
        }
        state_.store(static_cast<u8>(State::Releasing), std::memory_order_release);
    }
    MemWork done{*this, nullptr, {}, true};
    ops_->released(context_, std::move(done));
}

void PageHold::Drop::operator()(Data& d) const noexcept {
    if (auto* pins = d.frame.pins) {
        if (d.wired) libk_assert(pins->wired.fetch_sub(1, std::memory_order_release));
        libk_assert(pins->uses.fetch_sub(1, std::memory_order_release));
        // Zero permits live trim to destroy the node containing these counters.
    }
    d.owner->drop_page();
}

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

auto Mem::prepare(const object::ref<>& quota, Pmm& pmm, usize bytes, Config config) noexcept
    -> std::expected<Data, MemErr> {
    if (!bytes || bytes % page_size) return std::unexpected(MemErr::InvalidSize);
    Data d{pmm, bytes / page_size};
    d.quota_ = quota;
    auto ready = std::visit([&]<class C>(C& c) noexcept -> std::expected<void, MemErr> {
        if constexpr (std::same_as<C, AnonCfg>) {
            if (!valid_perms(c.perms)) return std::unexpected(MemErr::InvalidAccess);
            d.perms_ = c.perms;
            d.store_.emplace(std::in_place_type<Anon>, pmm, c.perms);
            if (c.eager) {
                auto& cache = std::get<Anon>(*d.store_);
                for (usize i = 0; i < d.pages_; ++i) {
                    auto page = cache.allocate(d.quota_, i);
                    if (!page) return std::unexpected(page.error());
                }
            }
        } else if constexpr (std::same_as<C, PagedCfg>) {
            auto pager = c.pager.template as<Pager>();
            if (!pager || !valid_perms(c.perms)) return std::unexpected(MemErr::InvalidState);
            if (pager->get().state() != Pager::State::Open) return std::unexpected(MemErr::AttachmentState);
            d.perms_ = c.perms;
            d.pager_ = std::move(c.pager);
            if (!c.credits) return std::unexpected(MemErr::InvalidSize);
            const auto credits = std::min(c.credits, d.pages_);
            d.store_.emplace(std::in_place_type<Paged>, pmm, c.perms, &pager->get(), c.priv, credits);
            if (auto result = std::get<Paged>(*d.store_).prepay(d.quota_, credits); !result) return result;
        } else {
            auto checked = validate_extents(pmm, d.pages_, c.extents, c.owned, c.external);
            if (!checked) return checked;
            d.store_.emplace(std::in_place_type<Extents>, pmm);
            auto ready = std::get<Extents>(*d.store_).initialize(d.quota_, c.extents, std::move(c.owned), std::move(c.external));
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
    : quota_(std::move(d.quota_)), pmm_(d.pmm_), logical_pages_(d.pages_),
      store_(std::move(d.store_)), perms_(d.perms_), pager_ref_(std::move(d.pager_)) {
    libk_assert(store_ && logical_pages_);
    if (std::holds_alternative<Extents>(*store_) && perms_.contains(Perm::Execute)) {
        seal_ = SealState::Executable;
        content_epoch_ = {1};
    }
}

Mem::~Mem() noexcept {
    libk_assert(state_ == MemState::Retired && !cleanup_ && !releasing_ && !stopping_ && !scan_);
    libk_assert(!store_ && !operations_ && attachments_.empty());
}

void Mem::links(u64 cursor, LinkRecords& out) const noexcept {
    sync::Lock guard{lock_};
    const auto* physical = store_ ? std::get_if<Extents>(&*store_) : nullptr;
    backing_.links(out, physical ? LINK_PHYS : LINK_USE, cursor);
    const auto range = physical ? physical->external() : Pages{};
    for (auto& row : std::span{out.records, out.count}) { row.first = range.base().raw(); row.bytes = physical ? range.page_count() * page_size : logical_pages_ * page_size; }
}

auto Mem::managed() const noexcept -> bool {
    sync::Lock guard{lock_};
    if (!store_) return false;
    const auto* physical = std::get_if<Extents>(&*store_);
    return !physical || physical->managed();
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

auto Mem::materialize(usize page_index, Source::Context* context, bool wired) noexcept
    -> std::expected<PageHold, MemErr> {
    Store* store{};
    {
        sync::Lock guard{lock_};
        if (reassigning_) return std::unexpected(MemErr::Busy);
        if (state_ != MemState::Live) {
            return std::unexpected(MemErr::InvalidState);
        }
        if (page_index >= logical_pages_) {
            return std::unexpected(MemErr::InvalidRange);
        }
        libk_assert(operations_ != std::numeric_limits<usize>::max());
        if (trimming_) {
            if (!context) return std::unexpected(MemErr::Busy);
            const auto ready = trim_events_.await(*context, {.mask = ~u64{}}, {}, {},
                Source::Action::bind<&Mem::drop_page>(*this));
            if (ready.status != STATUS_PENDING) return std::unexpected(MemErr::Busy);
            ++operations_;
            return std::unexpected(MemErr::Pending);
        }
        ++operations_; ++preparing_;
        store = &*store_;
    }
    Pin hold{*this};
    libk::scope_exit preparation{[&]() noexcept { prepared(); }};

    auto result = [&]() noexcept -> std::expected<Frame, MemErr> {
        if (auto* data = std::get_if<Paged>(store))
            return data->materialize(*this, page_index, context);
        if (auto* data = std::get_if<Anon>(store)) return data->materialize(*this, page_index, context);
        return std::get_if<Extents>(store)->materialize(page_index);
    }();
    const bool retained = context && !result && result.error() == MemErr::Pending;
    bool live{};
    {
        sync::Lock guard{lock_};
        live = state_ == MemState::Live;
    }
    if (retained) {
        hold.release(); // The Thread's fixed Source registration owns this operation.
        return std::unexpected(MemErr::Pending);
    }
    if (!result) {
        return std::unexpected(live ? result.error() : MemErr::InvalidState);
    }
    if (!live) {
        return std::unexpected(MemErr::InvalidState);
    }
    hold.release(); // The installed PTE or caller now owns this operation lease.
    return (PageHold{*this, result.value(), wired});
}

auto Mem::begin_transfer(usize page_index) noexcept -> std::expected<PageTransfer, MemErr> {
    Anon* backing{};
    {
        sync::Lock guard{lock_};
        if (reassigning_) return std::unexpected(MemErr::Busy);
        if (state_ != MemState::Live || !attachments_.empty() || operations_ != 0 ||
            page_index >= logical_pages_ || !store_) {
            return std::unexpected((!attachments_.empty() || operations_ != 0) ? MemErr::Busy
                                                                               : MemErr::InvalidState);
        }
        libk_assert(operations_ != std::numeric_limits<usize>::max());
        ++operations_; ++preparing_;
        backing = std::get_if<Anon>(&*store_);
    }
    Pin hold{*this};
    libk::scope_exit preparation{[&]() noexcept { prepared(); }};
    auto page = backing ? backing->begin_transfer(page_index)
                        : std::expected<OwnedPage, MemErr>{std::unexpected(MemErr::OwnershipMismatch)};
    if (!page) {
        return std::unexpected(page.error());
    }
    (void)preparation.release(); // Transfer completion still owns allocation preparation.
    hold.release(); // The move-only transfer owns the source operation.
    return (PageTransfer{*this, page_index, std::move(page).value()});
}

template <class F> auto Mem::paged(F&& fn, MemErr error) noexcept {
    using Result = decltype(fn(std::declval<Paged&>()));
    Paged* backing{};
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || reassigning_ || trimming_ || !store_ ||
            !(backing = std::get_if<Paged>(&*store_)))
            return Result{std::unexpected(error)};
        ++operations_; ++preparing_;
    }
    Pin hold{*this};
    libk::scope_exit preparation{[&]() noexcept { prepared(); }};
    return std::forward<F>(fn)(*backing);
}

auto Mem::supply(Pager& pager, u64 id, OwnedPage&& page, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates) noexcept -> std::expected<void, MemErr> {
    return paged([&](Paged& p) noexcept { return p.supply(*this, pager, id, std::move(page), admission, gates); });
}

auto Mem::supply(Pager& pager, PageTransfer&& transfer, u64 id, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates) noexcept -> std::expected<void, MemErr> {
    if (!transfer || !transfer.page().valid()) return std::unexpected(MemErr::OwnershipMismatch);
    const auto result = supply(pager, id, std::move(transfer.page_), admission, gates);
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

auto Mem::trim(ObjectRange range, Source::Context& context, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates, const cap::GrantPin* grant) noexcept -> std::expected<void, MemErr> {
    Source::Publication publication;
    auto cpu = context.thread.cpu_domain();
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || !store_ || !std::get_if<Paged>(&*store_))
            return std::unexpected(MemErr::InvalidState);
        if (range.empty() || !ObjectRange{0, logical_pages_}.contains(range))
            return std::unexpected(MemErr::InvalidRange);
        if (reassigning_ || trimming_ || advancing_ || request_pins_.load(std::memory_order_acquire))
            return std::unexpected(MemErr::Busy);
        auto self = object::ref<>::retain_alive(job_.key());
        auto& op = trim_op_.emplace(object::ref<>{}, Source::Action::bind<&Mem::post>(*this),
            Source::Action::bind<&Mem::advance>(*this), &ordinary_);
        op.bind(*work_, job_.key());
        auto install = [&]() noexcept {
            publication = trim_events_.prepare(0, true);
            const auto ready = trim_events_.await(context, {.mask = ~u64{}}, {}, {},
                Source::Action::bind<&Mem::drop_page>(*this));
            libk_assert(ready.status == STATUS_PENDING);
            operations_ += 2;
            trimming_ = range; trim_phase_ = TrimPhase::Invalidate;
            trim_cursor_ = range.base(); trim_removing_ = false; trim_status_ = STATUS_OK;
            scan_ = attachments_.empty() ? nullptr : &attachments_.front();
        };
        // Execution is sponsored by this operation's caller, never permanently
        // by whoever created the transferable memory object.
        if (op.start(std::move(cpu), self, admission, gates, Source::Action::bind(install), grant) != STATUS_OK) {
            trim_op_.reset();
            return std::unexpected(MemErr::Busy);
        }
    }
    publication.publish();
    { sync::Lock guard{lock_}; if (trim_op_) trim_op_->post(); }
    return std::unexpected(MemErr::Pending);
}

void Mem::finish_trim() noexcept {
    sync::Lock guard{lock_};
    if (trim_op_) trim_op_->post();
}

auto Mem::writeback(usize index) noexcept -> std::expected<void, MemErr> {
    return paged([&](Paged& p) noexcept { return p.writeback(index); }, MemErr::NotBacked);
}

auto Mem::pager_finish(Pager& pager, u64 id, bool fail, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates) noexcept -> std::expected<void, MemErr> {
    return paged([&](Paged& p) noexcept { return p.finish(*this, pager, id, fail, admission, gates); });
}

auto Mem::write(usize offset, std::span<const byte> input) noexcept -> std::expected<void, MemErr> {
    const usize within = offset & (page_size - 1);
    if (input.empty() || input.size() > page_size - within || offset >= size() ||
        input.size() > size() - offset)
        return std::unexpected(MemErr::InvalidRange);
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || std::get_if<Anon>(&*store_) == nullptr ||
            seal_ != SealState::Loadable || !perms_.contains(Perm::Write))
            return std::unexpected(MemErr::InvalidAccess);
    }
    auto lease = materialize(offset / page_size);
    if (!lease) return std::unexpected(lease.error());
    {
        sync::Lock guard{lock_};
        // The page lease pins storage; the lock serializes this bounded write
        // with sealing. Read-only mappings do not make mutable RAM immutable.
        if (state_ != MemState::Live || seal_ != SealState::Loadable)
            return std::unexpected(MemErr::InvalidAccess);
        memmove(pmm_->bytes(lease.value().page()) + within, input.data(), input.size());
    }
    return {};
}

auto Mem::read(usize offset, std::span<byte> output) noexcept -> std::expected<void, MemErr> {
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
        memmove(output.data() + copied, source, amount);
        copied += amount;
    }
    return {};
}

auto Mem::attach(MemLink& attachment, Perms perms) noexcept -> std::expected<void, MemErr> {
    sync::Lock guard{lock_};
    if (state_ != MemState::Live || trimming_ || !valid_perms(perms) ||
        !perms_.contains(perms)) {
        return std::unexpected(MemErr::InvalidState);
    }
    if ((perms.contains(Perm::Execute) && seal_ != SealState::Executable) ||
        (perms.contains(Perm::Write) && seal_ != SealState::Loadable)) {
        return std::unexpected(MemErr::InvalidAccess);
    }
    libk_assert(!perms.contains(Perm::Execute) || content_epoch_.raw != 0);
    if (attachment.owner_ != nullptr ||
        static_cast<MemLink::State>(attachment.state_.load(std::memory_order_relaxed)) !=
            MemLink::State::Idle ||
        attachment.ops_ == nullptr || attachment.ops_->invalidate == nullptr ||
        attachment.ops_->released == nullptr) {
        return std::unexpected(MemErr::AttachmentState);
    }
    const auto id = object::new_id<object::Link>();
    if (!id.valid()) return std::unexpected(MemErr::GenerationExhausted);
    attachment.id_ = id; attachment.owner_ = this;
    attachment.perms_ = perms;
    attachment.state_.store(static_cast<u8>(MemLink::State::Attached), std::memory_order_release);
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
            static_cast<MemLink::State>(attachment.state_.load(std::memory_order_relaxed));
        libk_assert(current == MemLink::State::Attached || current == MemLink::State::Invalidating);
        if (scan_ == &attachment) {
            auto next = ++attachments_.iterator_to(attachment);
            scan_ = next == attachments_.end() ? nullptr : &*next;
        }
        attachments_.erase(attachment);
        attachment.owner_ = nullptr;
        sync::Lock work{attachment.lock_};
        attachment.state_.store(static_cast<u8>(MemLink::State::Detached), std::memory_order_release);
        quiescent = attachment.work_.load(std::memory_order_relaxed) == 0;
    }
    finish_retire();
    return quiescent;
}

// The cleanup token retains object storage until backing work, mappings, and
// pins have drained. The retirement walk itself owns an operation lease: a
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
            work_open_.store(false, std::memory_order_release);
            if (trimming_) return;
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
// Only trim needs an additional Mem lease: retirement already retains its storage.
auto Mem::invalidate(ObjectRange range, usize limit) noexcept -> bool {
    for (usize step = 0; step < limit; ++step) {
        MemLink* link;
        MemWork work, running;
        {
            sync::Lock guard{lock_};
            link = scan_;
            if (!link) return true;
            auto next = ++attachments_.iterator_to(*link);
            scan_ = next == attachments_.end() ? nullptr : &*next;
            if (static_cast<MemLink::State>(link->state_.load(std::memory_order_relaxed)) != MemLink::State::Attached)
                continue;
            link->state_.store(static_cast<u8>(MemLink::State::Invalidating), std::memory_order_release);
            { sync::Lock work{link->lock_}; link->work_.fetch_add(2, std::memory_order_relaxed); }
            if (!range.empty()) { operations_ += 2; trim_pending_ += 2; }
            work = MemWork{*link, range.empty() ? nullptr : this, range};
            running = MemWork{*link, range.empty() ? nullptr : this};
        }
        link->ops_->invalidate(link->context_, std::move(work));
    }
    sync::Lock guard{lock_};
    return !scan_;
}

void Mem::stop() noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!stopping_);
        stopping_ = true;
        scan_ = attachments_.empty() ? nullptr : &attachments_.front();
    }
    run_work();
}

void Mem::run_work() noexcept {
    {
        sync::Lock guard{lock_};
        if (trimming_ && trim_op_->schedule()) return;
    }
    advance();
}

void Mem::advance() noexcept {
    // Inline retirement and both worker classes share this single provider step.
    auto execution = object::ref<>::retain_alive(job_.key());
    {
        sync::Lock guard{lock_};
        if (advancing_) { again_ = true; return; }
        advancing_ = true;
    }
    libk::scope_exit leave{[&]() noexcept {
        sync::Lock guard{lock_};
        advancing_ = false;
        if (std::exchange(again_, false)) {
            if (trim_op_) trim_op_->post(); else post();
        }
    }};
    bool stopping, releasing;
    std::optional<ObjectRange> range;
    {
        sync::Lock guard{lock_};
        stopping = stopping_; releasing = releasing_; range = trimming_;
    }
    if (range) {
        auto& op = *trim_op_;
        switch (trim_phase_) {
        case TrimPhase::Invalidate:
            if (op.query().sequence == OP_ACCEPTED && !op.commit()) {
                trim_status_ = STATUS_CANCELED;
                { sync::Lock guard{lock_}; scan_ = nullptr; }
                trim_phase_ = TrimPhase::Finish;
                break;
            }
            if (invalidate(*range, 8)) trim_phase_ = TrimPhase::Wait;
            break;
        case TrimPhase::Wait: {
            sync::Lock guard{lock_};
            if (trim_pending_ || preparing_ || request_pins_.load(std::memory_order_acquire)) return;
            trim_phase_ = TrimPhase::Pages;
            break;
        }
        case TrimPhase::Pages: {
            auto result = std::get<Paged>(*store_).trim(*range, trim_cursor_, trim_removing_);
            if (!result && result.error() == MemErr::Pending) return;
            if (result && !*result) break;
            trim_status_ = result ? STATUS_OK : result.error() == MemErr::Dirty ? STATUS_DIRTY : STATUS_BUSY;
            sync::Lock guard{lock_};
            scan_ = attachments_.empty() ? nullptr : &attachments_.front();
            trim_phase_ = TrimPhase::Restore;
            break;
        }
        case TrimPhase::Restore: {
            sync::Lock guard{lock_};
            for (usize step = 0; step < 8 && scan_; ++step) {
                auto* link = scan_;
                auto next = ++attachments_.iterator_to(*link);
                scan_ = next == attachments_.end() ? nullptr : &*next;
                link->state_.store(static_cast<u8>(MemLink::State::Attached), std::memory_order_release);
            }
            if (!scan_) trim_phase_ = TrimPhase::Finish;
            break;
        }
        case TrimPhase::Finish: {
            if (!op.detach()) return;
            Source::Publication publication;
            bool closing;
            {
                sync::Lock guard{lock_};
                op.finish(trim_status_);
                trim_op_.reset();
                trimming_.reset();
                publication = trim_events_.prepare(trim_status_ == STATUS_OK ? MEM_TRIM_CLEAN
                    : trim_status_ == STATUS_DIRTY ? MEM_TRIM_DIRTY
                    : trim_status_ == STATUS_CANCELED ? MEM_TRIM_CANCELED : MEM_TRIM_BUSY, true);
                closing = state_ == MemState::Stopping;
                if (closing) ++operations_;
            }
            if (closing) stop();
            publication.publish();
            drop_page();
            return;
        }
        }
        op.post();
        return;
    }
    if (releasing) { release(); return; }
    if (!stopping) return;
    if (!invalidate({}, 8)) { post(); return; }
    Paged* backing;
    { sync::Lock guard{lock_}; backing = store_ ? std::get_if<Paged>(&*store_) : nullptr; }
    if (backing && !backing->stop(*this)) { post(); return; }
    trim_events_.close();
    { sync::Lock guard{lock_}; stopping_ = false; }
    drop_page();
}

void Mem::drop_page() noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(operations_ != 0);
        --operations_;
        if (!trimming_) {
            guard.restore();
            finish_retire();
            return;
        }
    }
    finish_trim();
}

void Mem::drop_work() noexcept {
    { sync::Lock guard{lock_}; libk_assert(trim_pending_); --trim_pending_; }
    drop_page();
}

void Mem::request_pin() noexcept { (void)request_pins_.fetch_add(1, std::memory_order_acq_rel); }
void Mem::request_drop() noexcept {
    // An active trim owns a lease, so request retirement cannot destroy it here.
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
    libk_assert(p->complete(reply, true));
}

void Mem::finish_transfer(usize page_index, OwnedPage&& page, bool commit) noexcept {
    Anon* backing{};
    {
        sync::Lock guard{lock_};
        libk_assert(operations_ != 0 && store_.has_value());
        backing = std::get_if<Anon>(&*store_);
    }
    Pin hold{*this};
    libk::scope_exit preparation{[&]() noexcept { prepared(); }};
    libk_assert(backing != nullptr);
    const auto result = commit ? backing->commit_transfer(page_index)
                               : backing->restore_transfer(page_index, std::move(page));
    libk_assert(result);
}

void Mem::finish_retire(bool drop_request) noexcept {
    {
        sync::Lock guard{lock_};
        if (drop_request) libk_assert(request_pins_.fetch_sub(1, std::memory_order_acq_rel));
        if (state_ != MemState::Stopping || operations_ != 0 || preparing_ || reassigning_ ||
            request_pins_.load(std::memory_order_acquire) != 0 || !attachments_.empty() || releasing_) {
            return;
        }
        releasing_ = true;
    }
    run_work();
}

void Mem::release() noexcept {
    if (!trim_events_.drained()) return;
    if (store_) {
        if (!std::visit([](auto& store) noexcept { return store.drain(); }, *store_)) {
            libk_assert(work_); work_->post(job_); return;
        }
        store_.reset(); pager_ref_.reset();
    }
    if (!backing_.close()) return;

    object::cleanup cleanup;
    {
        sync::Lock guard{lock_};
        libk_assert(releasing_);
        releasing_ = false;
        state_ = MemState::Retired;
        cleanup = std::move(cleanup_);
    }

    if (work_) work_->close(job_);
    if (cleanup) cleanup.complete();
}

} // namespace mm
