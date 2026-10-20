#include <algorithm>
#include <array>
#include <libk/types.hpp>
#include <cap/grant.hpp>
#include <cpu/cpu.hpp>
#include <expected>
#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/mem.h>
#include <libk/memory.hpp>
#include <libk/scope_guard.hpp>
#include <limits>
#include <mm/mem.hpp>
#include <mm/table.hpp>
#include <mm/vspace.hpp>
#include <object/ref.hpp>
#include <panic.hpp>
#include <sync.hpp>
#include <task/thread.hpp>
#include <utility>

namespace mm {

auto status(VSpaceError error) noexcept -> status_t {
    switch (error) {
    case VSpaceError::InvalidAuthority:
    case VSpaceError::InvalidAccess:
        return STATUS_BAD_RIGHTS;
    case VSpaceError::InvalidRegion:
    case VSpaceError::InvalidMapping:
    case VSpaceError::NotMapped:
        return STATUS_NOT_FOUND;
    case VSpaceError::InvalidRange:
    case VSpaceError::Overlap:
    case VSpaceError::NotRam:
        return STATUS_BAD_ARGS;
    case VSpaceError::OutOfMemory:
    case VSpaceError::QuotaExceeded:
    case VSpaceError::GenerationExhausted:
        return STATUS_NO_MEMORY;
    case VSpaceError::Canceled:
        return STATUS_CANCELED;
    case VSpaceError::ResourceExhausted:
        return STATUS_NO_MEMORY;
    case VSpaceError::InvalidState:
    case VSpaceError::Busy:
    case VSpaceError::GrantUnavailable:
        return STATUS_BUSY;
    case VSpaceError::BackingFailed:
        return STATUS_BACKING_FAILED;
    case VSpaceError::TranslationCorrupt:
        libk_assert(false);
        __builtin_unreachable();
    }
    return STATUS_INTERNAL;
}

VmOp::VmOp(Pmm& pmm, VSpace& owner) noexcept
    : owner_(&owner), state_({}, Source::Action::bind<&VSpace::flush_ready>(owner)),
      flush_(pmm, sync::Latch::Notifier::bind<&VSpace::flush_ready>(owner)) {}
VmOp::VmOp(object::ref<> provider) noexcept
    : owner_(static_cast<VSpace*>(provider.get())),
      state_(std::move(provider), Source::Action::bind<&VSpace::flush_ready>(*owner_), Source::Action::bind<&VSpace::advance>(*owner_)),
      flush_(*owner_->pmm_, sync::Latch::Notifier::bind<&VSpace::flush_ready>(*owner_)) {}
VmOp::~VmOp() noexcept { libk_assert(!maps_ && !pages_ && retired_.empty() && !refund_ && !edit_); }

auto VmOp::drain_pages() noexcept -> bool {
    libk_assert(owner_->lock_.held());
    if (flushed_) return true;
    if (edit_ && !edit_->batch) return false;
    if (!flush_.complete()) return false;
    if (!released_) {
        if (!flush_.release(refund_)) { owner_->flush_ready(); return false; }
        released_ = true;
        if (!edit_) state_.drain();
    }
    usize remaining = 8;
    while (pages_ && remaining) {
        --remaining;
        auto* page = pages_; pages_ = page->pending_next_;
        owner_->release_page(*page, refund_);
    }
    while (maps_ && remaining) {
        --remaining;
        auto* map = maps_; maps_ = map->pending_next_; map->pending_next_ = nullptr;
        owner_->destroy_layout(*map);
    }
    if (pages_ || maps_) { owner_->flush_ready(); return false; }
    flushed_ = true;
    return true;
}

auto VmOp::finish() noexcept -> bool {
    refund_.reset(); // Physical storage was released before its capacity refund.
    return state_.detach();
}

auto VSpace::transaction() noexcept -> VmOp& {
    libk_assert(lock_.held());
    if (!active_) {
        active_ = &control_.emplace(*pmm_, *this);
        active_->state_.bind(*work_, job_.key());
        active_->state_.accept(); libk_assert(active_->state_.commit());
    }
    return *active_;
}

auto VSpace::accept(VmCtx ctx, bool commit) noexcept -> std::expected<void, VSpaceError> {
    libk_assert(lock_.held());
    if (!ctx.submission.publish) return {};
    libk_assert(!active_ && ctx.submission.execution && *ctx.submission.execution);
    auto install = [&] noexcept {
        operation_ = std::move(*ctx.submission.execution);
        active_ = static_cast<VmOp*>(operation_.get());
    };
    if (ctx.submission.publish(Source::Action::bind(install), {}) != STATUS_OK)
        return std::unexpected(VSpaceError::GrantUnavailable);
    if (commit && !active_->state_.commit()) return std::unexpected(VSpaceError::Canceled);
    return {};
}

auto VSpace::fault(Thread& thread, VmCtx ctx, va_t address, Perm perm) noexcept -> FaultKind {
    libk_assert(ctx.cpus);
    Source::Context context{thread, *ctx.cpus, current_cpu().dispatcher()};
    auto result = fault(ctx, address, perm, &context);
    const auto kind = result ? result->kind : fault_kind(result.error());
    if (kind == FaultKind::Materialized) {
        const auto ready = wait_pending(thread, *ctx.cpus, current_cpu().dispatcher());
        return ready.status == STATUS_PENDING ? FaultKind::Pending : kind;
    }
    if (kind != FaultKind::Busy) return kind;
    object::ref<> source;
    usize index{};
    {
        sync::Lock guard{lock_};
        auto* map = find(VRange{va_t{address.raw() & ~(page_size - 1)}, page_size});
        if (map && map->kind_ == MapKind::Map && map->perms_.contains(perm)) {
            auto& backing = *map->binding_;
            if (backing.memory_work_ && !backing.memory_work_->range().empty()) {
                source = backing.memory_ref_;
                index = map->object_.base() + (address.raw() - map->range_.base().raw()) / page_size;
            }
        }
    }
    if (!source) return kind;
    auto mem = source.as<Mem>();
    libk_assert(mem);
    const auto ready = mem->get().materialize(index, &context);
    return ready ? FaultKind::Busy : fault_kind(ready.error());
}

[[nodiscard]] static auto node_error(SlabErr error) noexcept -> VSpaceError {
    switch (error) {
    case SlabErr::OutOfMemory:
        return VSpaceError::OutOfMemory;
    case SlabErr::QuotaExceeded:
        return VSpaceError::QuotaExceeded;
    case SlabErr::GenerationExhausted:
        return VSpaceError::GenerationExhausted;
    case SlabErr::ResourceExhausted:
        return VSpaceError::ResourceExhausted;
    }
    return VSpaceError::OutOfMemory;
}

[[nodiscard]] static auto memory_error(MemErr error) noexcept -> VSpaceError {
    switch (error) {
    case MemErr::OutOfMemory:
        return VSpaceError::OutOfMemory;
    case MemErr::ResourceExhausted:
        return VSpaceError::ResourceExhausted;
    case MemErr::GenerationExhausted:
        return VSpaceError::GenerationExhausted;
    case MemErr::Dirty:
    case MemErr::Busy:
        return VSpaceError::Busy;
    case MemErr::Pending:
        return VSpaceError::Busy;
    case MemErr::BackingFailed:
    case MemErr::NotBacked:
        return VSpaceError::BackingFailed;
    case MemErr::NotRam:
        return VSpaceError::NotRam;
    case MemErr::InvalidAccess:
        return VSpaceError::InvalidAccess;
    case MemErr::InvalidSize:
    case MemErr::InvalidRange:
        return VSpaceError::InvalidRange;
    case MemErr::InvalidState:
    case MemErr::AttachmentState:
    case MemErr::OwnershipMismatch:
        return VSpaceError::InvalidState;
    }
    return VSpaceError::InvalidState;
}

Map::~Map() noexcept {}

MapPage::~MapPage() noexcept {
    libk_assert(!tree_hook_.is_linked());
    libk_assert(binding_ == nullptr);
}

const MemOps Backing::memory_ops_{
    .invalidate = &Backing::invalidate_memory,
    .released = &Backing::released,
};

const cap::AttachOps Backing::grant_ops_{
    .invalidate = &Backing::invalidate_grant,
    .released = &Backing::released,
};

Backing::Backing(VSpace& owner, object::ref<> memory, Mem& object, Perms perms, bool private_write) noexcept
    : owner_(&owner), memory_ref_(std::move(memory)), memory_(&object), perms_(perms),
      private_write_(private_write), memory_attachment_(this, memory_ops_) {}

Backing::~Backing() noexcept {
    libk_assert(mappings_.empty());
    libk_assert(views_.empty());
    libk_assert(pages_.empty());
    libk_assert(!invalidation_hook_.is_linked() && !retired_hook_.is_linked());
    libk_assert(drained());
    libk_assert(!memory_work_ && !grant_work_);
    if (grant_attachment_) {
        grant_attachment_.reset();
    }
}

auto Backing::attach_memory() noexcept -> std::expected<void, MemErr> {
    return memory_->attach(memory_attachment_, private_write_ ? Perms::of(Perm::Read) : perms_);
}

auto Backing::detach() noexcept -> bool {
    static_cast<void>(memory_attachment_.detach());
    if (grant_attachment_) static_cast<void>(grant_attachment_->detach());
    MemWork mem;
    object::LinkWork grant;
    {
        sync::Lock lock{owner_->lock_};
        if (memory_work_) {
            mem = std::move(*memory_work_);
            memory_work_.reset();
        }
        if (grant_work_) {
            grant = std::move(*grant_work_);
            grant_work_.reset();
        }
        if (invalidation_hook_.is_linked()) owner_->invalidations_.erase(*this);
    }
    // Late publication still owns a real link work pin. It will requeue this
    // exit; never destroy storage merely because no work was visible yet.
    mem.reset();
    grant.reset();
    return drained();
}

bool Backing::drained() const noexcept {
    return !memory_attachment_.attached() && !memory_attachment_.busy() &&
           (!grant_attachment_ || (!grant_attachment_->attached() && !grant_attachment_->busy()));
}

void Backing::invalidate_memory(void* ctx, MemWork&& work) noexcept {
    auto& auth = *static_cast<Backing*>(ctx);
    auth.owner_->request_invalidation(auth, std::move(work));
}

void Backing::released(void* ctx, MemWork&& done) noexcept {
    auto& space = *static_cast<Backing*>(ctx)->owner_;
    sync::Lock guard{space.lock_};
    done.reset();
    space.work_->post(space.job_);
}

void Backing::released(void* ctx, object::LinkWork&& done) noexcept {
    auto& space = *static_cast<Backing*>(ctx)->owner_;
    sync::Lock guard{space.lock_};
    done.reset();
    space.work_->post(space.job_);
}

void Backing::invalidate_grant(void* ctx, object::LinkWork&& work) noexcept {
    auto& auth = *static_cast<Backing*>(ctx);
    auth.owner_->request_invalidation(auth, std::move(work));
}

auto VSpace::prepare(const object::ref<>& payer, Pmm& pmm, KSpace& kernel, WorkQueue& work) noexcept
    -> std::expected<Data, VSpaceError> {
    object::ref<> source;
    resource::Charge charge;
    if (payer) {
        source = payer;
        auto acquired = resource::acquire(source, resource::budget{.memory = page_size});
        if (!acquired) return std::unexpected(VSpaceError::ResourceExhausted);
        charge = std::move(*acquired);
    }
    auto root = PageTable::create(pmm, PageTable::Kind::User, &kernel.pages());
    if (!root) return std::unexpected(VSpaceError::OutOfMemory);
    return Data{pmm, kernel, work, std::move(source), std::move(charge), std::move(*root)};
}

VSpace::VSpace(Data&& data) noexcept
    : pmm_(data.pmm_), kernel_(data.kernel_), work_(data.work_), payer_(std::move(data.payer_)),
      mappings_(*pmm_), binding_pool_(*pmm_), pages_(*pmm_), views_(*pmm_),
      job_(Work::Fn::bind<&VSpace::run_work>(*this)),
      table_charge_(std::move(data.charge_)) {
    (void) root_.emplace(std::move(data.root_));
}

VSpace::~VSpace() noexcept {
    libk_assert(state_ == VSpaceState::Quiescent);
    libk_assert(layout_.empty());
    libk_assert(!root_);
    libk_assert(!editing_ && !draining_);
    libk_assert(invalidations_.empty());
    libk_assert(!active_ && !control_ && !operation_ && !cleanup_);

    libk_assert(bindings_ == 0);
    libk_assert(!table_charge_);
}

void VSpace::release_root() noexcept {
    libk_assert(root_);
    const usize pages = root_->page_count();
    root_.reset();
    if (!table_charge_) {
        libk_assert(!payer_);
        return;
    }
    const resource::budget expected{
        .memory = static_cast<u64>(pages) * page_size,
    };
    libk_assert(table_charge_.amount() == expected);
    // The user tree released every owned table page above.  Capacity becomes
    // available only after that physical ownership transition.
    table_charge_.reset();
}

auto VSpace::state() const noexcept -> VSpaceState {
    sync::Lock guard{lock_};
    return state_;
}

auto VSpace::binding_count() const noexcept -> usize {
    sync::Lock guard{lock_};
    return bindings_;
}

auto VSpace::attach_execution() noexcept -> bool {
    sync::Lock guard{lock_};
    if (state_ != VSpaceState::Live || bindings_ == std::numeric_limits<usize>::max()) {
        return false;
    }
    ++bindings_;
    return true;
}

void VSpace::detach_execution() noexcept {
    bool ready{};
    {
        sync::Lock guard{lock_};
        libk_assert(bindings_ != 0);
        --bindings_;
        ready = state_ == VSpaceState::Stopping && bindings_ == 0;
    }
    if (ready) {
        work_->post(job_);
    }
}

auto VSpace::can_destroy_object(cap::VmLimit auth) const noexcept -> bool {
    return auth.range.contains(VRange{va_t{UserBegin}, UserEnd - UserBegin});
}

auto VSpace::root() noexcept -> Root {
    libk_assert(root_);
    return Root{tlb_, root_->cpu_root()};
}

auto VSpace::commit_flush(Tlb::Edit&& mutation, VmCtx ctx, Flush& retire,
                          bool instruction_sync) noexcept -> VmStatus {
    libk_assert(active_);
    const bool complete = mutation.commit(retire, ctx.cpus, ctx.local, instruction_sync);
    if (complete) {
        if (active_->drain_pages()) {
            return (VmStatus::Complete);
        }
        return (VmStatus::Pending);
    }
    work_->post(job_);
    return (VmStatus::Pending);
}

void VSpace::link(Map& map) noexcept {
    if (layout_epoch_ != std::numeric_limits<u64>::max()) ++layout_epoch_;
    layout_.insert(map);
    if (map.kind_ == MapKind::Map) mapped_ += map.range_.size();
}
void VSpace::unlink(Map& map) noexcept {
    if (layout_epoch_ != std::numeric_limits<u64>::max()) ++layout_epoch_;
    layout_.erase(map);
    if (map.kind_ == MapKind::Map) mapped_ -= map.range_.size();
}
auto VSpace::info(u64) const noexcept -> VSpaceInfo {
    sync::Lock guard{lock_};
    return {.version = 1, .size = sizeof(VSpaceInfo), .mapped = mapped_, .resident = resident_,
        .state = static_cast<u32>(state_), .reserved = 0};
}

auto VSpace::links(u64 cursor, u64 epoch, LinkRecords& out) const noexcept -> status_t {
    sync::Lock guard{lock_};
    if (layout_epoch_ == std::numeric_limits<u64>::max() || (epoch && epoch != layout_epoch_)) return STATUS_STALE;
    out = {}; out.version = 1; out.size = sizeof(out); out.epoch = layout_epoch_;
    auto* map = layout_.lower_bound(va_t{cursor ? cursor : UserBegin});
    for (usize scanned = 0; map && scanned < LINK_RECORD_PAGE; ++scanned) {
        out.cursor = map->range_.limit()->raw();
        if (auto* backing = map->binding_) {
            out.records[out.count++] = {backing->memory_attachment_.id(), backing->memory_ref_.id().value,
                backing->grant_attachment_ ? backing->grant_attachment_->id().value : 0,
                map->range_.base().raw(), map->range_.size(), map->object_.base() * page_size,
                LINK_VM, backing->invalid() ? LINK_INVALIDATING : LINK_LIVE, map->perms_.raw(), 0};
        }
        map = layout_.next(*map);
    }
    out.more = map != nullptr;
    if (!out.more) out.cursor = 0;
    return STATUS_OK;
}

auto VSpace::inspect(VRange scope, va_t cursor) const noexcept -> std::expected<VmRecords, VSpaceError> {
    sync::Lock guard{lock_};
    if (!cursor.raw()) cursor = scope.base();
    const auto end = scope.limit();
    if (!end || cursor < scope.base() || cursor >= *end || cursor.raw() % page_size)
        return std::unexpected(VSpaceError::InvalidRange);
    VmRecords info{};
    info.version = 1; info.size = sizeof(info);
    auto* map = layout_.lower_bound(cursor);
    if (!map || map->range_.base() > cursor) {
        auto* previous = map ? layout_.previous(*map) : layout_.maximum();
        if (previous && previous->range_.contains(VRange{cursor, page_size})) map = previous;
    }
    while (map && map->range_.base() < *end && info.count < VM_RECORD_COUNT) {
        const auto first = std::max(cursor.raw(), map->range_.base().raw());
        const auto last = std::min({map->range_.limit()->raw(), end->raw(), first + 64 * page_size});
        auto& row = info.records[info.count++];
        row.address = first; row.bytes = last - first; row.perms = map->perms_.raw();
        row.kind = map->kind_ == MapKind::Map ? VM_RECORD_MAP
            : map->kind_ == MapKind::Reserved ? VM_RECORD_RESERVED : VM_RECORD_GUARD;
        if (map->binding_ && map->binding_->memory_->managed()) {
            const auto& pages = map->binding_->pages_;
            for (auto* page = pages.lower_bound(va_t{first}); page && page->address_.raw() < last; page = pages.next(*page))
                row.resident += page_size;
        }
        cursor = va_t{last};
        if (cursor == *end) break;
        if (cursor == *map->range_.limit()) map = layout_.next(*map);
    }
    if (map && map->range_.base() < *end && cursor < *end) info.cursor = cursor.raw();
    return info;
}

void VSpace::queue_layout(Map& node) noexcept {
    libk_assert(node.pending_next_ == nullptr);
    node.pending_next_ = active_->maps_;
    active_->maps_ = &node;
}

void VSpace::queue_page(MapPage& page, bool removed) noexcept {
    if (removed && page.binding_->memory_->managed()) { libk_assert(resident_ >= page_size); resident_ -= page_size; }
    libk_assert(page.pending_next_ == nullptr);
    page.pending_next_ = active_->pages_;
    active_->pages_ = &page;
}

void VSpace::queue_binding(Backing& auth) noexcept {
    if (!auth.retired_hook_.is_linked()) transaction().retired_.push_back(auth);
}

void VSpace::detach_mapping(Map& mapping) noexcept {
    Backing& auth = *mapping.binding_;
    if (mapping.backing_hook_.is_linked()) {
        auth.mappings_.erase(mapping);
    }

    mappings_.destroy(mapping);
    if (auth.mappings_.empty()) {
        queue_binding(auth);
    }
}

void VSpace::destroy_layout(Map& node) noexcept {
    if (node.binding_)
        detach_mapping(node);
    else
        mappings_.destroy(node);
}

void VSpace::release_page(MapPage& page, resource::Charge& refund) noexcept {
    page.binding_ = nullptr;
    refund.merge(page.take_charge());
    pages_.destroy(page);
}

void VSpace::finish_bindings() noexcept {
    {
        sync::Lock lock{lock_};
        if (draining_ || editing_ || (!active_ && invalidations_.empty())) return;
        draining_ = true;
        if (active_ && !editing_) (void)active_->drain_pages();
    }
    object::ref<> provider;
    { sync::Lock guard{lock_}; if (active_) provider = active_->state_.provider(); }
    libk::scope_exit completed{[this]() noexcept {
        VmOp* op;
        { sync::Lock guard{lock_}; op = active_ && !active_->edit_ && active_->flushed_ && !editing_ && active_->retired_.empty() ? active_ : nullptr; }
        const bool done = op && op->finish();
        object::ref<> execution;
        bool internal{};
        { sync::Lock guard{lock_}; internal = done && !operation_; }
        // The internal control Source has no observers and its storage must
        // finish before reuse. Public Done follows removal of the provider
        // transaction, so a woken caller can immediately submit its next edit.
        if (internal) op->complete();
        {
            sync::Lock guard{lock_};
            if (done) {
                libk_assert(active_ == op);
                active_ = nullptr;
                execution = std::move(operation_);
                if (control_) control_.reset();
            }
            draining_ = false;
            try_finish_retire();
        }
        if (done && !internal) op->complete();
        if (work_ready()) work_->post(job_);
        settle();
    }};
    for (usize step = 0; step < 8; ++step) {
        MemWork work;
        {
            sync::Lock guard{lock_};
            if ((active_ && !active_->flushed_) || editing_) break;
            for (auto& auth : invalidations_) {
                if (!auth.memory_work_ || auth.memory_work_->range().empty() || auth.grant_work_) continue;
                if (!auth.trim_scanned_) continue;
                work = std::move(*auth.memory_work_);
                auth.memory_work_.reset();
                auth.trim_cursor_ = {};
                auth.trim_scanned_ = false;
                invalidations_.erase(auth);
                break;
            }
        }
        if (!work) break;
        work.reset();
    }
    for (usize step = 0; step < 8; ++step) {
        Backing* auth{};
        {
            sync::Lock guard{lock_};
            if (!active_ || !active_->flushed_ || active_->retired_.empty()) return;
            auth = &active_->retired_.front();
            libk_assert(auth->mappings_.empty());
            libk_assert(auth->pages_.empty());
            if (auth->invalidation_hook_.is_linked()) {
                invalidations_.erase(*auth);
            }
        }

        // The sole drain owner keeps this node indexed while callbacks run.
        // Reentrant service may enqueue work, but cannot claim this exit.
        if (!auth->detach()) return;
        {
            sync::Lock guard{lock_};
            active_->retired_.erase(*auth);
        }
        binding_pool_.destroy(*auth);
    }
}

auto VSpace::wait_pending(Thread& thread, Cpus& cpus, sched::Dispatcher& dispatcher) noexcept -> WaitResult {
    sync::Lock guard{lock_};
    if (!pending_locked()) return {};
    const auto seen = settled_.snapshot().sequence;
    return settled_.await({thread, cpus, dispatcher}, {.mask = 0, .seen = seen, .changed = true});
}

void VSpace::settle() noexcept {
    Source::Publication publication;
    {
        sync::Lock guard{lock_};
        if (pending_locked()) return;
        publication = settled_.prepare(0);
    }
}

void VSpace::retire(object::cleanup&& cleanup) noexcept {
    bool can_start{};
    {
        sync::Lock guard{lock_};
        libk_assert(state_ == VSpaceState::Live);
        state_ = VSpaceState::Stopping;
        libk_assert(!cleanup_);
        [[maybe_unused]] auto& retained = cleanup_.emplace(std::move(cleanup));
        if (!root_ && bindings_ == 0) {
            libk_assert(layout_.empty() && !active_ && !editing_);
            work_->close(job_);
            state_ = VSpaceState::Quiescent;
            guard.restore();
            complete_cleanup();
            return;
        }
        can_start = bindings_ == 0 && !active_ && !editing_ && tlb_.active_cpus().empty();
    }
    if (can_start) {
        static_cast<void>(clear(VmCtx{.local = CpuId{0}}, VRange{va_t{UserBegin}, UserEnd - UserBegin}));
    } else {
        work_->post(job_);
    }
    complete_cleanup();
}

auto VSpace::valid_user_range(VRange range) noexcept -> bool {
    const auto end = range.limit();
    return range.valid() && !range.empty() && end && (range.base().raw() & (page_size - 1)) == 0 &&
           (range.size() & (page_size - 1)) == 0 && range.base().raw() >= mm::UserBegin &&
           end->raw() <= mm::UserEnd;
}

auto VSpace::find(VRange range) noexcept -> Map* {
    auto* n = layout_.lower_bound(range.base());
    if (!n || n->range_.base() > range.base()) n = n ? layout_.previous(*n) : layout_.maximum();
    return n && n->range_.contains(range) ? n : nullptr;
}

auto VSpace::overlap(VRange range) noexcept -> Map* {
    auto* n = layout_.lower_bound(range.base());
    if (n && n->range_.intersects(range)) return n;
    auto* prev = n ? layout_.previous(*n) : layout_.maximum();
    return prev && prev->range_.intersects(range) ? prev : nullptr;
}

auto VSpace::begin_edit(VRange range, bool empty) noexcept -> std::expected<void, VSpaceError> {
    if (state_ != VSpaceState::Live || editing_ || draining_ || active_) return std::unexpected(VSpaceError::Busy);
    if (!valid_user_range(range)) return std::unexpected(VSpaceError::InvalidRange);
    if (empty && overlap(range)) return std::unexpected(VSpaceError::Overlap);
    editing_ = true;
    return {};
}

void VSpace::end_edit() noexcept {
    bool wake{};
    {
        sync::Lock lock{lock_};
        libk_assert(editing_);
        editing_ = false;
        wake = (!waiting_view_ || waiting_view_->idle()) && ((active_ && active_->flush_.complete())
            || !invalidations_.empty() || state_ == VSpaceState::Stopping);
    }
    // Submit from actual obligations, after local rollback and outside lock_.
    if (wake) work_->post(job_);
    else settle();
}

auto VSpace::reserve(VRange range, bool guard) noexcept -> std::expected<void, VSpaceError> {
    {
        sync::Lock lock{lock_};
        auto admitted = begin_edit(range, true);
        if (!admitted) return std::unexpected(admitted.error());
    }
    Edit edit{this};
    auto made = mappings_.create(payer_, range, guard ? MapKind::Guard : MapKind::Reserved);
    if (!made) return std::unexpected(node_error(made.error()));
    auto rollback = libk::scope_exit{[&]() noexcept { mappings_.destroy(*made->object); }};
    sync::Lock lock{lock_};
    if (state_ != VSpaceState::Live) return std::unexpected(VSpaceError::InvalidState);
    link(*made->object);
    rollback.release();
    return {};
}

Borrow::~Borrow() noexcept { libk_assert(!hook_.is_linked() && idle()); }

void Borrow::leave() noexcept {
    // The execution's View and VSpace owners cover this final notification.
    // Once zero is published the invalidation worker can consume the Borrow.
    auto* owner = owner_;
    const auto before = uses_.fetch_sub(1, std::memory_order_acq_rel);
    libk_assert(before & ~closed);
    if (before == closed + 1) owner->flush_ready();
}

void View::Drop::operator()(Borrow* borrow) const noexcept { borrow->owner_->detach_view(*borrow); }
bool View::valid() const noexcept { return h_ && !(h_.get()->uses_.load(std::memory_order_acquire) & Borrow::closed); }

auto View::access() const noexcept -> std::optional<Access> {
    if (!h_) return {};
    auto& borrow = *h_.get();
    auto uses = borrow.uses_.load(std::memory_order_relaxed);
    while (!(uses & Borrow::closed) && uses != Borrow::closed - 1)
        if (borrow.uses_.compare_exchange_weak(uses, uses + 1,
                std::memory_order_acquire, std::memory_order_relaxed)) return Access{borrow};
    return {};
}

auto View::Access::region(usize offset, usize count, Perm mode) const noexcept -> std::span<byte> {
    const auto& borrow = *h_.get();
    const auto size = borrow.object_.size() * page_size;
    if (!borrow.perms_.contains(mode) || !count || offset >= size || count > size - offset
        || count > page_size - offset % page_size) return {};
    return {borrow.owner_->pmm_->bytes(borrow.pages_[offset / page_size].page()) + offset % page_size, count};
}

auto View::Access::copy(usize offset, std::span<byte> data, bool write) const noexcept -> bool {
    const auto& borrow = *h_.get();
    const auto size = borrow.object_.size() * page_size;
    if (offset > size || data.size() > size - offset
        || !borrow.perms_.contains(write ? Perm::Write : Perm::Read)) return false;
    usize copied{};
    while (copied < data.size()) {
        const auto position = offset + copied;
        const auto count = std::min(data.size() - copied, page_size - position % page_size);
        auto* page = borrow.owner_->pmm_->bytes(borrow.pages_[position / page_size].page()) + position % page_size;
        if (write) ::memcpy(page, data.data() + copied, count);
        else ::memcpy(data.data() + copied, page, count);
        copied += count;
    }
    return true;
}

auto VSpace::bind_view(ViewReq&& request) noexcept -> std::expected<View, VSpaceError> {
    const auto page_count = request.virtual_range.page_count();
    if (!request.memory || !valid_perms(request.perms) || !valid_user_range(request.virtual_range) ||
        !page_count || !*page_count || *page_count > ViewReq::max_pages || request.object.size() != *page_count) {
        return std::unexpected(VSpaceError::InvalidRange);
    }
    auto memory_pin = request.memory.as<Mem>();
    if (!memory_pin) {
        return std::unexpected(VSpaceError::InvalidAuthority);
    }
    Mem& memory = memory_pin.value().get();
    Map* mapping{};
    auto validate = [&]() noexcept -> bool {
        auto* node = find(request.virtual_range);
        if (!node || !node->binding_) return false;
        auto* const current = static_cast<Map*>(node);
        if (current->binding_->private_write_ || !current->layout_hook_.is_linked() || !current->range_.contains(request.virtual_range) ||
            !current->perms_.contains(request.perms) || &current->binding_->memory() != &memory) {
            return false;
        }
        const auto page_offset = current->range_.page_offset(request.virtual_range.base());
        libk_assert(page_offset);
        const ObjectRange expected{current->object_.base() + *page_offset, *page_count};
        if (expected != request.object) {
            return false;
        }
        mapping = current;
        return true;
    };
    {
        sync::Lock guard{lock_};
        if (!validate()) {
            return std::unexpected(VSpaceError::InvalidMapping);
        }
        auto admitted = begin_edit(request.virtual_range, false);
        if (!admitted) {
            return std::unexpected(admitted.error());
        }
    }

    Edit edit{this};
    auto made = views_.create(payer_, *this, *mapping->binding_, std::move(request));
    if (!made) return std::unexpected(node_error(made.error()));
    Borrow* const relation = made->object;
    auto rollback = libk::scope_exit{[&]() noexcept { views_.destroy(*relation); }};
    for (usize i = 0; i < *page_count; ++i) {
        auto page = memory.materialize(relation->object_.base() + i, nullptr, true);
        if (!page) return std::unexpected(memory_error(page.error()));
        if (!pmm_->is_ram(page->page())) return std::unexpected(VSpaceError::NotRam);
        libk_assert(relation->pages_.try_push_back(std::move(*page)));
    }
    {
        sync::Lock guard{lock_};
        if (state_ != VSpaceState::Live || mapping->binding_->invalid())
            return std::unexpected(VSpaceError::InvalidMapping);
        mapping->binding_->views_.push_back(*relation);
        relation->uses_.store(0, std::memory_order_release);
        rollback.release();
    }
    return View{*relation};
}

void VSpace::detach_view(Borrow& borrow) noexcept {
    Borrow::Pages released;
    {
        sync::Lock lock{lock_};
        (void)borrow.close();
        libk_assert(borrow.idle());
        released = std::move(borrow.pages_);
        if (waiting_view_ == &borrow) { waiting_view_ = nullptr; flush_ready(); }
        if (active_ && active_->edit_ && active_->edit_->view == &borrow) {
            auto& list = borrow.backing_->views_;
            auto next = ++list.iterator_to(borrow);
            active_->edit_->view = next == list.end() ? nullptr : &*next;
        }
        if (borrow.hook_.is_linked()) borrow.backing_->views_.erase(borrow);
        borrow.backing_ = nullptr;
    }
    views_.destroy(borrow);
}


auto VSpace::reserve_tables(MapPage* pages) noexcept -> std::expected<TableReserve, VSpaceError> {
    auto& editor = *root_;
    auto plan = editor.count();
    for (MapPage* page = pages; page != nullptr; page = page->pending_next_) {
        const auto virtual_page = VPage::from_base(page->address_);
        if (!virtual_page || !plan.include(*virtual_page)) {
            return std::unexpected(VSpaceError::TranslationCorrupt);
        }
    }
    const usize count = plan.pages();
    resource::Charge charge{};
    if (payer_ && count != 0) {
        const auto bytes = libk::checked_multiply<u64>(static_cast<u64>(count), static_cast<u64>(page_size));
        if (!bytes) {
            return std::unexpected(VSpaceError::ResourceExhausted);
        }
        auto acquired = resource::acquire(payer_, resource::budget{
                                                      .memory = bytes.value(),
                                                  });
        if (!acquired) {
            return std::unexpected(VSpaceError::ResourceExhausted);
        }
        charge = std::move(acquired).value();
    }

    PageGroup reserve = pmm_->group();
    if (!reserve.grow(count)) {
        return std::unexpected(VSpaceError::OutOfMemory);
    }
    return (TableReserve{
        std::move(charge),
        std::move(reserve),
    });
}

void VSpace::commit_tables(TableReserve& reserve) noexcept {
    if (reserve.charge) {
        const resource::budget charged = reserve.charge.amount();
        libk_assert(charged.caps == 0 && charged.memory % page_size == 0);
        const usize total = static_cast<usize>(charged.memory / page_size);
        libk_assert(total >= reserve.pages.page_count());
        const usize consumed = total - reserve.pages.page_count();
        if (consumed != 0) {
            table_charge_.merge(reserve.charge.split(resource::budget{
                .memory = static_cast<u64>(consumed) * page_size,
            }));
        }
    }
    // Unconsumed prepared pages return to PMM before their capacity token.
    reserve.pages.reset();
    reserve.charge.reset();
}

void VSpace::retire_table(Flush& retire, OwnedPage&& page) noexcept {
    if (!table_charge_) {
        libk_assert(retire.adopt(std::move(page)));
        return;
    }
    auto charge = table_charge_.split(resource::budget{
        .memory = page_size,
    });
    libk_assert(retire.adopt(std::move(page), std::move(charge)));
}

auto VSpace::map(VmCtx ctx, MapReq request, object::ref<> ref, Mem& mem, cap::MemLimit auth) noexcept
    -> std::expected<MapResult, VSpaceError> {
    return map_impl(ctx, request, std::move(ref), mem, auth,
                    Perms::of(Perm::Read, Perm::Write, Perm::Execute), nullptr);
}

auto VSpace::map(VmCtx ctx, cap::VmLimit where, MapReq request, cap::Resolved<Mem>& mem) noexcept
    -> std::expected<MapResult, VSpaceError> {
    auto view = mem.view();
    auto* auth = std::get_if<cap::MemLimit>(&view.data);
    if (!auth || !view.rights.contains(cap::Right::Map) || !where.range.contains(request.virtual_range) ||
        !where.perms.contains(request.perms))
        return std::unexpected(VSpaceError::InvalidAuthority);
    auto ref = mem.reference();
    if (!ref) return std::unexpected(VSpaceError::InvalidState);
    return map_impl(ctx, request, std::move(*ref), mem.object(), *auth, where.perms, &mem);
}

auto VSpace::map_impl(VmCtx ctx, MapReq request, object::ref<> memory_ref, Mem& memory,
                      cap::MemLimit mem_auth, Perms vspace_access, cap::Resolved<Mem>* capability) noexcept
    -> std::expected<MapResult, VSpaceError> {
    {
        sync::Lock lock{lock_};
        auto admitted = begin_edit(request.virtual_range, true);
        if (!admitted) return std::unexpected(admitted.error());
    }
    Edit editing{this};
    const auto page_count = request.virtual_range.page_count();
    libk_assert(page_count);
    const usize count = *page_count;
    const bool private_write = request.private_write;
    const Perms ceiling = vspace_access.intersect(
        private_write ? Perms::from_raw(mem_auth.perms.raw() | static_cast<u8>(Perm::Write))
                      : mem_auth.perms);
    const bool invalid_access = !valid_perms(request.perms) || (request.perms.contains(Perm::Write) &&
                                                                request.perms.contains(Perm::Execute));
    if (invalid_access) {
        return std::unexpected(VSpaceError::InvalidAccess);
    }
    if (!memory_ref || memory_ref.kind() != ObjMem || count == 0 ||
        request.object.size() != count || !request.object.within(memory.page_count()) ||
        !mem_auth.range.contains(request.object) || !ceiling.contains(request.perms) ||
        (private_write &&
         (!request.perms.contains(Perm::Write) || !mem_auth.perms.contains(Perm::Read) ||
          memory.kind() == BackingKind::Physical || memory.seal_state() != SealState::Executable))) {
        return std::unexpected(VSpaceError::InvalidAuthority);
    }

    auto backing_entry =
        binding_pool_.create(payer_, *this, std::move(memory_ref), memory, request.perms, private_write);
    if (!backing_entry) {
        return std::unexpected(node_error(backing_entry.error()));
    }
    Backing* const auth = backing_entry.value().object;

    auto backing = libk::scope_exit{[&]() noexcept {
        {
            sync::Lock lock{lock_};
            queue_binding(*auth);
        }
        finish_bindings();
    }};

    auto memory_attached = auth->attach_memory();
    if (!memory_attached) {
        // attach() failed before relation publication.
        return std::unexpected(memory_error(memory_attached.error()));
    }
    if (capability != nullptr) {
        auto& attachment = auth->grant_attachment_.emplace(auth, Backing::grant_ops_);
        auto grant_attached = capability->attach(attachment);
        if (!grant_attached) {
            return std::unexpected(VSpaceError::GrantUnavailable);
        }
    }

    auto mapping_entry =
        mappings_.create(payer_, request.virtual_range, request.object, request.perms, ceiling, *auth);
    if (!mapping_entry) {
        return std::unexpected(node_error(mapping_entry.error()));
    }
    Map* const mapping = mapping_entry.value().object;
    mapping->key_ = MapId{mapping_entry.value().key};
    const MapId key = mapping->key_;
    auto layout = libk::scope_exit{[&]() noexcept { mappings_.destroy(*mapping); }};

    // Map publishes one authorized VMA. Every backing kind uses the same
    // single-page fault path; mapping never scans a transient resident set.
    sync::Lock lock{lock_};
    if (state_ != VSpaceState::Live || auth->invalid())
        return std::unexpected(VSpaceError::InvalidState);
    if (auto accepted = accept(ctx); !accepted) return std::unexpected(accepted.error());
    auth->mappings_.push_back(*mapping);
    link(*mapping);
    layout.release(); backing.release();
    return MapResult{key, VmStatus::Complete};
}

auto VSpace::protect(VmCtx ctx, cap::VmLimit auth, VRange range, Perms perms) noexcept
    -> std::expected<VmStatus, VSpaceError> {
    if (!auth.range.contains(range) || !auth.perms.contains(perms))
        return std::unexpected(VSpaceError::InvalidAuthority);
    return protect(ctx, range, perms);
}

auto VSpace::protect(VmCtx ctx, VRange range, Perms perms) noexcept -> std::expected<VmStatus, VSpaceError> {
    return edit(ctx, range, perms);
}

auto VSpace::edit(VmCtx ctx, VRange range, std::optional<Perms> perms, bool clear) noexcept
    -> std::expected<VmStatus, VSpaceError> {
    if (perms && (!valid_perms(*perms) || (perms->contains(Perm::Write) && perms->contains(Perm::Execute))))
        return std::unexpected(VSpaceError::InvalidAccess);
    // Inline control edits and both worker classes share one execution claim.
    // Public edits and final Clear only prepare here; their runner owns progress.
    bool inline_step{};
    libk::scope_exit leave{[&]() noexcept {
        if (!inline_step) return;
        sync::Lock guard{lock_}; advancing_ = false;
        if (std::exchange(again_, false)) work_->post(job_);
    }};
    {
        sync::Lock guard{lock_};
        if (!valid_user_range(range)) return std::unexpected(VSpaceError::InvalidRange);
        if (state_ != VSpaceState::Live && !(clear && state_ == VSpaceState::Stopping))
            return std::unexpected(VSpaceError::InvalidState);
        if (editing_ || draining_ || active_) return std::unexpected(VSpaceError::Busy);
        if (!ctx.submission.publish && !clear && advancing_) return std::unexpected(VSpaceError::Busy);
        if (auto accepted = accept(ctx, false); !accepted) return std::unexpected(accepted.error());
        auto& op = transaction();
        op.edit_.emplace(VmOp::Edit{.range=range, .perms=perms, .clear=clear,
            .cursor=overlap(range), .covered=range.base()});
        if (!ctx.submission.publish && !clear) { advancing_ = inline_step = true; }
        if (!inline_step) active_->state_.post();
    }
    if (!inline_step) return VmStatus::Pending;
    const auto result = advance_edit(ctx);
    finish_bindings();
    return result;
}

auto VSpace::advance_edit(VmCtx ctx) noexcept -> std::expected<VmStatus, VSpaceError> {
    Borrow::Pages released;
    Edit editing{};
    std::optional<sync::Lock<sync::Spin>> guard{std::in_place, lock_};
    libk_assert(active_ && active_->edit_);
    auto& op = *active_;
    auto& edit = *op.edit_;
    if (editing_ || draining_) return VmStatus::Pending;
    if (edit.batch) {
        if (!op.flushed_ || !op.retired_.empty()) return VmStatus::Pending;
        op.refund_.reset();
        std::destroy_at(&op.flush_);
        std::construct_at(&op.flush_, *pmm_, sync::Latch::Notifier::bind<&VSpace::flush_ready>(*this));
        op.released_ = op.flushed_ = edit.batch = false;
    }
    editing_ = true;
    editing = Edit{this};
    auto fail = [&](VSpaceError error) -> std::expected<VmStatus, VSpaceError> {
        const auto tails = edit.tails;
        op.error_ = error;
        op.edit_.reset();
        guard->restore();
        for (auto* tail : tails) if (tail) mappings_.destroy(*tail);
        { sync::Lock lock{lock_}; (void)op.drain_pages(); }
        return std::unexpected(error);
    };
    std::optional<Tlb::Edit> mutation{std::in_place, tlb_.begin()};
    bool changed{};
    for (usize step = 0; step < 8; ++step) {
        using E = VmOp::Edit;
        auto* map = edit.cursor;
        if (edit.phase < E::Apply && (op.state_.canceled() ||
            (state_ != VSpaceState::Live && !edit.clear))) {
            mutation->abort();
            return fail(VSpaceError::Canceled);
        }
        switch (edit.phase) {
        case E::Scan:
            if (!map || map->range_.base() >= *edit.range.limit()) {
                if (!edit.clear && edit.perms && edit.covered < *edit.range.limit()) {
                    mutation->abort(); return fail(VSpaceError::NotMapped);
                }
                edit.phase = E::Prepare;
                break;
            }
            // Unmap is idempotent over holes, including mappings already removed
            // by grant revocation. Protect still requires complete coverage;
            // neither operation may destroy a reserved or guarded VA region.
            if (!edit.clear && (!map->binding_ || (edit.perms && map->range_.base() > edit.covered))) {
                mutation->abort(); return fail(VSpaceError::NotMapped);
            }
            if (map->binding_ && map->binding_->invalid() && !edit.clear) {
                mutation->abort(); return fail(VSpaceError::Busy);
            }
            if (edit.perms && !map->ceiling_.contains(*edit.perms)) {
                mutation->abort(); return fail(VSpaceError::InvalidAccess);
            }
            if (!edit.first) edit.first = map;
            edit.last = map;
            edit.covered = *map->range_.limit();
            edit.page = std::max(map->range_.base(), edit.range.base());
            edit.view = map->binding_ && !map->binding_->views_.empty() ? &map->binding_->views_.front() : nullptr;
            edit.phase = E::Views;
            break;
        case E::Views:
            if (!edit.clear && edit.view) {
                auto& list = map->binding_->views_;
                auto next = ++list.iterator_to(*edit.view);
                if (edit.view->range_.intersects(edit.range)) {
                    mutation->abort(); return fail(VSpaceError::Busy);
                }
                edit.view = next == list.end() ? nullptr : &*next;
            } else { edit.view = nullptr; edit.phase = E::Check; }
            break;
        case E::Check: {
            auto* page = edit.perms && map->binding_ ? map->binding_->pages_.lower_bound(edit.page) : nullptr;
            if (!page || page->address_ >= std::min(*map->range_.limit(), *edit.range.limit())) {
                edit.cursor = layout_.next(*map); edit.phase = E::Scan; break;
            }
            if (!page->perms().contains(*edit.perms) || !PageTable::user_perms(*edit.perms)) {
                mutation->abort(); return fail(VSpaceError::InvalidAccess);
            }
            edit.page = va_t{page->address_.raw() + page_size};
            break;
        }
        case E::Prepare: {
            // The active transaction owns these records across allocation;
            // editing_ excludes retirement and competing preparations.
            mutation->abort(); guard->restore();
            std::expected<Map*, VSpaceError> made{nullptr};
            if (edit.first && edit.first->range_.base() < edit.range.base() && !edit.tails[0])
                made = make_fragment(*edit.first, {edit.first->range_.base(), edit.range.base().raw() - edit.first->range_.base().raw()}, edit.first->perms_);
            if (made && *made) edit.tails[0] = *made;
            if (made && edit.last && *edit.last->range_.limit() > *edit.range.limit() && !edit.tails[1]) {
                made = make_fragment(*edit.last, {*edit.range.limit(), edit.last->range_.limit()->raw() - edit.range.limit()->raw()}, edit.last->perms_);
                if (made) edit.tails[1] = *made;
            }
            guard.emplace(lock_);
            if (!made) return fail(made.error());
            if (edit.stale && !edit.clear) return fail(VSpaceError::Busy);
            if (op.state_.query().sequence == OP_ACCEPTED && !op.state_.commit()) return fail(VSpaceError::Canceled);
            edit.cursor = edit.first; edit.phase = edit.cursor ? E::Apply : E::Done;
            mutation.emplace(tlb_.begin());
            break;
        }
        case E::Apply: {
            const auto lo = std::max(map->range_.base(), edit.range.base());
            const auto hi = std::min(*map->range_.limit(), *edit.range.limit());
            unlink(*map);
            if (map->binding_) map->object_ = {map->object_.base() + (lo.raw() - map->range_.base().raw()) / page_size, (hi.raw() - lo.raw()) / page_size};
            map->range_ = {lo, hi.raw() - lo.raw()};
            for (usize i = 0; i < edit.tails.size(); ++i) {
                auto*& tail = edit.tails[i];
                if (tail && map == (i == 0 ? edit.first : edit.last)) {
                    if (tail->binding_) tail->binding_->mappings_.push_back(*tail);
                    link(*tail); tail = nullptr;
                }
            }
            edit.page = lo;
            edit.view = edit.clear && map->binding_ && !map->binding_->views_.empty() ? &map->binding_->views_.front() : nullptr;
            edit.phase = edit.view ? E::Revoke : E::Pages;
            break;
        }
        case E::Revoke: {
            auto& list = map->binding_->views_;
            if (edit.view) {
                auto* view = edit.view;
                auto next = ++list.iterator_to(*view);
                if (view->range_.intersects(map->range_)) {
                    (void)view->close();
                    if (!view->idle()) {
                        waiting_view_ = view;
                        mutation->abort(); return VmStatus::Pending;
                    }
                    waiting_view_ = nullptr;
                    released = std::move(view->pages_);
                    list.erase(*view); view->backing_ = nullptr;
                    step = 8; // Release one bounded resident borrow outside lock_.
                }
                edit.view = next == list.end() ? nullptr : &*next;
            } else edit.phase = E::Pages;
            break;
        }
        case E::Pages: {
            auto* page = map->binding_ ? map->binding_->pages_.lower_bound(edit.page) : nullptr;
            if (page && page->address_ < *map->range_.limit()) {
                auto vp = VPage::from_base(page->address_); libk_assert(vp);
                edit.page = va_t{page->address_.raw() + page_size};
                if (edit.perms) {
                    const auto perms = PageTable::user_perms(map->binding_->private_write_ && !page->private_owned() ? Perms::of(Perm::Read) : *edit.perms);
                    libk_assert(perms);
                    auto old = root_->protect(*vp, *perms); libk_assert(old);
                    libk_assert(fold_usage(*page, *old));
                } else {
                    map->binding_->pages_.erase(*page);
                    auto old = root_->unmap(*vp); libk_assert(old);
                    libk_assert(fold_usage(*page, old->usage));
                    for (auto& table : old->tables) retire_table(op.flush_, std::move(table));
                    queue_page(*page);
                }
                changed = true;
            } else {
                // cursor remains live until this Map's pages have left. The
                // successor is taken before handing the Map to retirement.
                edit.cursor = map == edit.last ? nullptr : layout_.lower_bound(*map->range_.limit());
                if (edit.perms) { map->perms_ = *edit.perms; link(*map); }
                else queue_layout(*map);
                edit.phase = edit.cursor ? E::Apply : E::Done;
            }
            break;
        }
        case E::Done:
            op.edit_.reset();
            if (changed) return commit_flush(std::move(*mutation), ctx, op.flush_);
            mutation->abort();
            return op.drain_pages() ? VmStatus::Complete : VmStatus::Pending;
        }
    }
    // Only application batches create receipts. Validation has no effects.
    if (edit.phase >= VmOp::Edit::Apply) {
        edit.batch = true;
        if (changed) return commit_flush(std::move(*mutation), ctx, op.flush_);
        mutation->abort();
        (void)op.drain_pages();
    }
    return VmStatus::Pending;
}

auto VSpace::make_fragment(Map& source, VRange range, Perms perms) noexcept
    -> std::expected<Map*, VSpaceError> {
    auto made =
        source.binding_
            ? mappings_.create(payer_, range,
                               ObjectRange{source.object_.base() + *source.range_.page_offset(range.base()),
                                           *range.page_count()},
                               perms, source.ceiling_, *source.binding_)
            : mappings_.create(payer_, range, source.kind_);
    if (!made) return std::unexpected(node_error(made.error()));
    made->object->key_ = made->key;
    return made->object;
}

auto VSpace::unmap(VmCtx ctx, cap::VmLimit auth, VRange range) noexcept
    -> std::expected<VmStatus, VSpaceError> {
    if (!auth.range.contains(range)) return std::unexpected(VSpaceError::InvalidAuthority);
    return unmap(ctx, range);
}

auto VSpace::unmap(VmCtx ctx, VRange range) noexcept -> std::expected<VmStatus, VSpaceError> {
    return edit(ctx, range, std::nullopt);
}

auto VSpace::clear(VmCtx ctx, VRange range) noexcept -> std::expected<VmStatus, VSpaceError> {
    return edit(ctx, range, std::nullopt, true);
}

auto fault_kind(VSpaceError error) noexcept -> FaultKind {
    switch (error) {
    case VSpaceError::Busy:
        return FaultKind::Busy;
    case VSpaceError::OutOfMemory:
        return FaultKind::OutOfMemory;
    case VSpaceError::ResourceExhausted:
    case VSpaceError::QuotaExceeded:
        return FaultKind::ResourceExhausted;
    case VSpaceError::BackingFailed:
        return FaultKind::BackingFailed;
    default:
        return FaultKind::BackingFailed;
    }
}

auto fault_kind(MemErr error) noexcept -> FaultKind {
    return error == MemErr::Pending ? FaultKind::Pending : fault_kind(memory_error(error));
}

auto VSpace::fault(VmCtx ctx, va_t address, Perm perms, Source::Context* context) noexcept -> std::expected<FaultResult, VSpaceError> {
    const usize aligned = address.raw() & ~(page_size - 1);
    const VRange page_range{va_t{aligned}, page_size};
    if (!valid_user_range(page_range)) {
        return (FaultResult{.kind = FaultKind::NoMapping});
    }

    Map* mapping{};
    MapPage* private_source{};
    usize object_page{};
    {
        sync::Lock guard{lock_};
        if (state_ != VSpaceState::Live || active_ || editing_ || draining_) {
            return (FaultResult{.kind = FaultKind::Busy});
        }
        do {
            Map* node = find(page_range);
            if (node == nullptr || !node->range_.contains(page_range)) {
                return (FaultResult{.kind = FaultKind::NoMapping});
            }
            if (node->kind_ == MapKind::Guard) {
                return (FaultResult{.kind = FaultKind::Guard});
            }
            if (node->kind_ != MapKind::Map) {
                return (FaultResult{.kind = FaultKind::NoMapping});
            }
            mapping = static_cast<Map*>(node);
            if (!mapping->layout_hook_.is_linked() || !mapping->perms_.contains(perms)) {
                return (FaultResult{.kind = FaultKind::AccessDenied});
            }
            const auto mapping_offset = mapping->range_.page_offset(page_range.base());
            libk_assert(mapping_offset);
            object_page = mapping->object_.base() + *mapping_offset;
            if (auto* existing = mapping->binding_->pages_.find(page_range.base())) {
                if (perms == Perm::Write && mapping->binding_->private_write_ && !existing->private_owned()) {
                    auto admitted = begin_edit(page_range, false);
                    if (!admitted) return (FaultResult{.kind = FaultKind::Busy});
                    private_source = existing;
                    break;
                }

                return (FaultResult{
                    .kind = FaultKind::Ready,
                    .mapping = mapping->key_,
                    .object_page = object_page,
                });
            }
            auto admitted = begin_edit(page_range, false);
            if (!admitted) {
                return (FaultResult{.kind = FaultKind::Busy});
            }
            break;
        } while (false);
    }
    libk_assert(mapping != nullptr);
    if (private_source != nullptr) return copy_private_fault(ctx, *mapping, *private_source);
    return materialize_fault(ctx, *mapping, page_range.base(), object_page, context);
}

auto VSpace::copy_private_fault(VmCtx ctx, Map& mapping, MapPage& source) noexcept
    -> std::expected<FaultResult, VSpaceError> {
    Edit editing{this};
    if (!pmm_->is_ram(source.page())) return std::unexpected(VSpaceError::NotRam);
    resource::Charge charge{};
    if (payer_) {
        auto acquired = resource::acquire(payer_, resource::budget{.memory = page_size});
        if (!acquired) return std::unexpected(VSpaceError::ResourceExhausted);
        charge = std::move(acquired).value();
    }
    auto allocated = pmm_->allocate_page();
    if (!allocated) return std::unexpected(VSpaceError::OutOfMemory);
    OwnedPage private_page = std::move(allocated).value();
    memcpy(private_page.bytes(), pmm_->bytes(source.page()), page_size);

    auto made = pages_.create(payer_, source.address_, source.object_page_, std::move(private_page),
                              std::move(charge));
    if (!made) return std::unexpected(node_error(made.error()));
    MapPage* const replacement = made.value().object;
    const usize object_page = source.object_page_;
    const MapId mapping_key = mapping.key_;
    replacement->binding_ = mapping.binding_;
    auto rollback = libk::scope_exit{[&]() noexcept {
        replacement->binding_ = nullptr;
        pages_.destroy(*replacement);
    }};

    sync::Lock lock{lock_};
    Backing& auth = *mapping.binding_;
    if (state_ != VSpaceState::Live || auth.invalid()) {
        lock.restore();
        return std::unexpected(VSpaceError::Busy);
    }
    auto mutation = tlb_.begin();

    auto& editor = *root_;
    auto& retire = transaction().flush_;
    const auto virtual_page = VPage::from_base(source.address_);
    libk_assert(virtual_page);
    const auto permissions = mm::PageTable::user_perms(mapping.perms_);
    libk_assert(permissions);
    auto replaced = editor.replace(*virtual_page, replacement->page(), *permissions);
    libk_assert(replaced);
    auto folded = fold_usage(source, *replaced);
    libk_assert(folded);
    auth.pages_.erase(source);
    auth.pages_.insert(*replacement);
    rollback.release();
    queue_page(source, false);
    auto committed = commit_flush(std::move(mutation), ctx, retire);
    lock.restore();
    editing.reset();
    if (committed == VmStatus::Complete) finish_bindings();
    return (FaultResult{.kind = FaultKind::Materialized,
                        .mapping = mapping_key,
                        .object_page = object_page,
                        .status = committed});
}

auto VSpace::materialize_fault(VmCtx ctx, Map& mapping, va_t page_address, usize object_page,
                               Source::Context* context) noexcept
    -> std::expected<FaultResult, VSpaceError> {
    Edit editing{this};
    Backing& auth = *mapping.binding_;
    auto fail = [&](FaultKind kind) -> std::expected<FaultResult, VSpaceError> {
        return (FaultResult{
            .kind = kind,
            .mapping = mapping.key_,
            .object_page = object_page,
        });
    };
    Mem* const memory = &auth.memory();
    auto resident = memory->materialize(object_page, context);
    if (!resident) {
        if (resident.error() == MemErr::Pending) {
            return fail(FaultKind::Pending);
        }
        if (resident.error() == MemErr::Busy) {
            return fail(FaultKind::Busy);
        }

        if (resident.error() == MemErr::BackingFailed || resident.error() == MemErr::NotBacked) {
            return fail(FaultKind::BackingFailed);
        }
        return std::unexpected(memory_error(resident.error()));
    }
    PageHold source = std::move(resident).value();
    const Perms source_access = auth.private_write_ ? Perms::of(Perm::Read) : mapping.perms_;
    if (!source.perms().contains(source_access)) {
        return fail(FaultKind::AccessDenied);
    }
    const auto permissions = mm::PageTable::user_perms(source_access);
    if (!permissions) {
        return std::unexpected(VSpaceError::InvalidAccess);
    }

    auto made = pages_.create(payer_, page_address, object_page, std::move(source));
    if (!made) {
        return std::unexpected(node_error(made.error()));
    }
    MapPage* const page = made.value().object;
    page->binding_ = &auth;
    const MapId key = mapping.key_;
    auto rollback = libk::scope_exit{[&]() noexcept {
        page->binding_ = nullptr;
        pages_.destroy(*page);
    }};
    auto table_reserve = reserve_tables(page);
    if (!table_reserve) {
        return std::unexpected(table_reserve.error());
    }
    TableReserve tables = std::move(table_reserve).value();

    sync::Lock lock{lock_};
    if (state_ != VSpaceState::Live || auth.invalid()) {
        lock.restore();
        return std::unexpected(VSpaceError::InvalidState);
    }
    auto mutation = tlb_.begin();

    auto& editor = *root_;
    const auto virtual_page = VPage::from_base(page_address);
    libk_assert(virtual_page);
    auto installed = editor.map(*virtual_page, page->page(), *permissions, tables.pages);
    libk_assert(installed);
    commit_tables(tables);
    page->pending_next_ = nullptr;
    auth.pages_.insert(*page);
    if (memory->managed()) resident_ += page_size;
    rollback.release();
    auto& retire = transaction().flush_;
    auto committed =
        commit_flush(std::move(mutation), ctx, retire, mapping.perms_.contains(Perm::Execute));
    lock.restore();
    editing.reset();
    if (committed == VmStatus::Complete) {
        finish_bindings();
    }
    return (FaultResult{
        .kind = FaultKind::Materialized,
        .mapping = key,
        .object_page = object_page,
        .status = committed,
    });
}

auto VSpace::sample_usage(VmCtx ctx, va_t address, bool clear) noexcept
    -> std::expected<PageUsage, VSpaceError> {
    const usize aligned = address.raw() & ~(page_size - 1);
    const VRange page_range{va_t{aligned}, page_size};
    if (!valid_user_range(page_range)) {
        return std::unexpected(VSpaceError::InvalidRange);
    }

    sync::Lock lock{lock_};
    auto fail = [&](VSpaceError error) -> std::expected<PageUsage, VSpaceError> {
        lock.restore();
        return std::unexpected(error);
    };
    if (state_ != VSpaceState::Live || active_ || editing_ || draining_) {
        return fail(VSpaceError::Busy);
    }

    Map* mapping{};
    auto* node = find(page_range);
    if (!node || !node->binding_) return fail(VSpaceError::NotMapped);
    mapping = node;
    if (mapping == nullptr || !mapping->layout_hook_.is_linked()) {
        return fail(VSpaceError::NotMapped);
    }
    const auto offset = mapping->range_.page_offset(page_range.base());
    if (!offset) {
        return fail(VSpaceError::InvalidRange);
    }
    Backing& auth = *mapping->binding_;
    MapPage* const mapped = auth.pages_.find(page_range.base());
    if (mapped == nullptr) {
        return fail(VSpaceError::NotMapped);
    }
    const auto virtual_page = VPage::from_base(page_range.base());
    if (!virtual_page) {
        return fail(VSpaceError::InvalidRange);
    }
    auto& editor = *root_;
    if (!clear) {
        auto observed = editor.usage(*virtual_page);
        if (!observed) return fail(VSpaceError::TranslationCorrupt);
        auto usage = fold_usage(*mapped, *observed);
        lock.restore();
        return usage;
    }

    auto mutation = tlb_.begin();

    const auto cleared = editor.clear_usage(*virtual_page);
    if (!cleared) {
        mutation.abort();
        return fail(VSpaceError::TranslationCorrupt);
    }
    auto usage = fold_usage(*mapped, *cleared);
    libk_assert(usage);
    auto& retire = transaction().flush_;
    auto committed = commit_flush(std::move(mutation), ctx, retire);
    lock.restore();
    if (committed != VmStatus::Complete) return std::unexpected(VSpaceError::Busy);
    finish_bindings();
    return usage;
}

auto VSpace::fold_usage(MapPage& page, PageUsage observed) noexcept -> std::expected<PageUsage, VSpaceError> {
    libk_assert(page.binding_ != nullptr);
    if (!page.private_owned()) {
        auto folded =
            page.binding_->memory().observe_usage(page.object_page_, observed.accessed, observed.dirty);
        if (!folded) return std::unexpected(memory_error(folded.error()));
    }
    return observed;
}

auto VSpace::inspect(MapId key) const noexcept -> std::expected<MapInfo, VSpaceError> {
    sync::Lock guard{lock_};
    Map* const mapping = const_cast<Slab<Map>&>(mappings_).find(key);
    if (mapping == nullptr || mapping->key_ != key || !mapping->layout_hook_.is_linked()) {
        return std::unexpected(VSpaceError::InvalidMapping);
    }
    return (MapInfo{
        .key = mapping->key_,
        .range = mapping->range_,
        .object = mapping->object_,
        .perms = mapping->perms_,
        .ceiling = mapping->ceiling_,
    });
}

void VSpace::request_invalidation(Backing& auth, MemWork&& work) noexcept {
    {
        sync::Lock guard{lock_};
        if (active_ && active_->edit_) active_->edit_->stale = true;
        if (layout_epoch_ != std::numeric_limits<u64>::max()) ++layout_epoch_;
        libk_assert(!auth.memory_work_);
        [[maybe_unused]] auto& retained = auth.memory_work_.emplace(std::move(work));
        if (!auth.invalidation_hook_.is_linked()) {
            invalidations_.push_back(auth);
        }
    }
    work_->post(job_);
}

void VSpace::request_invalidation(Backing& auth, object::LinkWork&& work) noexcept {
    {
        sync::Lock guard{lock_};
        if (active_ && active_->edit_) active_->edit_->stale = true;
        if (layout_epoch_ != std::numeric_limits<u64>::max()) ++layout_epoch_;
        libk_assert(!auth.grant_work_);
        [[maybe_unused]] auto& retained = auth.grant_work_.emplace(std::move(work));
        if (!auth.invalidation_hook_.is_linked()) {
            invalidations_.push_back(auth);
        }
    }
    work_->post(job_);
}

auto VSpace::start_invalidation(VmCtx ctx, Backing& auth) noexcept -> std::expected<VmStatus, VSpaceError> {
    Borrow::Pages released;
    sync::Lock lock{lock_};
    if (active_ || editing_ || draining_) {
        return std::unexpected(VSpaceError::Busy);
    }
    const ObjectRange trim = auth.memory_work_ ? auth.memory_work_->range() : ObjectRange{};
    const bool keep_maps = !trim.empty() && !auth.grant_work_ && state_ == VSpaceState::Live;
    if (auth.mappings_.empty()) {
        if (auth.invalidation_hook_.is_linked()) {
            invalidations_.erase(auth);
        }
        queue_binding(auth);
        lock.restore();
        // Relation detach and sponsored storage refund are external callbacks.
        // The auth is indexed in the active operation, so the
        // unlocked drain can safely finish it without making lock_ reentrant.
        finish_bindings();
        return (VmStatus::Complete);
    }

    if (!keep_maps && !auth.views_.empty()) {
        auto& view = auth.views_.front();
        (void)view.close();
        if (!view.idle()) { waiting_view_ = &view; return VmStatus::Pending; }
        waiting_view_ = nullptr;
        released = std::move(view.pages_);
        auth.views_.erase(view); view.backing_ = nullptr;
        return VmStatus::Pending;
    }
    auto mutation = tlb_.begin();

    auto& retire = transaction().flush_;
    auto& editor = *root_;
    usize remaining = 8;
    auto* page = keep_maps ? auth.pages_.lower_bound(auth.trim_cursor_) : auth.pages_.minimum();
    while (page && remaining) {
        --remaining;
        auto* next = auth.pages_.next(*page);
        if (keep_maps) auth.trim_cursor_ = va_t{page->address_.raw() + page_size};
        if (keep_maps && !trim.contains(page->object_page_)) {
            page = next;
            continue;
        }
        auth.pages_.erase(*page);
        const auto virtual_page = VPage::from_base(page->address_);
        libk_assert(virtual_page);
        auto unmapped = editor.unmap(*virtual_page);
        libk_assert(unmapped);
        auto folded = fold_usage(*page, unmapped->usage);
        libk_assert(folded);
        for (auto& table : unmapped.value().tables) {
            retire_table(retire, std::move(table));
        }
        queue_page(*page);
        page = next;
    }
    if (keep_maps) auth.trim_scanned_ = !page;
    else if (auth.pages_.empty()) {
        // No Map can retire its Backing before all installed pages and
        // borrowed consumers have left. Each batch owns a separate receipt.
        if (auth.views_.empty()) {
            for (auto& mapping : auth.mappings_) {
                if (!remaining) break;
                if (!mapping.layout_hook_.is_linked()) continue;
                --remaining;
                unlink(mapping);
                queue_layout(mapping);
            }
        }
    }

    if (active_->pages_ == nullptr) {
        mutation.abort();
        const bool drained = active_->drain_pages();
        lock.restore();
        if (drained) finish_bindings();
        return drained ? VmStatus::Complete : VmStatus::Pending;
    }
    auto committed = commit_flush(std::move(mutation), ctx, retire);
    lock.restore();
    if (committed == VmStatus::Complete) {
        finish_bindings();
    }
    return committed;
}

auto VSpace::step(VmCtx ctx) noexcept -> bool {

    bool edit{};
    {
        sync::Lock guard{lock_};
        edit = active_ && active_->edit_ && !editing_ && !draining_ &&
            (!active_->edit_->batch || (active_->flushed_ && active_->retired_.empty()));
    }
    if (edit) {
        (void)advance_edit(ctx);
        finish_bindings();
        complete_cleanup();
        return work_ready();
    }
    Backing* next{};
    bool retire_root{};
    Flush* waiting_flush{};
    bool waiting{};
    {
        sync::Lock guard{lock_};
        if (editing_ || draining_) return false;
        if (active_ && !active_->drain_pages()) {
            if (active_->flush_.submitted()) {
                waiting_flush = &active_->flush_;
            } else {
                waiting = true;
            }
        }
    }
    if (waiting_flush) {
        libk_assert(ctx.cpus);
        if (waiting_flush->kick(*ctx.cpus)) {
            ipi_retries_ = 0;
            return false;
        }
        libk_assert(++ipi_retries_ < 8);
        return true;
    }
    if (waiting) {
        return false;
    }

    // This drains external Memory/Grant relations and sponsored node storage.
    // It owns its short internal lock sections and must be entered unlocked.
    finish_bindings();
    {
        sync::Lock guard{lock_};
        if (active_ || editing_ || draining_) {
            waiting = true;
        } else if (!invalidations_.empty()) {
            next = &invalidations_.front();
        } else if (state_ == VSpaceState::Stopping && bindings_ == 0 && !layout_.empty() && tlb_.active_cpus().empty()) {
            retire_root = true;
        } else {
            try_finish_retire();
        }
    }
    complete_cleanup();
    if (waiting || (next == nullptr && !retire_root)) {
        return false;
    }
    auto started = retire_root
        ? clear(ctx, VRange{va_t{UserBegin}, UserEnd - UserBegin})
        : start_invalidation(ctx, *next);
    if (!started && started.error() == VSpaceError::Busy) return false;
    libk_assert(started);
    if (*started == VmStatus::Complete) finish_bindings();
    complete_cleanup();
    { sync::Lock guard{lock_}; if (waiting_view_ && !waiting_view_->idle()) return false; }
    return *started != VmStatus::Complete || pending();
}

auto VSpace::pending() const noexcept -> bool {
    sync::Lock guard{lock_};
    return pending_locked();
}

auto VSpace::pending_locked() const noexcept -> bool {
    libk_assert(lock_.held());
    return editing_ || draining_ || active_ || !invalidations_.empty() ||
           state_ == VSpaceState::Stopping;
}

void VSpace::flush_ready() noexcept { work_->post(job_); }

auto VSpace::work_ready() const noexcept -> bool {
    sync::Lock guard{lock_};
    if (editing_ || draining_ || (waiting_view_ && !waiting_view_->idle())) return false;
    if (active_) {
        return active_->flushed_ ? active_->ready() : active_->flush_.complete();
    }
    return !invalidations_.empty() ||
           (state_ == VSpaceState::Stopping && bindings_ == 0 && tlb_.active_cpus().empty());
}

void VSpace::try_finish_retire() noexcept {
    if (state_ != VSpaceState::Stopping || bindings_ != 0 || active_ || editing_ || draining_ || !invalidations_.empty() || !tlb_.active_cpus().empty() || !layout_.empty()) {
        return;
    }
    if (root_) release_root();
    work_->close(job_);
    state_ = VSpaceState::Quiescent;
}

void VSpace::complete_cleanup() noexcept {
    object::cleanup cleanup{};
    {
        sync::Lock guard{lock_};
        if (state_ != VSpaceState::Quiescent || !cleanup_) return;
        if (!settled_.drained()) {
            guard.restore();
            settled_.close();
            return;
        }
        cleanup = std::move(*cleanup_);
        cleanup_.reset();
    }
    cleanup.complete();
}

void VSpace::run_work() noexcept {
    object::ref<> operation;
    { sync::Lock guard{lock_}; operation = operation_; }
    if (operation && static_cast<VmOp*>(operation.get())->op().schedule()) return;
    advance();
}
void VSpace::advance() noexcept {
    auto provider = object::ref<>::retain_alive(job_.key());
    object::ref<> execution;
    bool changed;
    { sync::Lock guard{lock_};
      if (advancing_) { again_ = true; return; }
      advancing_ = true; execution = operation_;
      changed = observed_epoch_ != layout_epoch_; observed_epoch_ = layout_epoch_; }
    libk::scope_exit leave{[&]() noexcept {
        sync::Lock guard{lock_}; advancing_ = false;
        if (std::exchange(again_, false)) {
            if (active_) active_->state_.post(); else work_->post(job_);
        }
    }};
    if (changed) object::changed(job_.key());
    VmCtx ctx{};
    if (auto* cpu = Cpu::local()) {
        ctx = {cpu->cpus, cpu->id};
    } // Before CPU publication, no hardware root can have remote users.
    if (step(ctx) || work_ready()) {
        sync::Lock guard{lock_};
        if (active_) active_->state_.post(); else work_->post(job_);
    }
}

} // namespace mm
