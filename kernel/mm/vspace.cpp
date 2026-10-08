#include <algorithm>
#include <array>
#include <base/types.hpp>
#include <cap/cspace.hpp>
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

Fence::Fence(object::ref<>&& target, VSpace& space) noexcept : target_(std::move(target)), space_(&space) {
    receipt_.commit();
}

Fence::~Fence() noexcept { libk_assert(!receipt_.completion().attached() && !hook_.is_linked()); }

void Fence::start() noexcept {
    libk_assert(receipt_.completion().attached());
    space_->wait_pending(*this);
}

auto VSpace::fault(Thread& thread, VmCtx ctx, Virt address, Perm perm) noexcept -> FaultKind {
    libk_assert(ctx.cpus);
    for (;;) {
        PageReq req{thread, *ctx.cpus};
        auto result = fault(ctx, address, perm, &req.relation, &req, &PageReq::publish);
        const auto kind = result ? result->kind : fault_kind(result.error());
        if (kind == FaultKind::Busy) {
            object::ref<> source;
            usize index{};
            {
                sync::Lock guard{lock_};
                auto* map = find(VRange{Virt{address.raw() & ~(page_size - 1)}, page_size});
                if (map && map->kind_ == MapKind::Map && map->perms_.contains(perm)) {
                    auto& backing = *map->binding_;
                    if (backing.memory_work_ && !backing.memory_work_->range().empty()) {
                        auto ref = backing.memory_ref_.clone();
                        if (ref) source = std::move(*ref);
                        index = map->object_.base() + (address.raw() - map->range_.base().raw()) / page_size;
                    }
                }
            }
            if (source) {
                auto mem = source.as<Mem>();
                libk_assert(mem);
                auto ready = mem->get().populate(thread, *ctx.cpus, index);
                if (!ready) return fault_kind(ready.error());
                continue;
            }
        }
        const bool pending = kind == FaultKind::Pending;
        libk_assert(!pending || result->memory);
        const auto rc = req.wait(pending ? result->memory : nullptr);
        if (rc != WaitRc::Ready && rc != WaitRc::Dirty) return FaultKind::BackingFailed;
        if (!pending) return kind;
    }
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

const cap::GrantAttachmentOps Backing::grant_ops_{
    .invalidate = &Backing::invalidate_grant,
    .released = &Backing::released,
};

Backing::Backing(VSpace& owner, object::ref<>&& memory, Mem& object, Perms perms, bool private_write) noexcept
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
    cap::GrantWork grant;
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

void Backing::released(void* ctx) noexcept { auto& space = *static_cast<Backing*>(ctx)->owner_; space.work_->post(space.job_); }

void Backing::invalidate_grant(void* ctx, cap::GrantWork&& work) noexcept {
    auto& auth = *static_cast<Backing*>(ctx);
    auth.owner_->request_invalidation(auth, std::move(work));
}

auto VSpace::prepare(const object::ref<>& payer, Pmm& pmm, KSpace& kernel, WorkQueue& work) noexcept
    -> std::expected<Data, VSpaceError> {
    object::ref<> source;
    resource::Charge charge;
    if (payer) {
        auto cloned = payer.clone();
        if (!cloned) return std::unexpected(VSpaceError::ResourceExhausted);
        source = std::move(*cloned);
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
    libk_assert(waiters_.empty());
    libk_assert(state_ == VSpaceState::Quiescent);
    libk_assert(layout_.empty());
    libk_assert(!root_);
    libk_assert(!editing_ && !draining_ && retired_.empty());
    libk_assert(invalidations_.empty());
    libk_assert(!receipt_ && !cleanup_);

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

auto VSpace::prepare_retire() noexcept -> bool {
    sync::Lock guard{lock_};
    if (state_ == VSpaceState::Stopping || bindings_ != 0) {
        return false;
    }
    state_ = VSpaceState::Stopping;
    return true;
}

auto VSpace::can_destroy_object(cap::VmLimit auth) const noexcept -> bool {
    return auth.range.contains(VRange{Virt{UserBegin}, UserEnd - UserBegin});
}

auto VSpace::root() noexcept -> Root {
    libk_assert(root_);
    return Root{tlb_, root_->cpu_root()};
}

auto VSpace::commit_flush(Tlb::Edit&& mutation, VmCtx ctx, Flush& retire, resource::Charge& refund,
                          bool instruction_sync) noexcept -> VmStatus {
    libk_assert(receipt_);
    const bool complete = mutation.commit(retire, ctx.cpus, ctx.local, instruction_sync);
    if (complete) {
        if (finish_pending(refund)) {
            return (VmStatus::Complete);
        }
        return (VmStatus::Pending);
    }
    work_->post(job_);
    return (VmStatus::Pending);
}

void VSpace::queue_layout(Map& node) noexcept {
    libk_assert(node.pending_next_ == nullptr);
    node.pending_next_ = receipt_->maps;
    receipt_->maps = &node;
}

void VSpace::queue_page(MapPage& page) noexcept {
    libk_assert(page.pending_next_ == nullptr);
    page.pending_next_ = receipt_->pages;
    receipt_->pages = &page;
}

void VSpace::queue_binding(Backing& auth) noexcept {
    if (!auth.retired_hook_.is_linked()) retired_.push_back(auth);
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
        if (draining_) return;
        draining_ = true;
    }
    libk::scope_exit completed{[this]() noexcept {
        bool wake{};
        {
            sync::Lock lock{lock_};
            draining_ = false;
            try_finish_retire();
            wake = state_ != VSpaceState::Quiescent &&
                   (!invalidations_.empty() || !retired_.empty() || state_ == VSpaceState::Stopping);
        }
        if (wake) work_->post(job_);
        finish_waiters();
    }};
    for (;;) {
        MemWork work;
        {
            sync::Lock guard{lock_};
            if (receipt_ || editing_) break;
            for (auto& auth : invalidations_) {
                if (!auth.memory_work_ || auth.memory_work_->range().empty() || auth.grant_work_) continue;
                bool resident{};
                for (auto* p = auth.pages_.minimum(); p; p = auth.pages_.next(*p))
                    if (auth.memory_work_->range().contains(p->object_page_)) {
                        resident = true;
                        break;
                    }
                if (resident) continue;
                work = std::move(*auth.memory_work_);
                auth.memory_work_.reset();
                invalidations_.erase(auth);
                break;
            }
        }
        if (!work) break;
        work.reset();
    }
    for (;;) {
        Backing* auth{};
        {
            sync::Lock guard{lock_};
            if (retired_.empty()) return;
            auth = &retired_.front();
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
            retired_.erase(*auth);
        }
        binding_pool_.destroy(*auth);
    }
}

auto VSpace::finish_pending(resource::Charge& refund) noexcept -> bool {
    if (!receipt_) {
        try_finish_retire();
        return true;
    }
    if (!receipt_->flush.complete()) {
        return false;
    }
    // Refund after dropping lock_; keep the batch until every relation drains.
    libk_assert(receipt_->flush.release(refund));
    while (receipt_->pages != nullptr) {
        MapPage* const page = receipt_->pages;
        MapPage* const next = page->pending_next_;
        release_page(*page, refund);
        receipt_->pages = next;
    }
    while (receipt_->maps != nullptr) {
        Map* const node = receipt_->maps;
        receipt_->maps = node->pending_next_;
        node->pending_next_ = nullptr;
        destroy_layout(*node);
    }
    receipt_.reset();
    for (auto& wait : waiters_) wait.ready_ = true;
    try_finish_retire();
    return true;
}

void VSpace::wait_pending(mm::Fence& wait) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!wait.hook_.is_linked());
        if (receipt_) {
            waiters_.push_back(wait);
            return;
        }
        wait.ready_ = true;
    }
    wait.receipt_.signal();
}

void VSpace::finish_waiters() noexcept {
    for (;;) {
        mm::Fence* ready{};
        {
            sync::Lock guard{lock_};
            for (auto& wait : waiters_) {
                if (wait.ready_) {
                    ready = &wait;
                    break;
                }
            }
            if (ready == nullptr) return;
            waiters_.erase(*ready);
        }
        // Dequeue transfers publication ownership; cancellation must now wait
        // for signal, even though the transaction result is already ready.
        ready->receipt_.signal();
    }
}

void VSpace::retire(object::cleanup&& cleanup) noexcept {
    bool can_start{};
    {
        sync::Lock guard{lock_};
        libk_assert(state_ == VSpaceState::Stopping);
        libk_assert(bindings_ == 0);
        libk_assert(!cleanup_);
        [[maybe_unused]] auto& retained = cleanup_.emplace(std::move(cleanup));
        if (!root_) {
            libk_assert(layout_.empty() && !receipt_ && !editing_);
            work_->close(job_);
            state_ = VSpaceState::Quiescent;
            guard.restore();
            complete_cleanup();
            return;
        }
        can_start = !receipt_ && !editing_ && tlb_.active_cpus().empty();
    }
    if (can_start) {
        static_cast<void>(clear(VmCtx{.local = CpuId{0}}, VRange{Virt{UserBegin}, UserEnd - UserBegin}));
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
    if (state_ != VSpaceState::Live || editing_ || receipt_) return std::unexpected(VSpaceError::Busy);
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
        wake = (receipt_ && receipt_->flush.complete()) || !invalidations_.empty() || !retired_.empty() ||
               state_ == VSpaceState::Stopping;
    }
    // Submit from actual obligations, after local rollback and outside lock_.
    if (wake) work_->post(job_);
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
    layout_.insert(*made->object);
    rollback.release();
    return {};
}

Borrow::~Borrow() noexcept { libk_assert(!hook_.is_linked()); }

void View::Drop::operator()(Borrow* borrow) const noexcept { borrow->owner_->detach_view(*borrow); }
bool View::valid() const noexcept { return h_ && h_.get()->owner_->view_active(*h_.get()); }

auto VSpace::bind_view(ViewReq&& request) noexcept -> std::expected<View, VSpaceError> {
    const auto page_count = request.virtual_range.page_count();
    if (!request.memory || !valid_perms(request.perms) || !valid_user_range(request.virtual_range) ||
        !page_count || request.object.size() != *page_count) {
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
        if (!current->layout_hook_.is_linked() || !current->range_.contains(request.virtual_range) ||
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
    {
        sync::Lock guard{lock_};
        if (state_ != VSpaceState::Live || mapping->binding_->invalid())
            return std::unexpected(VSpaceError::InvalidMapping);
        mapping->binding_->views_.push_back(*relation);
        rollback.release();
    }
    return View{*relation};
}

void VSpace::detach_view(Borrow& borrow) noexcept {
    {
        sync::Lock lock{lock_};
        if (borrow.hook_.is_linked()) borrow.backing_->views_.erase(borrow);
        borrow.backing_ = nullptr;
    }
    views_.destroy(borrow);
}

bool VSpace::view_active(const Borrow& borrow) const noexcept {
    sync::Lock lock{lock_};
    return borrow.hook_.is_linked();
}

bool VSpace::borrowed(const Map& map, VRange range) const noexcept {
    if (!map.binding_) return false;
    for (const auto& borrow : map.binding_->views_)
        if (borrow.range_.intersects(range)) return true;
    return false;
}

void VSpace::invalidate_views(Map& map) noexcept {
    if (!map.binding_) return;
    auto& views = map.binding_->views_;
    for (auto it = views.begin(); it != views.end();) {
        auto& borrow = *it++;
        if (borrow.range_.intersects(map.range_)) {
            views.erase(borrow);
            borrow.backing_ = nullptr;
        }
    }
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

auto VSpace::map(VmCtx ctx, MapReq request, object::ref<>&& ref, Mem& mem, cap::MemLimit auth) noexcept
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

auto VSpace::map_impl(VmCtx ctx, MapReq request, object::ref<>&& memory_ref, Mem& memory,
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
    if (!memory_ref || memory_ref.kind() != object::ObjectKind::Mem || count == 0 ||
        request.object.size() != count || !request.object.within(memory.page_count()) ||
        !mem_auth.range.contains(request.object) || !ceiling.contains(request.perms) ||
        (private_write &&
         (!request.perms.contains(Perm::Write) || !mem_auth.perms.contains(Perm::Read) ||
          memory.kind() != BackingKind::Pager || memory.seal_state() != SealState::Executable))) {
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

    MapPage* prepared_head{};
    MapPage* prepared_tail{};
    auto pages = libk::scope_exit{[&]() noexcept {
        while (prepared_head != nullptr) {
            MapPage* const page = prepared_head;
            prepared_head = page->pending_next_;
            page->pending_next_ = nullptr;

            page->binding_ = nullptr;
            pages_.destroy(*page);
        }
    }};

    // Pageable content is resolved by faults, including pages another address
    // space is currently loading or reclaiming. Map only establishes the
    // authorized range; it must not depend on a transient cache snapshot.
    const usize resident_pages = memory.kind() == BackingKind::Pager ? 0 : count;
    for (usize index = 0; index < resident_pages; ++index) {
        const usize object_page = request.object.base() + index;
        auto content = memory.query(object_page);
        if (!content) {
            return std::unexpected(memory_error(content.error()));
        }
        if (content.value() == ContentState::Zero) {
            continue;
        }
        if (content.value() == ContentState::Busy) {
            return std::unexpected(VSpaceError::Busy);
        }
        if (content.value() == ContentState::Failed) {
            return std::unexpected(VSpaceError::BackingFailed);
        }
        auto resident = memory.materialize(object_page);
        if (!resident) {
            return std::unexpected(memory_error(resident.error()));
        }
        PageHold source_page = std::move(resident).value();
        const Perms source_access = private_write ? Perms::of(Perm::Read) : request.perms;
        if (!source_page.perms().contains(source_access)) {
            return std::unexpected(VSpaceError::InvalidAuthority);
        }
        if (!mm::PageTable::user_perms(source_access)) {
            return std::unexpected(VSpaceError::InvalidAccess);
        }

        const Virt address{request.virtual_range.base().raw() + index * page_size};
        auto page_entry = pages_.create(payer_, address, object_page, std::move(source_page));
        if (!page_entry) {
            return std::unexpected(node_error(page_entry.error()));
        }
        MapPage* const page = page_entry.value().object;
        page->binding_ = auth;
        if (prepared_tail != nullptr) {
            prepared_tail->pending_next_ = page;
        } else {
            prepared_head = page;
        }
        prepared_tail = page;
    }

    auto table_reserve = reserve_tables(prepared_head);
    if (!table_reserve) {
        return std::unexpected(table_reserve.error());
    }
    TableReserve tables = std::move(table_reserve).value();

    sync::Lock lock{lock_};
    if (state_ != VSpaceState::Live || auth->invalid()) {
        return std::unexpected(VSpaceError::InvalidState);
    }

    if (prepared_head == nullptr) {

        auth->mappings_.push_back(*mapping);
        layout_.insert(*mapping);
        layout.release();
        backing.release();
        return (MapResult{key, VmStatus::Complete});
    }

    auto mutation = tlb_.begin();

    auth->mappings_.push_back(*mapping);
    layout_.insert(*mapping);
    auto& editor = *root_;
    while (prepared_head != nullptr) {
        MapPage* const page = prepared_head;
        prepared_head = page->pending_next_;
        page->pending_next_ = nullptr;
        const auto virtual_page = VPage::from_base(page->address_);
        libk_assert(virtual_page);
        const auto permissions =
            mm::PageTable::user_perms(private_write ? Perms::of(Perm::Read) : request.perms);
        libk_assert(permissions);
        auto installed = editor.map(*virtual_page, page->page(), *permissions, tables.pages);
        libk_assert(installed);
        auth->pages_.insert(*page);
    }
    commit_tables(tables);
    layout.release();
    backing.release();
    auto& retire = receipt_.emplace(*pmm_, *this).flush;
    resource::Charge refund{};
    auto committed =
        commit_flush(std::move(mutation), ctx, retire, refund, request.perms.contains(Perm::Execute));
    lock.restore();
    refund.reset();
    editing.reset();
    if (committed == VmStatus::Complete) {
        finish_bindings();
    }
    return (MapResult{key, committed});
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
    {
        sync::Lock lock{lock_};
        if (!clear) {
            auto admitted = begin_edit(range, false);
            if (!admitted) return std::unexpected(admitted.error());
        } else {
            if (!valid_user_range(range)) return std::unexpected(VSpaceError::InvalidRange);
            if (state_ != VSpaceState::Live && state_ != VSpaceState::Stopping)
                return std::unexpected(VSpaceError::InvalidState);
            if (editing_ || receipt_) return std::unexpected(VSpaceError::Busy);
            editing_ = true;
        }
    }
    Edit editing{this};
    Map* first{};
    Map* last{};
    if (perms && (!valid_perms(*perms) || (perms->contains(Perm::Write) && perms->contains(Perm::Execute))))
        return std::unexpected(VSpaceError::InvalidAccess);
    {
        sync::Lock lock{lock_};
        auto cursor = range.base();
        auto* n = overlap(range);
        for (; n && n->range_.base() < *range.limit(); n = layout_.next(*n)) {
            if (!clear && (!n->binding_ || n->range_.base() > cursor)) {
                return std::unexpected(VSpaceError::NotMapped);
            }
            if (!n->layout_hook_.is_linked() || (!clear && borrowed(*n, range))) {
                return std::unexpected(VSpaceError::Busy);
            }
            if (perms) {
                if (!n->ceiling_.contains(*perms)) {
                    return std::unexpected(VSpaceError::InvalidAccess);
                }
                auto& backing = *n->binding_;
                const auto lo = std::max(n->range_.base(), range.base());
                const auto hi = std::min(*n->range_.limit(), *range.limit());
                for (auto* page = backing.pages_.lower_bound(lo); page && page->address_ < hi;
                     page = backing.pages_.next(*page)) {
                    if (!page->perms().contains(*perms)) {
                        return std::unexpected(VSpaceError::InvalidAccess);
                    }
                    if (!PageTable::user_perms(*perms)) {
                        return std::unexpected(VSpaceError::InvalidAccess);
                    }
                }
            }
            if (!first) first = n;
            last = n;
            cursor = *n->range_.limit();
        }
        if (!clear && cursor < *range.limit()) {
            return std::unexpected(VSpaceError::NotMapped);
        }
        if (!first) {
            try_finish_retire();
            return VmStatus::Complete;
        }
    }

    // Only the two outside tails need new records. The selected middle keeps
    // its backing identity; no duplicate selected fragments or state arrays.
    std::array<Map*, 2> tails{};
    auto discard = libk::scope_exit{[&]() noexcept {
        for (auto* tail : tails)
            if (tail) mappings_.destroy(*tail);
    }};
    auto tail = [&](usize i, Map& src, VRange r) -> std::expected<void, VSpaceError> {
        auto made = make_fragment(src, r, src.perms());
        if (!made) return std::unexpected(made.error());
        tails[i] = *made;
        return {};
    };
    if (first->range_.base() < range.base()) {
        auto made =
            tail(0, *first, VRange{first->range_.base(), range.base().raw() - first->range_.base().raw()});
        if (!made) return std::unexpected(made.error());
    }
    if (*last->range_.limit() > *range.limit()) {
        auto made =
            tail(1, *last, VRange{*range.limit(), last->range_.limit()->raw() - range.limit()->raw()});
        if (!made) return std::unexpected(made.error());
    }

    resource::Charge refund{};
    sync::Lock lock{lock_};
    if ((!clear && state_ != VSpaceState::Live) ||
        (clear && state_ != VSpaceState::Live && state_ != VSpaceState::Stopping))
        return std::unexpected(VSpaceError::InvalidState);
    for (auto* n = first; !clear && n; n = n == last ? nullptr : layout_.next(*n))
        if (n->binding_ && n->binding_->invalid()) return std::unexpected(VSpaceError::Busy);
    auto mutation = tlb_.begin();

    auto& retire = receipt_.emplace(*pmm_, *this).flush;
    bool changed{};
    for (auto* n = first; n;) {
        auto* next = n == last ? nullptr : layout_.next(*n);
        const auto lo = std::max(n->range_.base(), range.base());
        const auto hi = std::min(*n->range_.limit(), *range.limit());
        layout_.erase(*n);
        if (n->binding_) {
            const auto offset = (lo.raw() - n->range_.base().raw()) / page_size;
            n->object_ = ObjectRange{n->object_.base() + offset, (hi.raw() - lo.raw()) / page_size};
        }
        n->range_ = VRange{lo, hi.raw() - lo.raw()};
        if (clear) invalidate_views(*n);
        if (perms) {
            n->perms_ = *perms;
            layout_.insert(*n);
        } else {
            queue_layout(*n);
        }
        if (n->binding_) {
            auto& backing = *n->binding_;
            for (auto* page = backing.pages_.lower_bound(lo); page && page->address_ < hi;) {
                auto* after = backing.pages_.next(*page);
                auto vp = VPage::from_base(page->address_);
                libk_assert(vp);
                if (perms) {
                    const auto p = PageTable::user_perms(
                        backing.private_write_ && !page->private_owned() ? Perms::of(Perm::Read) : *perms);
                    libk_assert(p);
                    auto old = root_->protect(*vp, *p);
                    libk_assert(old);
                    auto folded = fold_usage(*page, *old);
                    libk_assert(folded);
                } else {
                    backing.pages_.erase(*page);
                    auto old = root_->unmap(*vp);
                    libk_assert(old);
                    auto folded = fold_usage(*page, old->usage);
                    libk_assert(folded);
                    for (auto& table : old->tables) retire_table(retire, std::move(table));
                    queue_page(*page);
                }
                changed = true;
                page = after;
            }
        }
        n = next;
    }
    for (auto*& n : tails)
        if (n) {
            if (n->binding_) n->binding_->mappings_.push_back(*n);
            layout_.insert(*n);
            n = nullptr;
        }
    if (!changed) {
        mutation.abort();
        libk_assert(finish_pending(refund));
        lock.restore();
        refund.reset();
        editing.reset();
        finish_bindings();
        return VmStatus::Complete;
    }
    auto committed = commit_flush(std::move(mutation), ctx, retire, refund);
    lock.restore();
    refund.reset();
    editing.reset();
    if (committed == VmStatus::Complete) finish_bindings();
    return committed;
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

auto VSpace::fault(VmCtx ctx, Virt address, Perm perms, WaitRelation* relation, void* owner,
                   WaitRelation::Publish publish) noexcept -> std::expected<FaultResult, VSpaceError> {
    const usize aligned = address.raw() & ~(page_size - 1);
    const VRange page_range{Virt{aligned}, page_size};
    if (!valid_user_range(page_range)) {
        return (FaultResult{.kind = FaultKind::NoMapping});
    }

    Map* mapping{};
    MapPage* private_source{};
    usize object_page{};
    {
        sync::Lock guard{lock_};
        if (state_ != VSpaceState::Live || receipt_ || editing_) {
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
    return materialize_fault(ctx, *mapping, page_range.base(), object_page, relation, owner, publish);
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
    auto& retire = receipt_.emplace(*pmm_, *this).flush;
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
    queue_page(source);
    resource::Charge refund{};
    auto committed = commit_flush(std::move(mutation), ctx, retire, refund);
    lock.restore();
    refund.reset();
    editing.reset();
    if (committed == VmStatus::Complete) finish_bindings();
    return (FaultResult{.kind = FaultKind::Materialized,
                        .mapping = mapping_key,
                        .object_page = object_page,
                        .status = committed});
}

auto VSpace::materialize_fault(VmCtx ctx, Map& mapping, Virt page_address, usize object_page,
                               WaitRelation* relation, void* owner, WaitRelation::Publish publish) noexcept
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
    auto resident = memory->materialize(object_page, relation, owner, publish);
    if (!resident) {
        if (resident.error() == MemErr::Pending) {
            auto pending = fail(FaultKind::Pending);
            if (pending && relation != nullptr) {
                pending.value().memory = memory;
            }
            return pending;
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
    rollback.release();
    auto& retire = receipt_.emplace(*pmm_, *this).flush;
    resource::Charge refund{};
    auto committed =
        commit_flush(std::move(mutation), ctx, retire, refund, mapping.perms_.contains(Perm::Execute));
    lock.restore();
    refund.reset();
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

auto VSpace::sample_usage(VmCtx ctx, Virt address, bool clear) noexcept
    -> std::expected<PageUsage, VSpaceError> {
    const usize aligned = address.raw() & ~(page_size - 1);
    const VRange page_range{Virt{aligned}, page_size};
    if (!valid_user_range(page_range)) {
        return std::unexpected(VSpaceError::InvalidRange);
    }

    sync::Lock lock{lock_};
    auto fail = [&](VSpaceError error) -> std::expected<PageUsage, VSpaceError> {
        lock.restore();
        return std::unexpected(error);
    };
    if (state_ != VSpaceState::Live || receipt_ || editing_) {
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
    auto& retire = receipt_.emplace(*pmm_, *this).flush;
    resource::Charge refund{};
    auto committed = commit_flush(std::move(mutation), ctx, retire, refund);
    lock.restore();
    refund.reset();
    if (committed != VmStatus::Complete) return std::unexpected(VSpaceError::Busy);
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
        libk_assert(!auth.memory_work_);
        [[maybe_unused]] auto& retained = auth.memory_work_.emplace(std::move(work));
        if (!auth.invalidation_hook_.is_linked()) {
            invalidations_.push_back(auth);
        }
    }
    work_->post(job_);
}

void VSpace::request_invalidation(Backing& auth, cap::GrantWork&& work) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!auth.grant_work_);
        [[maybe_unused]] auto& retained = auth.grant_work_.emplace(std::move(work));
        if (!auth.invalidation_hook_.is_linked()) {
            invalidations_.push_back(auth);
        }
    }
    work_->post(job_);
}

auto VSpace::start_invalidation(VmCtx ctx, Backing& auth) noexcept -> std::expected<VmStatus, VSpaceError> {
    resource::Charge refund{};
    sync::Lock lock{lock_};
    if (receipt_ || editing_ || draining_) {
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
        // The auth was published to retired_ above, so the
        // unlocked drain can safely finish it without making lock_ reentrant.
        finish_bindings();
        return (VmStatus::Complete);
    }

    auto mutation = tlb_.begin();

    auto& retire = receipt_.emplace(*pmm_, *this).flush;
    if (!keep_maps) {
        if (auth.invalidation_hook_.is_linked()) invalidations_.erase(auth);
        for (auto& mapping : auth.mappings_) {
            invalidate_views(mapping);
            if (mapping.layout_hook_.is_linked()) layout_.erase(mapping);
            queue_layout(mapping);
        }
    }
    auto& editor = *root_;
    for (auto* page = auth.pages_.minimum(); page;) {
        auto* next = auth.pages_.next(*page);
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

    if (receipt_->pages == nullptr) {
        mutation.abort();
        libk_assert(finish_pending(refund));
        lock.restore();
        refund.reset();
        finish_bindings();
        return (VmStatus::Complete);
    }
    auto committed = commit_flush(std::move(mutation), ctx, retire, refund);
    lock.restore();
    refund.reset();
    if (committed == VmStatus::Complete) {
        finish_bindings();
    }
    return committed;
}

auto VSpace::step(VmCtx ctx) noexcept -> bool {

    Backing* next{};
    bool retire_root{};
    Flush* waiting_flush{};
    bool waiting{};
    resource::Charge refund{};
    {
        sync::Lock guard{lock_};
        if (receipt_ && !finish_pending(refund)) {
            if (receipt_->flush.submitted()) {
                waiting_flush = &receipt_->flush;
            } else {
                waiting = true;
            }
        }
    }
    refund.reset();
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
        if (receipt_ || editing_ || draining_) {
            waiting = true;
        } else if (!invalidations_.empty()) {
            next = &invalidations_.front();
        } else if (state_ == VSpaceState::Stopping && !layout_.empty() && tlb_.active_cpus().empty()) {
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
        ? clear(ctx, VRange{Virt{UserBegin}, UserEnd - UserBegin})
        : start_invalidation(ctx, *next);
    if (!started && started.error() == VSpaceError::Busy) return false;
    libk_assert(started);
    if (*started == VmStatus::Complete) finish_bindings();
    complete_cleanup();
    return *started != VmStatus::Complete || pending();
}

auto VSpace::pending() const noexcept -> bool {
    sync::Lock guard{lock_};
    return editing_ || draining_ || receipt_ || !invalidations_.empty() || !retired_.empty() ||
           state_ == VSpaceState::Stopping;
}

void VSpace::flush_ready() noexcept { work_->post(job_); }

auto VSpace::work_ready() const noexcept -> bool {
    sync::Lock guard{lock_};
    if (editing_ || draining_) return false;
    if (receipt_) {
        return receipt_->flush.complete();
    }
    return !invalidations_.empty() || !retired_.empty() ||
           (state_ == VSpaceState::Stopping && tlb_.active_cpus().empty());
}

void VSpace::try_finish_retire() noexcept {
    if (state_ != VSpaceState::Stopping || receipt_ || editing_ || draining_ || !invalidations_.empty() ||
        !retired_.empty() || !tlb_.active_cpus().empty() || !root_ || !layout_.empty()) {
        return;
    }
    release_root();
    work_->close(job_);
    state_ = VSpaceState::Quiescent;
}

void VSpace::complete_cleanup() noexcept {
    object::cleanup cleanup{};
    {
        sync::Lock guard{lock_};
        if (state_ != VSpaceState::Quiescent || !cleanup_) return;
        cleanup = std::move(*cleanup_);
        cleanup_.reset();
    }
    cleanup.complete();
}

void VSpace::run_work() noexcept {
    VmCtx ctx{};
    if (auto* entry = arch::local(); entry && entry->owner) {
        auto& cpu = *entry->owner;
        ctx = {cpu.cpus, cpu.id};
    } // Before CPU publication, no hardware root can have remote users.
    if (step(ctx) || work_ready()) work_->post(job_);
}

} // namespace mm
