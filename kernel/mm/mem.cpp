#include <mm/slab.hpp>
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
Cache::Cache(Pmm& pmm, Perms perms, Pager* pager, bool local) noexcept
    : pmm_(&pmm), perms_(perms), rows_(pmm), cfg_{pager, local} {}

Cache::Cache(Cache&& other) noexcept
    : source_(std::move(other.source_)), source_first_(other.source_first_), pmm_(other.pmm_), perms_(other.perms_), nodes_(std::move(other.nodes_)),
      rows_(std::move(other.rows_)), cfg_(other.cfg_), capacity_epoch_(other.capacity_epoch_), capacity_(std::exchange(other.capacity_, nullptr)), done_(std::exchange(other.done_, nullptr)) {}

Cache::~Cache() noexcept {
    while (!drain()) {}
}

auto Cache::drain() noexcept -> bool {
    usize remaining = 8;
    while (remaining) {
        Node* node;
        {
            sync::Lock guard{tree_lock_};
            node = nodes_.minimum();
            if (!node) break;
        }
        if (cfg_.pager) {
            node->completion.close();
            if (!node->completion.drained()) return false;
        }
        { sync::Lock guard{tree_lock_}; nodes_.erase(*node); }
        rows_.destroy(*node);
        --remaining;
    }
    { sync::Lock guard{tree_lock_}; if (!nodes_.empty()) return false; }
    if (capacity_) {
        capacity_->completion.close();
        if (!capacity_->completion.drained()) return false;
        rows_.destroy(*capacity_); capacity_ = nullptr;
    }
    if (done_) {
        done_->completion.close();
        if (!done_->completion.drained()) return false;
        rows_.destroy(*done_); done_ = nullptr;
    }
    return rows_.drain(remaining);
}

auto Cache::prepay(const object::ref<>& quota, usize count) noexcept -> std::expected<bool, MemErr> {
    auto result = rows_.prepay(quota, count + 2);
    if (!result) return std::unexpected(MemErr::ResourceExhausted);
    if (!*result) return false;
    if (!capacity_) {
        auto row = make(quota, ~usize{});
        if (!row) return std::unexpected(row.error());
        capacity_ = row->release();
        if (owner_) {
            capacity_->request.mem = owner_;
            capacity_->completion.bind_work(*owner_->work_, owner_->job_.key(), Source::Drained::bind<&Mem::post>(*owner_));
        }
    }
    if (!done_) {
        auto row = make(quota, ~usize{} - 1);
        if (!row) return std::unexpected(row.error());
        done_ = row->release();
        if (owner_) {
            done_->request.mem = owner_;
            done_->completion.bind_work(*owner_->work_, owner_->job_.key(), Source::Drained::bind<&Mem::post>(*owner_));
        }
    }
    return true;
}

void Cache::completed(Mem& owner, usize page, status_t status, usize released) noexcept {
    if (!done_ || !cfg_.pager) return;
    done_->request.mem = &owner;
    (void)cfg_.pager->enqueue(done_->request, {.kind=Pager::Kind::Reclaimed, .page_index=page, .count=released, .status=status});
}

auto Cache::make(const object::ref<>& quota, usize i) noexcept -> std::expected<Row, MemErr> {
    auto n = rows_.create(quota, i);
    if (!n) return std::unexpected((n.error() == SlabErr::ResourceExhausted || n.error() == SlabErr::QuotaExceeded)
        ? MemErr::ResourceExhausted : n.error() == SlabErr::GenerationExhausted
        ? MemErr::GenerationExhausted : MemErr::OutOfMemory);
    return Row{*n, Drop{this}};
}

auto Cache::charge_page(const object::ref<>& quota) noexcept -> std::expected<resource::Usage, MemErr> {
    auto reserved = resource::acquire(quota, {.memory = page_size});
    if (!reserved) return std::unexpected(reserved.error() == resource::errc::exhausted
        ? MemErr::ResourceExhausted : MemErr::InvalidState);
    return std::move(*reserved);
}

auto Cache::query(usize i) const noexcept -> ContentState {
    sync::Lock guard{tree_lock_};
    const auto* n = nodes_.find(i);
    if (!n) return ContentState::Zero;
    if (n->present()) return ContentState::Resident;
    if (cfg_.pager) {
        if (n->failed) return ContentState::Failed;
        return cfg_.pager->active(n->request) ? ContentState::Busy : ContentState::Zero;
    } else return ContentState::Busy;
}

auto Cache::allocate(const object::ref<>& quota, usize i) noexcept -> std::expected<Frame, MemErr>
{
    {
        sync::Lock guard{tree_lock_};
        if (auto* n = nodes_.find(i))
            return n->resident ? std::expected<Frame, MemErr>{frame(*n)}
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
    if (owner_) {
        n->request.mem = owner_;
        n->completion.bind_work(*owner_->work_, owner_->job_.key(), Source::Drained::bind<&Mem::post>(*owner_));
    }
    n->resident = std::move(*page);
    n->usage = std::move(*fee);
    sync::Lock guard{tree_lock_};
    if (auto* old = nodes_.find(i))
        return old->resident ? std::expected<Frame, MemErr>{frame(*old)}
                             : std::unexpected(MemErr::Busy);
    nodes_.insert(*n);
    if (capacity_) capacity_->failed = false;
    (void)row->release();
    return frame(*n);
}

auto Cache::begin_transfer(usize i) noexcept -> std::expected<OwnedPage, MemErr>
{
    sync::Lock guard{tree_lock_};
    auto* n = nodes_.find(i);
    if (!n || !n->resident)
        return std::unexpected(!n ? MemErr::NotBacked : MemErr::OwnershipMismatch);
    return std::move(n->resident);
}

auto Cache::restore_transfer(usize i, OwnedPage&& page) noexcept -> std::expected<void, MemErr>
{
    sync::Lock guard{tree_lock_};
    auto* n = nodes_.find(i);
    if (!page || !n || n->resident) return std::unexpected(MemErr::OwnershipMismatch);
    n->resident = std::move(page);
    return {};
}

auto Cache::commit_transfer(usize i) noexcept -> std::expected<void, MemErr>
{
    Node* n;
    {
        sync::Lock guard{tree_lock_};
        n = nodes_.find(i);
        if (!n || n->resident) return std::unexpected(MemErr::OwnershipMismatch);
        if (n->buffered) return {};
        nodes_.erase(*n);
    }
    rows_.destroy(*n);
    return {};
}

auto Cache::buffer(usize index, bool refill) noexcept -> std::expected<void, MemErr> {
    sync::Lock guard{tree_lock_};
    auto* n = nodes_.find(index);
    if (!n || cfg_.pager || source_ || (refill && !n->buffered)) return std::unexpected(MemErr::InvalidState);
    if (!refill) {
        if (!n->resident) return std::unexpected(MemErr::NotBacked);
        n->buffered = true; return {};
    }
    if (n->resident) return {};
    auto page = pmm_->allocate_reserved();
    if (!page) return std::unexpected(MemErr::OutOfMemory);
    memset(page->bytes(), 0, page_size); n->resident = std::move(*page); return {};
}
auto Mem::buffer(usize page, bool refill) noexcept -> std::expected<void, MemErr> {
    sync::Lock guard{lock_};
    if (state_ != MemState::Live || reassigning_ || preparing_ || operations_ || !attachments_.empty()) return std::unexpected(MemErr::Busy);
    auto* cache = store_ ? std::get_if<Cache>(&*store_) : nullptr;
    if (!cache || pager_ref_) return std::unexpected(MemErr::InvalidState);
    return cache->buffer(page, refill);
}

auto Cache::stop(Mem& owner) noexcept -> bool
{
    if (capacity_) {
        cfg_.pager->cancel(capacity_->request);
        capacity_->completion.close();
    }
    if (done_) {
        cfg_.pager->cancel(done_->request);
        done_->completion.close();
    }
    (void)owner;
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

auto Cache::materialize(Mem& owner, usize index, Source::Context* context) noexcept -> std::expected<Frame, MemErr> {
    bool local;
    { sync::Lock guard{tree_lock_}; local = cfg_.local; }
    if ((!cfg_.pager || local) && source_) return borrow(owner, index, context);
    if (!cfg_.pager || local) {
        auto r = allocate(owner.quota_, index);
        if (!r && cfg_.pager && (r.error() == MemErr::ResourceExhausted || r.error() == MemErr::OutOfMemory))
            return capacity(owner, index, context, r.error());
        return r;
    }
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
                    if (!made) return capacity(owner, index, context, made.error());
                    row = std::move(*made);
                    row.get()->completion.bind_work(*owner.work_, owner.job_.key(), Source::Drained::bind<&Mem::post>(owner));
                    continue;
                }
                n = row.release();
                n->request.mem = &owner;
                nodes_.insert(*n);
            }
            if (n->present()) {
                return frame(*n);
            }
            if (n->failed) return std::unexpected(MemErr::BackingFailed);
            if (!cfg_.pager->active(n->request)) {
                // A declined speculative fill leaves this reusable row absent.
                (void)n->completion.update(0);
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

auto Cache::capacity(Mem& owner, usize index, Source::Context* context, MemErr error, std::optional<u64> quota) noexcept
    -> std::expected<Frame, MemErr> {
    if (!capacity_ || !cfg_.pager) return std::unexpected(error);
    bool queued{};
    {
        sync::Lock guard{tree_lock_};
        const auto affected = error == MemErr::OutOfMemory ? 0 : quota.value_or(owner.quota_.id().value);
        if (capacity_->failed && capacity_->request.info.quota == affected) return std::unexpected(error);
        if (!cfg_.pager->active(capacity_->request)) {
            (void)capacity_->completion.update(0);
            capacity_->failed = false;
            queued = cfg_.pager->enqueue(capacity_->request, {.kind=Pager::Kind::Capacity, .page_index=index, .quota=affected});
            if (!queued) return std::unexpected(MemErr::BackingFailed);
        }
        if (context) {
            auto ready = capacity_->completion.await(*context, {.mask=SOURCE_READY}, {}, {},
                Source::Action::bind<&Mem::drop_page>(owner));
            if (ready.status != STATUS_PENDING) return std::unexpected(error);
        }
    }
    if (queued) cfg_.pager->signal();
    return std::unexpected(context ? MemErr::Pending : error);
}

auto Cache::supply(Mem& owner, Pager& pager, u64 id, OwnedPage&& page,
    cap::Graph::Admission admission, std::span<const object::ref<>*> gates, std::optional<Page> copy, bool reclaim) noexcept
    -> std::expected<void, MemErr>
{
    if (&pager != cfg_.pager || (!page && !copy)) return std::unexpected(MemErr::OwnershipMismatch);
    auto reply = pager.reply(id, &owner);
    if (!reply || reply->req().kind != Pager::Kind::PageIn) return std::unexpected(MemErr::OwnershipMismatch);
    return install(owner, *reply, std::move(page), admission, gates, copy, reclaim);
}

auto Cache::install(Mem& owner, Pager::Reply& reply, OwnedPage&& page,
    cap::Graph::Admission admission, std::span<const object::ref<>*> gates, std::optional<Page> copy, bool reclaim) noexcept -> std::expected<void, MemErr> {
    auto fee = charge_page(owner.quota_);
    if (!fee) return std::unexpected(reclaim ? capacity(owner, reply.req().page_index, nullptr, fee.error()).error() : fee.error());
    if (!page) {
        auto allocated = pmm_->allocate_page();
        if (!allocated) return std::unexpected(reclaim ? capacity(owner, reply.req().page_index, nullptr, MemErr::OutOfMemory).error() : MemErr::OutOfMemory);
        page = std::move(*allocated);
        if (copy) memcpy(page.bytes(), pmm_->bytes(*copy), page_size);
        else memset(page.bytes(), 0, page_size);
    }
    Source::Publication publication;
    {
        sync::Lock guard{tree_lock_};
        auto* n = nodes_.find(reply.req().page_index);
        if (!owner.work_open_.load(std::memory_order_acquire) || n->failed || n->resident)
            return std::unexpected(MemErr::OwnershipMismatch);
        libk_assert(&n->request == reply.request());
        auto assign = [&] noexcept { n->resident = std::move(page); n->usage = std::move(*fee); };
        if (!reply.commit(admission, gates, Source::Action::bind(assign))) return std::unexpected(MemErr::InvalidState);
        publication = n->completion.prepare(1, true);
    }
    return {};
}

auto Cache::finish(Mem& owner, Pager& pager, u64 id, bool fail,
    cap::Graph::Admission admission, std::span<const object::ref<>*> gates, bool retry) noexcept -> std::expected<u32, MemErr>
{
    if (&pager != cfg_.pager) return std::unexpected(MemErr::OwnershipMismatch);
    auto reply = pager.reply(id, &owner);
    if (!reply) return std::unexpected(MemErr::OwnershipMismatch);
    if (retry && (!fail || reply->req().kind != Pager::Kind::PageIn)) return std::unexpected(MemErr::InvalidAccess);
    if (reply->req().kind == Pager::Kind::Capacity || reply->req().kind == Pager::Kind::Reclaimed) return complete(*reply, fail, admission, gates);
    if (!fail && reply->req().kind == Pager::Kind::PageIn && source_) {
        auto src = source_.as<Mem>();
        auto borrowed = src->get().materialize(source_first_ + reply->req().page_index);
        if (!borrowed) return std::unexpected(borrowed.error());
        Source::Publication publication;
        {
            sync::Lock guard{tree_lock_};
            auto* n = nodes_.find(reply->req().page_index);
            auto install = [&] noexcept { n->borrowed = std::move(*borrowed); };
            if (!reply->commit(admission, gates, Source::Action::bind(install))) return std::unexpected(MemErr::InvalidState);
            publication = n->completion.prepare(1, true);
        }
        return PAGER_RESIDENT;
    }
    if (!fail && reply->req().kind == Pager::Kind::PageIn) {
        auto result = install(owner, *reply, {}, admission, gates);
        return result ? std::expected<u32, MemErr>{PAGER_RESIDENT} : std::unexpected(result.error());
    }
    return complete(*reply, fail, admission, gates, retry);
}

auto Cache::complete(Pager::Reply& reply, bool fail, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates, bool retry) noexcept -> std::expected<u32, MemErr>
{
    if (reply.req().kind == Pager::Kind::Reclaimed) {
        if (!reply.commit(admission, gates, {}, fail)) return std::unexpected(MemErr::InvalidState);
        return 0;
    }
    if (reply.req().kind == Pager::Kind::Capacity) {
        Source::Publication publication;
        {
            sync::Lock guard{tree_lock_};
            auto done = [&] noexcept { capacity_->failed = fail; if (!fail) ++capacity_epoch_; };
            if (!reply.commit(admission, gates, Source::Action::bind(done), fail)) return std::unexpected(MemErr::InvalidState);
            publication = capacity_->completion.prepare(SOURCE_READY, true);
        }
        return 0;
    }
    Node* n;
    u32 state{};
    Source::Publication publication;
    const bool page_in = reply.req().kind == Pager::Kind::PageIn;
    {
        sync::Lock guard{tree_lock_};
        n = nodes_.find(reply.req().page_index);
        libk_assert(n && &n->request == reply.request());
        auto install = [&] noexcept {
            if (page_in) { libk_assert(fail && !n->resident); n->failed = !retry; }
            else if (fail) n->dirty_epoch = n->usage_epoch ? n->usage_epoch : ++n->usage_epoch;
            else if (n->usage_epoch == reply.req().dirty_epoch) n->dirty_epoch = 0;
            if (!page_in) n->snapshot = false;
            state = (n->present() ? PAGER_RESIDENT : 0) | (n->dirty_epoch ? PAGER_DIRTY : 0);
        };
        if (!reply.commit(admission, gates, Source::Action::bind(install), fail))
            return std::unexpected(MemErr::InvalidState);
        if (page_in) publication = n->completion.prepare(1, true);
    }
    return state;
}

auto Cache::observe_usage(usize index, bool accessed, bool dirty) noexcept -> std::expected<void, MemErr>
{
    sync::Lock guard{tree_lock_};
    auto* n = nodes_.find(index);
    if (!n) return std::unexpected(MemErr::NotBacked);
    if (!n->present()) return std::unexpected(n->failed ? MemErr::BackingFailed : MemErr::Pending);
    if (dirty && n->usage_epoch == std::numeric_limits<u64>::max())
        return std::unexpected(MemErr::GenerationExhausted);
    n->accessed |= accessed;
    if (dirty && !n->dirty_epoch) n->dirty_epoch = ++n->usage_epoch;
    return {};
}


auto Cache::trim(ObjectRange range, usize& cursor, bool& removing, usize& released) noexcept
    -> std::expected<bool, MemErr> {
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
                if (n->buffered || n->pins.wired.load(std::memory_order_acquire)) return std::unexpected(MemErr::Busy);
                if (n->pins.uses.load(std::memory_order_acquire)) return std::unexpected(MemErr::Busy);
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
        // Borrowed content and reusable records are not released data frames.
        const bool owned = bool(n->resident);
        rows_.destroy(*n);
        released += owned;
    }
    return false;
}

auto Cache::writeback_locked(Node& n) noexcept -> bool
{
    if (cfg_.pager->enqueue(n.request, {.kind = Pager::Kind::Writeback,
                                    .page_index = n.index,
                                    .first = n.index,
                                    .count = 1,
                                    .dirty_epoch = n.dirty_epoch}))
        return true;
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
            d.store_.emplace(std::in_place_type<Cache>, pmm, c.perms);
            if (c.eager) {
                auto& cache = std::get<Cache>(*d.store_);
                for (usize i = 0; i < d.pages_; ++i) {
                    auto page = cache.allocate(d.quota_, i);
                    if (!page) return std::unexpected(page.error());
                }
            }
        } else if constexpr (std::same_as<C, CopyCfg>) {
            auto source = c.source.template as<Mem>();
            if (!source || source->get().seal_state() != SealState::Executable || source->get().private_pages() || source->get().kind() == BackingKind::Physical
                || !ObjectRange{c.first, d.pages_}.within(source->get().page_count()))
                return std::unexpected(MemErr::InvalidState);
            d.perms_ = Perms::of(Perm::Read, Perm::Write);
            d.pager_ = std::move(c.pager);
            auto* pager = d.pager_ ? static_cast<Pager*>(d.pager_.get()) : nullptr;
            d.store_.emplace(std::in_place_type<Cache>, pmm, d.perms_, pager, true);
            auto& cache = std::get<Cache>(*d.store_);
            cache.source_ = std::move(c.source); cache.source_first_ = c.first;
            if (pager) {
                auto ready = cache.prepay(d.quota_, std::min(usize{64}, d.pages_));
                if (!ready) return std::unexpected(ready.error());
                if (!*ready) d.prepay_ = std::min(usize{64}, d.pages_);
            }
        } else if constexpr (std::same_as<C, PagedCfg>) {
            auto pager = c.pager.template as<Pager>();
            if (!pager || !valid_perms(c.perms)) return std::unexpected(MemErr::InvalidState);
            if (pager->get().state() != Pager::State::Open) return std::unexpected(MemErr::AttachmentState);
            d.perms_ = c.perms;
            d.pager_ = std::move(c.pager);
            if (!c.credits) return std::unexpected(MemErr::InvalidSize);
            const auto credits = std::min(c.credits, d.pages_);
            d.store_.emplace(std::in_place_type<Cache>, pmm, c.perms, &pager->get(), c.local);
            const auto result = std::get<Cache>(*d.store_).prepay(d.quota_, credits);
            if (!result) return std::unexpected(result.error());
            if (!*result) d.prepay_ = credits;
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
    if (d.pager_) {
        auto reservation = static_cast<Pager*>(d.pager_.get())->reserve(bytes);
        if (!reservation) return std::unexpected(MemErr::ResourceExhausted);
        d.reservation_ = std::move(*reservation);
    }
    return d;
}

Mem::Mem(Data&& d) noexcept
    : quota_(std::move(d.quota_)), pmm_(d.pmm_), logical_pages_(d.pages_), prepay_(d.prepay_),
      store_(std::move(d.store_)), perms_(d.perms_), pager_ref_(std::move(d.pager_)), reservation_(std::move(d.reservation_)) {
    libk_assert(store_ && logical_pages_);
    if (prepay_) work_open_.store(false, std::memory_order_relaxed);
    else { initialization_.accept(); initialization_.finish(); }
    if (std::holds_alternative<Extents>(*store_) && perms_.contains(Perm::Execute)) {
        seal_ = SealState::Executable;
        content_epoch_ = {1};
    }
}

void Cache::bind(Mem& owner, WorkQueue& work, object::Key key) noexcept {
    owner_ = &owner;
    auto bind = [&](Node& n) {
        n.request.mem = &owner;
        n.completion.bind_work(work, key, Source::Drained::bind<&Mem::post>(owner));
    };
    if (capacity_) bind(*capacity_);
    if (done_) bind(*done_);
    for (auto* n = nodes_.minimum(); n; n = nodes_.next(*n)) bind(*n);
}
void Mem::bind_work(WorkQueue& work, object::Key key) noexcept {
    work_ = &work; initialization_.bind(work, key); job_.bind(key); ordinary_.bind(key); backing_.bind(key);
    trim_events_.bind_work(work, key, Source::Drained::bind<&Mem::post>(*this));
    auto self = object::ref<>::retain_alive(key);
    binding_.mem = this; binding_.storage = key; binding_.key = self.id().value;
    if (auto* c = std::get_if<Cache>(&*store_)) c->bind(*this, work, key);
    if (pager_ref_) static_cast<Pager*>(pager_ref_.get())->attach(binding_);
}
auto Cache::bind_pager(Pager& pager) noexcept -> std::expected<void, MemErr> {
    if (source_) return std::unexpected(MemErr::InvalidState);
    auto ready = prepay(owner_->quota_, 1);
    if (!ready || !*ready) return std::unexpected(MemErr::OutOfMemory);
    sync::Lock guard{tree_lock_};
    if (cfg_.pager) return std::unexpected(MemErr::InvalidState);
    for (auto* n = nodes_.minimum(); n; n = nodes_.next(*n))
        if (n->buffered || n->pins.wired.load(std::memory_order_acquire)) return std::unexpected(MemErr::Busy);
    cfg_ = {&pager, true};
    for (auto* n = nodes_.minimum(); n; n = nodes_.next(*n))
        n->dirty_epoch = ++n->usage_epoch;
    return {};
}
auto Mem::bind_pager(object::ref<> ref, const cap::GrantPin* grant, std::span<const object::ref<>*> gates) noexcept -> std::expected<void, MemErr> {
    auto* pager = ref ? static_cast<Pager*>(ref.get()) : nullptr;
    if (!pager || pager->state() != Pager::State::Open) return std::unexpected(MemErr::InvalidState);
    sync::Lock guard{lock_};
    if (state_ != MemState::Live || reassigning_ || preparing_ || trimming_ || pager_ref_)
        return std::unexpected(MemErr::Busy);
    auto* c = store_ ? std::get_if<Cache>(&*store_) : nullptr;
    if (!c) return std::unexpected(MemErr::InvalidState);
    for (auto* link = attachments_.minimum(); link; link = attachments_.next(*link)) if (link->ops_->wired) return std::unexpected(MemErr::Busy);
    auto reservation = pager->reserve(size());
    if (!reservation) return std::unexpected(MemErr::ResourceExhausted);
    if (auto r = c->bind_pager(*pager); !r) return r;
    if (grant && backing_.attach(std::span{&grant, 1}, gates) != STATUS_OK) {
        sync::Lock content{c->tree_lock_}; c->cfg_ = {}; return std::unexpected(MemErr::InvalidState);
    }
    reservation_ = std::move(*reservation);
    pager_ref_ = std::move(ref);
    pager->attach(binding_);
    return {};
}
auto Cache::writable(usize index) const noexcept -> bool {
    sync::Lock guard{tree_lock_};
    auto* n = nodes_.find(index);
    return n && n->resident && (!cfg_.pager || n->dirty_epoch);
}
auto Mem::writable(usize index) const noexcept -> bool {
    sync::Lock guard{lock_};
    auto* c = store_ ? std::get_if<Cache>(&*store_) : nullptr;
    return c ? c->writable(index) : perms_.contains(Perm::Write);
}
auto Mem::dirty(usize index) noexcept -> std::expected<void, MemErr> {
    sync::Lock guard{lock_};
    if (state_ != MemState::Live || !store_) return std::unexpected(MemErr::InvalidState);
    if (trimming_ && trimming_->contains(index)) return std::unexpected(MemErr::Busy);
    auto* c = std::get_if<Cache>(&*store_);
    return c && c->paged() ? c->observe_usage(index, true, true) : std::expected<void, MemErr>{};
}
auto Mem::private_pages() const noexcept -> bool {
    sync::Lock guard{lock_};
    auto* c = store_ ? std::get_if<Cache>(&*store_) : nullptr;
    return c && bool(c->source_);
}
auto Mem::needs_copy(usize index) const noexcept -> bool {
    sync::Lock guard{lock_};
    auto* c = store_ ? std::get_if<Cache>(&*store_) : nullptr;
    if (!c || !c->source_) return false;
    sync::Lock contents{c->tree_lock_};
    auto* n = c->nodes_.find(index);
    return n && bool(n->borrowed);
}
auto Cache::borrow(Mem& owner, usize index, Source::Context* context) noexcept -> std::expected<Frame, MemErr> {
    {
        sync::Lock guard{tree_lock_};
        if (auto* n = nodes_.find(index); n && n->present()) return frame(*n);
    }
    auto source = source_.as<Mem>();
    auto page = source->get().materialize(source_first_ + index, context);
    if (!page) return std::unexpected(context && page.error() == MemErr::Pending ? MemErr::Forwarded : page.error());
    auto row = make(owner.quota_, index);
    if (!row) return capacity(owner, index, context, row.error());
    auto* n = row->get();
    n->request.mem = &owner;
    n->completion.bind_work(*owner.work_, owner.job_.key(), Source::Drained::bind<&Mem::post>(owner));
    n->borrowed = std::move(*page);
    sync::Lock guard{tree_lock_};
    if (auto* old = nodes_.find(index)) return frame(*old);
    nodes_.insert(*n); (void)row->release();
    return frame(*n);
}
auto Cache::copy(usize index, const object::ref<>& quota) noexcept -> std::expected<void, MemErr> {
    Node* n;
    {
        sync::Lock guard{tree_lock_};
        n = nodes_.find(index);
        if (!n || !n->borrowed || n->pins.uses.load(std::memory_order_acquire))
            return std::unexpected(MemErr::Busy);
    }
    auto fee = charge_page(quota);
    auto page = fee ? pmm_->allocate_page() : Pmm::AllocateResult{std::unexpected(AllocError::NoMemory)};
    if (!page) {
        sync::Lock guard{tree_lock_}; n->copy_error = fee ? MemErr::OutOfMemory : fee.error(); n->capacity_epoch = capacity_epoch_;
        return std::unexpected(*n->copy_error);
    }
    memcpy(page->bytes(), pmm_->bytes(n->borrowed.page()), page_size);
    PageHold borrowed;
    {
        sync::Lock guard{tree_lock_};
        n->usage = std::move(*fee); n->resident = std::move(*page);
        borrowed = std::move(n->borrowed); n->copy_error.reset();
        n->dirty_epoch = ++n->usage_epoch;
    }
    return {};
}
auto Mem::make_private(usize page, Source::Context& context) noexcept -> std::expected<void, MemErr> {
    Cache* cache{}; bool shortage{}; MemErr error{MemErr::OutOfMemory};
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || !store_) return std::unexpected(MemErr::InvalidState);
        if (reassigning_ || !work_open_.load(std::memory_order_acquire)) return std::unexpected(MemErr::Busy);
        cache = std::get_if<Cache>(&*store_);
        if (!cache) return std::unexpected(MemErr::InvalidState);
        sync::Lock contents{cache->tree_lock_};
        if (auto* n = cache->nodes_.find(page); n && n->copy_error) {
            shortage = n->capacity_epoch == cache->capacity_epoch_; error = *n->copy_error;
            if (!shortage) n->copy_error.reset();
        }
        if (shortage) { ++operations_; ++preparing_; }
    }
    if (shortage) {
        Pin pin{*this}; libk::scope_exit ready{[&]() noexcept { prepared(); }};
        auto result = cache->capacity(*this, page, &context, error);
        if (!result && result.error() == MemErr::Pending) pin.release();
        return std::unexpected(result ? MemErr::Busy : result.error());
    }
    return start_trim({page, 1}, context, {}, true, {}, {}, {}, true);
}
void Cache::records(PagerRecords& out) noexcept {
    sync::Lock guard{tree_lock_};
    for (auto* n = nodes_.lower_bound(out.cursor); n && out.count < PAGER_RECORD_COUNT; n = nodes_.next(*n)) {
        auto& r = out.pages[out.count++];
        r.page = n->index; r.epoch = n->usage_epoch;
        r.flags = (n->present() ? PAGER_RESIDENT : 0) | (n->dirty_epoch ? PAGER_DIRTY : 0)
            | (cfg_.pager && cfg_.pager->active(n->request) ? PAGER_BUSY : 0)
            | (n->pins.wired.load(std::memory_order_acquire) ? PAGER_PINNED : 0)
            | (n->accessed ? PAGER_ACCESSED : 0);
        n->accessed = false;
        out.cursor = n->index + 1;
    }
    if (!nodes_.lower_bound(out.cursor)) out.cursor = 0;
}
void Mem::records(PagerRecords& out, bool sample) noexcept {
    const auto first = out.cursor;
    {
        sync::Lock guard{lock_};
        out.key = id(); out.quota = quota_.id().value; out.bytes = size(); out.state = static_cast<u32>(state_);
        if (trimming_) out.state |= PAGER_BUSY << 16;
        if (!store_ || state_ != MemState::Live) return;
        ++operations_;
        if (auto* c = std::get_if<Cache>(&*store_)) c->records(out);
    }
    Pin pin{*this};
    if (!sample || !out.count) { out.alias_cursor = out.map_cursor = 0; return; }
    for (usize step = 0; step <= 8; ++step) {
        MemLink* next{};
        MemWork hold;
        {
            sync::Lock guard{lock_};
            next = attachments_.lower_bound(out.alias_cursor);
            if (next && next->id() == out.alias_cursor && !out.map_cursor) next = attachments_.next(*next);
            if (!next) { out.alias_cursor = out.map_cursor = 0; return; }
            if (step == 8) { out.state |= PAGER_PARTIAL; out.cursor = first; return; }
            out.alias_cursor = next->id();
            sync::Lock work{next->lock_};
            next->work_.fetch_add(1, std::memory_order_relaxed);
            hold = MemWork{*next};
        }
        out.map_cursor = next->ops_->sample ? next->ops_->sample(next->context_, {out.pages, out.count}, out.map_cursor) : 0;
        if (out.map_cursor) { out.state |= PAGER_PARTIAL; out.cursor = first; return; }
    }
}
auto Cache::snapshot(Mem& owner, usize index, const PageHold& destination) noexcept -> std::expected<void, MemErr> {
    Node* n;
    {
        sync::Lock guard{tree_lock_};
        n = nodes_.find(index);
        if (!n || !n->resident) return std::unexpected(MemErr::NotBacked);
        if (n->pins.uses.load(std::memory_order_acquire) || cfg_.pager->active(n->request))
            return std::unexpected(MemErr::Busy);
    }
    auto written = destination.h_.get().owner->write(destination, 0, {n->resident.bytes(), page_size});
    if (!written) return written;
    sync::Lock guard{tree_lock_};
    if (!n->dirty_epoch) n->dirty_epoch = ++n->usage_epoch;
    n->snapshot = true;
    n->request.mem = &owner;
    if (!writeback_locked(*n)) { n->snapshot = false; return std::unexpected(MemErr::BackingFailed); }
    n->dirty_epoch = 0;
    cfg_.local = false;
    return {};
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
    if (const auto* cache = std::get_if<Cache>(&*store_))
        return cache->paged() ? BackingKind::Pager : cache->source_ ? BackingKind::Private : BackingKind::Anonymous;
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
    // A transfer keeps preparation active until its source slot is committed
    // or restored. Publish immutable content only after that boundary.
    if (preparing_) return std::unexpected(MemErr::Busy);
    seal_ = SealState::Sealing;
    for (auto* attachment = attachments_.minimum(); attachment; attachment = attachments_.next(*attachment)) {
        if (attachment->perms_.contains(Perm::Write)) {
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
        if (auto* data = std::get_if<Cache>(store)) return data->query(page_index);
        return std::get_if<Extents>(store)->query(page_index);
    }();
    return (result);
}

auto Mem::materialize(usize page_index, Source::Context* context, bool wired) noexcept
    -> std::expected<PageHold, MemErr> {
    Store* store{};
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live) return std::unexpected(MemErr::InvalidState);
        if (!work_open_.load(std::memory_order_acquire) || reassigning_) return std::unexpected(MemErr::Busy);
        if (page_index >= logical_pages_) {
            return std::unexpected(MemErr::InvalidRange);
        }
        libk_assert(operations_ != std::numeric_limits<usize>::max());
        if (trimming_ && trimming_->contains(page_index)) {
            if (!context) return std::unexpected(MemErr::Busy);
            const auto ready = trim_events_.await(*context, {.mask = trim_clean | trim_dirty | trim_busy | trim_canceled}, {}, {},
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
        if (auto* data = std::get_if<Cache>(store))
            return data->materialize(*this, page_index, context);
        return std::get_if<Extents>(store)->materialize(page_index);
    }();
    const bool retained = context && !result && result.error() == MemErr::Pending;
    bool live{};
    {
        sync::Lock guard{lock_};
        live = state_ == MemState::Live;
    }
    if (!result && result.error() == MemErr::Forwarded) return std::unexpected(MemErr::Pending);
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

auto Mem::wait_capacity(usize index, Source::Context& context, MemErr error, u64 quota) noexcept -> std::expected<void, MemErr> {
    Cache* cache;
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || !work_open_.load(std::memory_order_acquire) || reassigning_
            || !store_ || !(cache = std::get_if<Cache>(&*store_)) || !cache->paged()) return std::unexpected(error);
        ++operations_;
    }
    Pin hold{*this};
    auto result = cache->capacity(*this, index, &context, error, quota);
    if (!result && result.error() == MemErr::Pending) hold.release();
    return std::unexpected(result.error());
}

auto Mem::begin_transfer(usize page_index) noexcept -> std::expected<PageTransfer, MemErr> {
    Cache* backing{};
    {
        sync::Lock guard{lock_};
        if (reassigning_) return std::unexpected(MemErr::Busy);
        if (seal_ != SealState::Loadable) return std::unexpected(MemErr::InvalidAccess);
        if (state_ != MemState::Live || !attachments_.empty() || operations_ != 0 ||
            page_index >= logical_pages_ || !store_) {
            return std::unexpected((!attachments_.empty() || operations_ != 0) ? MemErr::Busy
                                                                               : MemErr::InvalidState);
        }
        libk_assert(operations_ != std::numeric_limits<usize>::max());
        ++operations_; ++preparing_;
        backing = std::get_if<Cache>(&*store_);
        if (backing && backing->paged()) backing = nullptr;
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
    using Result = decltype(fn(std::declval<Cache&>()));
    Cache* backing{};
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || reassigning_ || !store_ ||
            !(backing = std::get_if<Cache>(&*store_)) || !backing->paged())
            return Result{std::unexpected(error)};
        ++operations_; ++preparing_;
    }
    Pin hold{*this};
    libk::scope_exit preparation{[&]() noexcept { prepared(); }};
    return std::forward<F>(fn)(*backing);
}

auto Mem::supply(Pager& pager, u64 id, OwnedPage&& page, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates) noexcept -> std::expected<void, MemErr> {
    return paged([&](Cache& p) noexcept { return p.supply(*this, pager, id, std::move(page), admission, gates); });
}

auto Mem::supply(Pager& pager, const PageHold& source, u64 id, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates, bool reclaim) noexcept -> std::expected<void, MemErr> {
    if (!source || source.h_.get().owner->pmm_ != pmm_ || !source.perms().contains(Perm::Read)) return std::unexpected(MemErr::InvalidAccess);
    return paged([&](Cache& p) noexcept { return p.supply(*this, pager, id, {}, admission, gates, source.page(), reclaim); });
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
    Cache* backing{};
    {
        sync::Lock guard{lock_};
        if ((state_ != MemState::Live && state_ != MemState::Stopping) || page_index >= logical_pages_ ||
            !store_) {
            return std::unexpected(MemErr::InvalidState);
        }
        ++operations_;
        backing = std::get_if<Cache>(&*store_);
    }
    Pin hold{*this};
    auto result = backing != nullptr && backing->paged() ? backing->observe_usage(page_index, accessed, dirty)
                                     : std::expected<void, MemErr>{};
    return result;
}

auto Mem::trim(ObjectRange range, Source::Context& context, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates, std::span<const cap::GrantPin*> grants) noexcept -> std::expected<void, MemErr> {
    return start_trim(range, context, {}, true, admission, gates, grants);
}
auto Mem::reclaim(usize page, Source::Context& context, PageHold&& snapshot,
    cap::Graph::Admission admission, std::span<const object::ref<>*> gates,
    std::span<const cap::GrantPin*> grants) noexcept -> std::expected<void, MemErr> {
    return start_trim({page, 1}, context, std::move(snapshot), false, admission, gates, grants);
}
auto Mem::start_trim(ObjectRange range, Source::Context& context, PageHold&& snapshot, bool waiter,
    cap::Graph::Admission admission, std::span<const object::ref<>*> gates,
    std::span<const cap::GrantPin*> grants, bool copy) noexcept -> std::expected<void, MemErr> {
    Source::Publication publication;
    auto cpu = context.thread.cpu_domain();
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || !store_ || !std::get_if<Cache>(&*store_)
            || (!std::get<Cache>(*store_).paged() && !std::get<Cache>(*store_).source_))
            return std::unexpected(MemErr::InvalidState);
        if (range.empty() || !ObjectRange{0, logical_pages_}.contains(range))
            return std::unexpected(MemErr::InvalidRange);
        if (reassigning_ || trimming_ || advancing_
            || (!copy && std::get<Cache>(*store_).done_ && std::get<Cache>(*store_).cfg_.pager->active(std::get<Cache>(*store_).done_->request)))
            return std::unexpected(MemErr::Busy);
        auto self = object::ref<>::retain_alive(job_.key());
        auto& op = trim_op_.emplace(object::ref<>{}, Source::Action::bind<&Mem::post>(*this),
            Source::Action::bind<&Mem::advance>(*this), &ordinary_);
        op.bind(*work_, job_.key());
        auto install = [&]() noexcept {
            publication = trim_events_.prepare(0, true);
            if (waiter) {
                const auto ready = trim_events_.await(context, {.mask = trim_clean | trim_dirty | trim_busy | trim_canceled}, {}, {},
                    Source::Action::bind<&Mem::drop_page>(*this));
                libk_assert(ready.status == STATUS_PENDING);
            }
            operations_ += waiter ? 2 : 1;

            copying_ = copy; notify_reclaim_ = !waiter;
            if (snapshot) snapshot_.emplace(std::move(snapshot));
            trimming_ = range; trim_phase_ = TrimPhase::Invalidate;
            trim_cursor_ = range.base(); trim_released_ = 0; trim_removing_ = false; trim_status_ = STATUS_OK;
            scan_ = attachments_.minimum();
        };
        // Execution is sponsored by this operation's caller, never permanently
        // by whoever created the transferable memory object.
        if (op.start(std::move(cpu), self, admission, gates, Source::Action::bind(install), grants) != STATUS_OK) {
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


auto Mem::pager_finish(Pager& pager, u64 id, bool fail, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates, bool retry) noexcept -> std::expected<u32, MemErr> {
    return paged([&](Cache& p) noexcept { return p.finish(*this, pager, id, fail, admission, gates, retry); });
}

auto Mem::write(usize offset, std::span<const byte> input) noexcept -> std::expected<void, MemErr> {
    const usize within = offset & (page_size - 1);
    if (input.empty() || input.size() > page_size - within || offset >= size() ||
        input.size() > size() - offset)
        return std::unexpected(MemErr::InvalidRange);
    {
        sync::Lock guard{lock_};
        if (state_ != MemState::Live || (std::get_if<Cache>(&*store_) == nullptr || std::get<Cache>(*store_).paged() || std::get<Cache>(*store_).source_) ||
            seal_ != SealState::Loadable || !perms_.contains(Perm::Write))
            return std::unexpected(MemErr::InvalidAccess);
    }
    auto lease = materialize(offset / page_size);
    if (!lease) return std::unexpected(lease.error());
    return write(*lease, within, input);
}

auto Mem::write(const PageHold& lease, usize offset, std::span<const byte> input) noexcept -> std::expected<void, MemErr> {
    libk_assert(lease && lease.h_.get().owner == this && offset <= page_size && input.size() <= page_size - offset);
    sync::Lock guard{lock_};
    // Storage pinning alone grants no write access. Serialize every bounded
    // kernel write with seal and pager binding at the actual copy.
    const auto* cache = store_ ? std::get_if<Cache>(&*store_) : nullptr;
    if (state_ != MemState::Live || !cache || cache->paged() || cache->source_
        || seal_ != SealState::Loadable || !perms_.contains(Perm::Write))
        return std::unexpected(MemErr::InvalidAccess);
    memmove(pmm_->bytes(lease.page()) + offset, input.data(), input.size());
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

auto Mem::protect(MemLink& link, Perms perms) noexcept -> std::expected<void, MemErr> {
    sync::Lock guard{lock_};
    if (link.owner_ != this || !valid_perms(perms) || !perms_.contains(perms))
        return std::unexpected(MemErr::InvalidAccess);
    // Shrinking a drained consumer is valid even during invalidation. Raising
    // permissions shares the same admission cut as sealing and new links.
    if (!link.perms_.contains(perms) &&
        (state_ != MemState::Live || trimming_ ||
         static_cast<MemLink::State>(link.state_.load()) != MemLink::State::Attached ||
         (perms.contains(Perm::Execute) && seal_ != SealState::Executable) ||
         (perms.contains(Perm::Write) && seal_ != SealState::Loadable)))
        return std::unexpected(MemErr::InvalidAccess);
    link.perms_ = perms;
    return {};
}

auto Mem::attach(MemLink& attachment, Perms perms) noexcept -> std::expected<void, MemErr> {
    sync::Lock guard{lock_};
    if (attachment.ops_->wired) {
        auto* c = store_ ? std::get_if<Cache>(&*store_) : nullptr;
        if (!c || c->paged() || c->source_) return std::unexpected(MemErr::InvalidState);
    }
    if (state_ != MemState::Live || !work_open_.load(std::memory_order_acquire) || trimming_ || !valid_perms(perms) ||
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
    attachments_.insert(attachment);
    return {};
}

auto Mem::attachment_count() const noexcept -> usize {
    sync::Lock guard{lock_};
    return attachments_.size();
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
            scan_ = attachments_.next(attachment);
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
        if (cleanup) initialization_.retire(std::move(cleanup));
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
            scan_ = attachments_.next(*link);
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
        scan_ = attachments_.minimum();
    }
    run_work();
}

void Mem::run_work() noexcept {
    const auto phase = initialization_.query().sequence;
    if (phase && phase != OP_DONE && initialization_.schedule()) return;
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
    if (const auto phase = initialization_.query().sequence; phase && phase != OP_DONE) {
        if (init_status_ == STATUS_PENDING) {
            init_status_ = STATUS_CANCELED;
            if (!initialization_.canceled()) {
                const auto ready = std::get<Cache>(*store_).prepay(quota_, prepay_);
                if (ready && !*ready) { init_status_ = STATUS_PENDING; initialization_.post(); return; }
                init_status_ = ready ? STATUS_OK : STATUS_NO_MEMORY;
                if (ready) {
                    if (initialization_.commit()) {
                        sync::Lock guard{lock_}; prepay_ = 0;
                        work_open_.store(state_ == MemState::Live, std::memory_order_release);
                    } else init_status_ = STATUS_CANCELED;
                }
            }
        }
        if (!initialization_.detach()) { initialization_.post(); return; }
        initialization_.finish(init_status_);
        { sync::Lock guard{lock_}; if (stopping_) post(); }
        return;
    }
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
            if (trim_pending_ || preparing_) return;
            trim_phase_ = TrimPhase::Pages;
            break;
        }
        case TrimPhase::Pages: {
            auto result = [&]() -> std::expected<bool, MemErr> {
                auto& cache = std::get<Cache>(*store_);
                if (copying_) {
                    auto copied = cache.copy(range->base(), quota_);
                    return copied ? std::expected<bool, MemErr>{true} : std::unexpected(copied.error());
                }
                if (!snapshot_) return cache.trim(*range, trim_cursor_, trim_removing_, trim_released_);
                auto copied = cache.snapshot(*this, range->base(), *snapshot_);
                return copied ? std::expected<bool, MemErr>{true} : std::unexpected(copied.error());
            }();
            if (!result && result.error() == MemErr::Pending) return;
            if (result && !*result) break;
            trim_status_ = result ? STATUS_OK : result.error() == MemErr::Dirty ? STATUS_DIRTY : STATUS_BUSY;
            sync::Lock guard{lock_};
            scan_ = attachments_.minimum();
            trim_phase_ = TrimPhase::Restore;
            break;
        }
        case TrimPhase::Restore: {
            sync::Lock guard{lock_};
            for (usize step = 0; step < 8 && scan_; ++step) {
                auto* link = scan_;
                scan_ = attachments_.next(*link);
                link->state_.store(static_cast<u8>(MemLink::State::Attached), std::memory_order_release);
            }
            if (!scan_) trim_phase_ = TrimPhase::Finish;
            break;
        }
        case TrimPhase::Finish: {
            if (!op.detach()) return;
            Source::Publication publication;
            bool closing;
            std::optional<PageHold> snapshot;
            {
                sync::Lock guard{lock_};
                op.finish(trim_status_);
                trim_op_.reset();
                const bool has_snapshot = snapshot_.has_value();
                if (notify_reclaim_ && (!has_snapshot || trim_status_ != STATUS_OK))
                    std::get<Cache>(*store_).completed(*this, range->base(), trim_status_, trim_released_);
                trimming_.reset();
                copying_ = false;
                snapshot = std::move(snapshot_);
                snapshot_.reset();
                publication = trim_events_.prepare(trim_status_ == STATUS_OK ? trim_clean
                    : trim_status_ == STATUS_DIRTY ? trim_dirty
                    : trim_status_ == STATUS_CANCELED ? trim_canceled : trim_busy, true);
                closing = state_ == MemState::Stopping;
                if (closing) ++operations_;
            }
            if (pager_ref_) static_cast<Pager*>(pager_ref_.get())->signal();
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
    Cache* backing;
    { sync::Lock guard{lock_}; backing = store_ ? std::get_if<Cache>(&*store_) : nullptr; }
    if (backing && backing->paged() && !backing->stop(*this)) { post(); return; }
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
    Cache* p;
    {
        sync::Lock guard{lock_};
        libk_assert(store_ && reply.mem() == this);
        p = std::get_if<Cache>(&*store_);
    }
    libk_assert(p);
    libk_assert(p->complete(reply, true));
}

void Mem::finish_transfer(usize page_index, OwnedPage&& page, bool commit) noexcept {
    Cache* backing{};
    {
        sync::Lock guard{lock_};
        libk_assert(operations_ != 0 && store_.has_value());
        backing = std::get_if<Cache>(&*store_);
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
        if (pager_ref_) static_cast<Pager*>(pager_ref_.get())->detach(binding_);
        if (!std::visit([](auto& store) noexcept { return store.drain(); }, *store_)) {
            libk_assert(work_); work_->post(job_); return;
        }
        store_.reset(); reservation_.reset(); pager_ref_.reset();
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
    initialization_.retire(std::move(cleanup));
}

} // namespace mm
