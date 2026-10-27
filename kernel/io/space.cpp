#include <io.hpp>
#include <io/space.hpp>
#include <cpu/cpu.hpp>
#include <ipc/notification.hpp>
#include <memory>
#include <algorithm>

namespace io {
Iommu::Iommu(mm::Pmm& pmm, mm::KSpace& vm, Cpus& cpus, WorkQueue& work, const time::Clock& clock,
         object::ref<> quota, mm::Phys::Claim&& claim, object::ref<irq::Ctl> irq_owner,
         irq::Line line, IommuConfig config) noexcept
    : vm_(vm), cpus_(cpus), work_(work), clock_(clock), quota_(std::move(quota)),
      claim_(std::move(claim)), irq_owner_(std::move(irq_owner)),
      irq_(line, irq::Route::Handler::bind<&Iommu::interrupt>(*this),
           irq::Route::Stop::bind<&Iommu::disconnected>(*this)), config_(config), storage_(pmm.group()) {}
Iommu::~Iommu() noexcept { libk_assert(!mapped_ && !domains_ && !cleanup_); }
auto Iommu::state() const noexcept -> IoState {
    sync::Lock lock{lock_};
    if (phase_ == Phase::Failed) return IO_SPACE_FAILED;
    if (phase_ == Phase::Closed) return IO_SPACE_CLOSED;
    if (closing_) return IO_SPACE_CLOSING;
    return phase_ == Phase::Ready ? IO_SPACE_READY : IO_SPACE_PENDING;
}
auto Iommu::page() noexcept -> std::expected<mm::Page, status_t> {
    auto fee = resource::acquire(quota_, {.memory = mm::page_size});
    if (!fee) return std::unexpected(STATUS_NO_MEMORY);
    auto page = storage_.allocate();
    if (!page) return std::unexpected(STATUS_NO_MEMORY);
    std::fill_n(storage_.bytes(*page), mm::page_size, byte{});
    fee_.merge(std::move(*fee));
    return *page;
}
auto Iommu::context(u32 did, bool create) noexcept -> u64* {
    // Extended contexts: 6 DID bits per leaf, 9 per directory level.
    const auto levels = arch::iommu_levels(config_.device_bits);
    auto page = directory_;
    for (unsigned level = levels; level; --level) {
        auto* entry = reinterpret_cast<u64*>(storage_.bytes(page)) + arch::iommu_index(did, level);
        auto value = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
        if (!value) {
            if (!create) return nullptr;
            auto child = this->page();
            if (!child) return nullptr;
            value = arch::iommu_link(child->raw());
            __atomic_store_n(entry, value, __ATOMIC_RELEASE);
        }
        page = mm::Page{arch::iommu_page(value)};
    }
    return reinterpret_cast<u64*>(storage_.bytes(page)) + arch::iommu_index(did, 0);
}
auto Iommu::open(object::ref<> cpu) noexcept -> status_t {
    const auto range = mm::Pages::from_aligned_bytes(mm::pa_t{config_.address}, 4096);
    if (!range || !claim_.covers(*range)) return STATUS_BAD_ARGS;
    if (!vm_.map(mapping_, *range, cpus_, current_cpu().id)) return STATUS_NO_MEMORY;
    mapped_ = true;
    if (!arch::iommu_caps(mapping_.address())) return STATUS_INVALID_OP;
    mm::Page* slots[]{&directory_, &commands_, &faults_};
    for (auto* slot : slots) {
        auto allocated = page();
        if (!allocated) return allocated.error();
        *slot = *allocated;
    }
    work_.open(job_, Work::Fn::bind<&Iommu::progress>(*this));
    phase_ = Phase::Prepared;
    auto self = object::ref<>::retain_alive(job_.key());
    const auto admitted = initialization_.start(std::move(cpu), self);
    if (admitted != STATUS_OK) return admitted;
    return STATUS_OK;
}
auto Iommu::activate() noexcept -> status_t {
    sync::Lock guard{lock_};
    if (phase_ != Phase::Prepared || closing_ || initialization_.canceled()) return STATUS_CLOSED;
    status_t status = STATUS_CLOSED;
    auto commit = [&]() noexcept {
        if (!irq_.connect()) { status = STATUS_BUSY; return; }
        phase_ = Phase::Disabling;
        deadline_ = clock_.rd_ticks() + clock_.rd_freq();
        arch::iommu_off(mapping_.address());
        status = STATUS_OK;
    };
    static_cast<void>(bindings_.publish(Source::Action::bind(commit)));
    if (status == STATUS_OK) post();
    return status;
}
void Iommu::invalidated() noexcept {
    { sync::Lock guard{lock_}; close(); }
    auto self = object::ref<>::retain_alive(job_.key());
    static_cast<void>(self.destroy()); post();
}
void Iommu::links(u64 cursor, LinkRecords& out) const noexcept {
    bindings_.links(out, LINK_PHYS, cursor);
    for (auto& row : std::span{out.records, out.count}) {
        if (row.object_id == irq_owner_.id().value) { row.kind = LINK_ROUTE; row.first = config_.source; row.bytes = 1; }
        else { row.first = config_.address; row.bytes = config_.bytes; }
    }
}
auto Iommu::attach(Space& domain) noexcept -> status_t {
    sync::Lock lock{lock_};
    if (phase_ != Phase::Ready || closing_ || reassigning_) return STATUS_BUSY;
    for (auto* d = domains_; d; d = d->next_)
        if (d->requester_ == domain.requester_) return STATUS_BUSY;
    for (;;) {
        bool used{};
        for (auto* d = domains_; d; d = d->next_) used |= d->pscid_ == domain.pscid_;
        if (!used) break;
        if (++domain.pscid_ == (1U << 20)) return STATUS_NO_MEMORY;
    }
    if (!context(domain.requester_, true)) return STATUS_NO_MEMORY;
    domain.next_ = domains_;
    domains_ = &domain;
    return STATUS_OK;
}
void Iommu::detach(Space& domain) noexcept {
    sync::Lock lock{lock_};
    libk_assert(!inflight_ || inflight_->owner != &domain);
    auto** link = &domains_;
    while (*link != &domain) link = &(*link)->next_;
    *link = domain.next_;
    domain.next_ = nullptr;
    if (closing_) work_.post(job_);
}
auto Iommu::sync(IoOp::Receipt& receipt, bool enable) noexcept -> status_t {
    auto& domain = *receipt.owner;
    sync::Lock lock{lock_};
    if (reassigning_) return STATUS_PENDING;
    if (phase_ != Phase::Ready) return STATUS_BACKING_FAILED;
    const auto polled = arch::iommu_poll(mapping_.address(), inflight_ ? inflight_->tail : tail_);
    if (polled == arch::IoStep::Failed) { fail(); return STATUS_BACKING_FAILED; }
    if (inflight_) {
        if (polled == arch::IoStep::Pending) {
            if (clock_.rd_ticks() >= deadline_) { fail(); return STATUS_BACKING_FAILED; }
            return STATUS_PENDING;
        }
        if (inflight_ != &receipt) return STATUS_PENDING;
        inflight_ = nullptr;
        receipt.submitted = false;
        return STATUS_OK;
    }
    auto* dc = context(domain.requester_, false);
    libk_assert(dc);
    arch::iommu_context(dc, domain.pscid_, domain.root_->page().raw(), enable);
    arch::iommu_commands(reinterpret_cast<u64*>(storage_.bytes(commands_)), tail_, domain.requester_);
    receipt.tail = tail_;
    receipt.submitted = true;
    inflight_ = &receipt;
    deadline_ = clock_.rd_ticks() + clock_.rd_freq();
    arch::iommu_tail(mapping_.address(), tail_);
    return STATUS_PENDING;
}
void Iommu::fail() noexcept {
    phase_ = Phase::Failed;
    // Keep all potentially reachable storage and receipts. Failure is not drain.
    for (auto* d = domains_; d; d = d->next_) {
        d->failed_.store(true, std::memory_order_release);
        work_.post(d->job_);
    }
}
auto Iommu::interrupt() noexcept -> irq::Route::Delivery {
    irq_pending_.store(true, std::memory_order_release);
    work_.post(job_);
    return {false, {}};
}
auto Iommu::disconnected() noexcept -> Source::Publication {
    disconnected_.store(true, std::memory_order_release);
    work_.post(job_);
    return {};
}
void Iommu::retire(object::cleanup&& cleanup) noexcept {
    {
        sync::Lock lock{lock_};
        close();
        cleanup_ = std::move(cleanup);
        // Construction can fail before the protocol worker was opened.
        if (phase_ == Phase::Empty) work_.open(job_, Work::Fn::bind<&Iommu::progress>(*this));
    }
    work_.post(job_);
}
void Iommu::close() noexcept {
    libk_assert(lock_.held());
    if (closing_) return;
    closing_ = true;
    for (auto* d = domains_; d; d = d->next_) {
        d->closing_.store(true, std::memory_order_release);
        work_.post(d->job_);
    }
}
void Iommu::progress() noexcept {
    const auto phase = initialization_.query().sequence;
    if (phase && phase != OP_DONE && initialization_.schedule()) return;
    advance();
}
void Iommu::advance() noexcept {
    bool closed;
    { sync::Lock guard{lock_};
      if (advancing_) { post(); return; }
      advancing_ = true;
      closed = phase_ == Phase::Closed;
    }
    libk::scope_exit advanced{[&]() noexcept { sync::Lock guard{lock_}; advancing_ = false; }};
    // Binding invalidation can finish hardware shutdown before the pool's
    // retire hook delivers its cleanup owner. Closed is terminal in either case.
    if (closed) { complete(); return; }
    bool rearm{};
    {
        sync::Lock lock{lock_};
        if (reassigning_) return;
        // Losing interrupt delivery requests shutdown; controller completion is
        // polled and does not depend on that IRQ route remaining connected.
        if (disconnected_.load(std::memory_order_acquire) || initialization_.canceled()) close();
        if (phase_ == Phase::Failed) return;
        if (phase_ == Phase::Ready && irq_pending_.exchange(false, std::memory_order_acq_rel)) {
            const auto tail = arch::iommu_faults(mapping_.address());
            if (tail == UINT32_MAX) { fail(); return; }
            while (fault_head_ != tail) {
                const auto did = arch::iommu_fault(storage_.bytes(faults_), fault_head_);
                for (auto* d = domains_; d; d = d->next_) if (d->requester_ == did) {
                    d->closing_.store(true, std::memory_order_release);
                    work_.post(d->job_);
                }
                fault_head_ = (fault_head_ + 1) & (arch::IoFaults - 1);
            }
            arch::iommu_fault_head(mapping_.address(), fault_head_);
            rearm = true;
        }
        if (phase_ == Phase::Prepared) {
            if (!closing_) return;
            phase_ = Phase::Unmapping;
        }
        if (closing_ && domains_) return;
        if (closing_ && phase_ != Phase::Empty && phase_ != Phase::Stopping && phase_ != Phase::Unmapping) {
            arch::iommu_off(mapping_.address());
            deadline_ = clock_.rd_ticks() + clock_.rd_freq();
            phase_ = Phase::Stopping;
        }
        switch (phase_) {
        case Phase::Empty: phase_ = Phase::Unmapping; break;
        case Phase::Disabling: {
            const auto status = arch::iommu_idle(mapping_.address());
            if (status == arch::IoStep::Pending) break;
            if (status == arch::IoStep::Failed) { fail(); return; }
            if (!arch::iommu_queues(mapping_.address(), commands_.raw(), faults_.raw())) { fail(); return; }
            phase_ = Phase::Queues;
            break;
        }
        case Phase::Queues: {
            const auto status = arch::iommu_queued(mapping_.address());
            if (status == arch::IoStep::Pending) break;
            if (status == arch::IoStep::Failed) { fail(); return; }
            if (!initialization_.commit()) { close(); work_.post(job_); return; }
            auto commit = [&]() noexcept {
                arch::iommu_root(mapping_.address(), directory_.raw(), config_.device_bits);
                phase_ = Phase::Directory;
            };
            if (!bindings_.publish(Source::Action::bind(commit))) { close(); post(); return; }
            break;
        }
        case Phase::Directory: {
            const auto status = arch::iommu_active(mapping_.address(), directory_.raw(), config_.device_bits);
            if (status == arch::IoStep::Pending) break;
            if (status == arch::IoStep::Failed) { fail(); return; }
            arch::iommu_irq_enable(mapping_.address());
            phase_ = Phase::Ready;
            break;
        }
        case Phase::Stopping: {
            const auto status = arch::iommu_idle(mapping_.address());
            if (status == arch::IoStep::Pending) break;
            if (status == arch::IoStep::Failed) { fail(); return; }
            phase_ = Phase::Unmapping;
            break;
        }
        default: break;
        }
        if (phase_ != Phase::Ready && phase_ != Phase::Unmapping && clock_.rd_ticks() >= deadline_) { fail(); return; }
        if (phase_ != Phase::Ready && phase_ != Phase::Unmapping) { initialization_.post(); return; }
        if (phase_ == Phase::Ready) { if (closing_) work_.post(job_); }
    }
    // Route lock never nests inside the controller lock: IRQ dispatch posts work.
    if (rearm) {
        auto commit = [&]() noexcept { irq_.reset(); if (!irq_.connect()) disconnected(); };
        static_cast<void>(bindings_.publish(Source::Action::bind(commit)));
    }
    if (phase_ == Phase::Ready && initialization_.query().sequence != OP_DONE) {
        if (!initialization_.detach()) { post(); return; }
        initialization_.finish();
    }
    if (phase_ != Phase::Unmapping) return;
    irq_.reset();
    if (mapped_) {
        if (!flush_) { flush_.emplace(); vm_.unmap(mapping_, *flush_, cpus_, current_cpu().id); }
        if (!flush_->complete()) { (void)flush_->kick(cpus_); work_.post(job_); return; }
        vm_.release(mapping_, *flush_);
        flush_.reset();
        mapped_ = false;
    }
    for (usize i = 0; i < 8 && storage_.page_count(); ++i) {
        auto page = storage_.take(); libk_assert(page); page->reset();
    }
    if (storage_.page_count()) { post(); return; }
    fee_.reset();
    if (initialization_.query().sequence && initialization_.query().sequence != OP_DONE) {
        if (!initialization_.detach()) { post(); return; }
        initialization_.finish(STATUS_CANCELED);
    }
    claim_.reset();
    if (!bindings_.close()) return;
    { sync::Lock lock{lock_}; phase_ = Phase::Closed; }
    complete();
}
void Iommu::complete() noexcept {
    object::cleanup cleanup;
    { sync::Lock lock{lock_}; cleanup = std::move(cleanup_); }
    if (!cleanup) return;
    work_.close(job_);
    cleanup.complete();
}



IoOp::IoOp(object::ref<> provider) noexcept
    : owner_(static_cast<Space*>(provider.get())), state_(std::move(provider), Source::Action::bind<&Space::post>(*owner_), Source::Action::bind<&Space::advance>(*owner_)),
      receipt_{owner_}, pages_(owner_->pmm_.group()) {}
IoOp::IoOp(Space& owner, bool enable) noexcept
    : owner_(&owner), state_({}, Source::Action::bind<&Space::post>(owner), Source::Action::bind<&Space::advance>(owner), enable ? &owner.ordinary_ : nullptr), receipt_{owner_}, pages_(owner.pmm_.group()), enable_(enable) {}

auto Space::transaction(bool enable) noexcept -> IoOp& {
    libk_assert(lock_.held() && !active_);
    active_ = &control_.emplace(*this, enable);
    active_->state_.bind(work_, job_.key());
    active_->state_.accept(); libk_assert(active_->state_.commit());
    return *active_;
}
auto Space::accept(Op::Submission ctx, bool commit) noexcept -> status_t {
    libk_assert(lock_.held() && !active_);
    auto controller = ctl_grant_.acquire();
    if (!controller) return STATUS_CLOSED;
    auto install = [&] noexcept {
        operation_ = std::move(*ctx.execution);
        active_ = static_cast<IoOp*>(operation_.get());
    };
    std::array<const cap::GrantPin*, 1> grants{&*controller};
    const auto status = ctx.publish(Source::Action::bind(install), grants);
    if (status != STATUS_OK) return status;
    return !commit || active_->state_.commit() ? STATUS_OK : STATUS_CANCELED;
}
void Map::released(void* arg, mm::MemWork&& done) noexcept {
    auto& space = static_cast<Map*>(arg)->owner;
    sync::Lock guard{space.lock_}; done.reset(); space.post();
}
void Map::released(void* arg, object::LinkWork&& done) noexcept {
    auto& space = static_cast<Map*>(arg)->owner;
    sync::Lock guard{space.lock_}; done.reset(); space.post();
}

Map::Map(Space& domain, usize iova, usize n) noexcept
    : owner(domain), first(iova), count(n) {}
Map::~Map() noexcept {
    libk_assert(!mem_link.busy() && !grant.busy());
    libk_assert(!pages);
}
void Map::invalidate_mem(void* arg, mm::MemWork&& work) noexcept {
    auto& m = *static_cast<Map*>(arg);
    sync::Lock lock{m.owner.lock_};
    m.mem_work = std::move(work);
    m.owner.close();
}
void Map::invalidate_grant(void* arg, object::LinkWork&& work) noexcept {
    auto& m = *static_cast<Map*>(arg);
    sync::Lock lock{m.owner.lock_};
    m.grant_work = std::move(work);
    m.owner.close();
}
void Space::invalidated(void* arg, object::LinkWork&& work) noexcept {
    auto& d = *static_cast<Space*>(arg);
    sync::Lock lock{d.lock_};
    d.ctl_work_ = std::move(work);
    d.close();
}
Space::Space(mm::Pmm& pmm, WorkQueue& work, object::ref<> quota, object::ref<Iommu> ctl, u32 requester) noexcept
    : pmm_(pmm), work_(work), quota_(std::move(quota)), controller_(std::move(ctl)),
      requester_(requester), maps_(pmm), pins_(pmm), tables_(pmm.group()) {
    work_.open(job_, Work::Fn::bind<&Space::progress>(*this));
}
Space::~Space() noexcept { libk_assert(!self_ && mappings_.empty() && !attached_ && !cleanup_ && !active_ && !control_ && !operation_); }
auto Space::open(object::ref<> self, cap::Resolved<Iommu>& controller, object::ref<> cpu) noexcept -> status_t {
    preparing_ = true;
    libk::scope_exit prepared{[&]() noexcept {
        { sync::Lock lock{lock_}; preparing_ = false; }
        work_.post(job_);
    }};
    self_ = std::move(self);
    const auto records = maps_.prepay(quota_, 8), pins = pins_.prepay(quota_, 64);
    if (!records || !*records || !pins || !*pins) return STATUS_NO_MEMORY;
    auto fee = resource::acquire(quota_, {.memory = mm::page_size});
    if (!fee) return STATUS_NO_MEMORY;
    auto root = mm::PageTable::create(pmm_, mm::PageTable::Kind::Device);
    if (!root) return STATUS_NO_MEMORY;
    (void)root_.emplace(std::move(*root));
    fee_ = std::move(*fee);
    if (!controller.attach(ctl_grant_)) return STATUS_CLOSED;
    const auto result = controller_->attach(*this);
    if (result != STATUS_OK) return result;
    attached_ = true;
    active_ = &control_.emplace(*this, true);
    active_->state_.bind(work_, job_.key());
    const auto admitted = active_->state_.start(std::move(cpu), self_);
    if (admitted != STATUS_OK) {
        active_->state_.accept(); static_cast<void>(active_->state_.cancel());
        active_->enable_ = false; close(); return admitted;
    }
    work_.post(job_);
    return STATUS_OK;
}
auto Space::state() const noexcept -> IoState {
    sync::Lock lock{lock_};
    return state_locked();
}
auto Space::state_locked() const noexcept -> IoState {
    if (phase_ == Phase::Failed) return IO_SPACE_FAILED;
    if (phase_ == Phase::Closed) return IO_SPACE_CLOSED;
    if (closing_.load()) return IO_SPACE_CLOSING;
    return phase_ == Phase::Ready && !active_ && !preparing_ ? IO_SPACE_READY : IO_SPACE_PENDING;
}
void Space::publish_state() noexcept {
    Source::Publication publication;
    {
        sync::Lock lock{lock_};
        const u64 bits = u64{1} << state_locked();
        if (events_.snapshot().value != bits) publication = events_.prepare(bits, true);
    }
    if (publication) object::changed(job_.key());
}

auto Space::map(Op::Submission ctx, cap::Resolved<mm::Mem>& source, mm::ObjectRange range, usize iova, mm::Perms perms) noexcept -> std::expected<usize, status_t> {
    const auto view = source.view();
    const auto* authorized = std::get_if<cap::MemLimit>(&view.data);
    if (!range.valid() || !range.within(source->page_count()) || !authorized || !authorized->range.contains(range) ||
        !authorized->perms.contains(perms) || !mm::valid_perms(perms) || perms.contains(mm::Perm::Execute) ||
        !perms.contains(mm::Perm::Read) || source->kind() != mm::BackingKind::Anonymous ||
        iova % mm::page_size || iova >= arch::IoEnd || range.size() > (arch::IoEnd - (iova ? iova : arch::IoBegin)) / mm::page_size)
        return std::unexpected(STATUS_BAD_ARGS);
    IoOp* op;
    {
        sync::Lock lock{lock_};
        if (phase_ != Phase::Ready || active_ || preparing_ || closing_.load()) return std::unexpected(STATUS_BUSY);
        const auto status = accept(ctx, false);
        if (status != STATUS_OK) return std::unexpected(status);
        op = active_; preparing_ = true;
        op->edit_.emplace(IoOp::Edit{.range=range, .perms=perms,
            .install=true, .automatic=!iova});
    }
    // Only these fixed preparations need the caller's Resolved capability.
    // Everything that may yield belongs to the accepted operation.
    auto made = maps_.create(quota_, *this, iova ? iova : arch::IoBegin, range.size());
    status_t result = made ? STATUS_OK : STATUS_NO_MEMORY;
    if (made) {
        op->edit_->mapping = *made;
        (*made)->perms = perms; (*made)->offset = range.base() * mm::page_size;
        auto ref = source.reference();
        auto typed = ref ? std::move(*ref).as<mm::Mem>() : std::expected<object::ref<mm::Mem>, object::error>{std::unexpected(object::error::closed)};
        if (typed) (*made)->memory = std::move(*typed);
        if (!(*made)->memory || !source.attach((*made)->grant) || !source->attach((*made)->mem_link, perms)) result = STATUS_CLOSED;
    }
    {
        sync::Lock lock{lock_};
        if (result != STATUS_OK) {
            if (op->edit_->mapping) op->retired_.insert(*op->edit_->mapping);
            op->edit_.reset();
            op->result_ = result; op->receipt_.acknowledged = true;
        }
        preparing_ = false;
        op->state_.post();
    }
    return iova;
}

auto Space::unmap(Op::Submission ctx, usize first, usize count) noexcept -> status_t {
    if (!count || first < arch::IoBegin || first % mm::page_size || first >= arch::IoEnd || count > (arch::IoEnd - first) / mm::page_size)
        return STATUS_BAD_ARGS;
    sync::Lock lock{lock_};
    if (phase_ != Phase::Ready || active_ || preparing_ || closing_.load()) return STATUS_BUSY;
    const auto status = accept(ctx, false);
    if (status != STATUS_OK) return status;
    active_->value_ = first;
    active_->edit_.emplace(IoOp::Edit{.range={0, count}});
    active_->state_.post();
    return STATUS_OK;
}

void Space::advance_edit(IoOp& op) noexcept {
    auto& edit = *op.edit_;
    for (usize step = 0; step < 8; ++step) {
        using E = IoOp::Edit;
        std::optional<sync::Lock<sync::Spin>> guard;
        if (edit.phase != E::Pin && !(edit.phase == E::Reserve && edit.tables)) guard.emplace(lock_);
        auto fail = [&](status_t result) noexcept {
            if (!guard) guard.emplace(lock_);
            if (edit.install && edit.mapping) op.retired_.insert(*edit.mapping);
            op.result_ = result; op.receipt_.acknowledged = true; op.edit_.reset();
        };
        auto* map = edit.mapping;
        if (edit.phase < E::Apply && (closing_.load() || op.state_.canceled())) { fail(STATUS_CANCELED); return; }
        switch (edit.phase) {
        case E::Seek: {
            if (!edit.install) {
                auto* current = mappings_.find(op.value_);
                if (!current || current->count != edit.range.size()) { fail(STATUS_NOT_FOUND); return; }
                edit.mapping = current; edit.phase = E::Apply; edit.pin = current->pages;
                if (!op.state_.commit()) { fail(STATUS_CANCELED); return; }
                mappings_.erase(*current); op.retired_.insert(*current);
                if (map_epoch_ != std::numeric_limits<u64>::max()) ++map_epoch_;
                break;
            }
            auto* current = mappings_.lower_bound(map->first);
            auto* previous = current ? mappings_.previous(*current) : mappings_.maximum();
            if (previous && previous->first + previous->count * mm::page_size > map->first) current = previous;
            if (current && current->first < map->first + map->count * mm::page_size) {
                if (!edit.automatic) { fail(STATUS_BUSY); return; }
                map->first = current->first + current->count * mm::page_size;
                if (map->first >= arch::IoEnd || map->count > (arch::IoEnd - map->first) / mm::page_size) {
                    fail(STATUS_NO_MEMORY); return;
                }
            } else edit.phase = E::Pin;
            break;
        }
        case E::Pin: {
            auto page = map->memory->materialize(edit.range.base() + edit.position, nullptr, true);
            if (!page || !pmm_.is_ram(page->page()) || !page->perms().contains(edit.perms)) { fail(STATUS_BACKING_FAILED); return; }
            auto pin = pins_.create(quota_, std::move(*page), edit.position);
            if (!pin) { fail(STATUS_NO_MEMORY); return; }
            guard.emplace(lock_);
            (*pin)->next = map->pages; map->pages = *pin;
            if (++edit.position == map->count) {
                edit.position = 0; edit.plan.emplace(root_->count()); edit.phase = E::Count;
            }
            break;
        }
        case E::Count: {
            auto page = mm::VPage::from_base(mm::va_t{map->first + edit.position * mm::page_size});
            libk_assert(page && edit.plan->include(*page));
            if (++edit.position == map->count) {
                edit.tables = edit.plan->pages(); edit.plan.reset(); edit.phase = E::Reserve;
            }
            break;
        }
        case E::Reserve:
            if (edit.tables) {
                {
                    sync::Lock locked{lock_};
                    if (auto page = tables_.take()) {
                        libk_assert(op.pages_.attach(std::move(*page)));
                        op.tables_fee_.merge(fee_.split({.memory=mm::page_size})); --edit.tables; break;
                    }
                }
                auto fee = resource::acquire(quota_, {.memory=mm::page_size});
                if (!fee || !op.pages_.allocate()) { fail(STATUS_NO_MEMORY); return; }
                op.tables_fee_.merge(std::move(*fee)); --edit.tables;
            } else {
                if (!op.state_.commit()) { fail(STATUS_CANCELED); return; }
                mappings_.insert(*map);
                if (map_epoch_ != std::numeric_limits<u64>::max()) ++map_epoch_;
                op.value_ = map->first; edit.pin = map->pages; edit.phase = E::Apply;
            }
            break;
        case E::Apply: {
            if (!edit.pin) { op.edit_.reset(); return; }
            auto* pin = edit.pin;
            edit.pin = pin->next;
            auto page = mm::VPage::from_base(mm::va_t{map->first + pin->index * mm::page_size}); libk_assert(page);
            if (edit.install) {
                const auto before = op.pages_.page_count();
                const auto perms = mm::PageTable::user_perms(edit.perms); libk_assert(perms);
                libk_assert(root_->map(*page, pin->page.page(), *perms, op.pages_));
                const auto used = before - op.pages_.page_count();
                if (used) fee_.merge(op.tables_fee_.split({.memory=used * mm::page_size}));
            } else {
                auto removed = root_->unmap(*page); libk_assert(removed);
                for (auto& table : removed->tables) {
                    libk_assert(op.pages_.attach(std::move(table)));
                    op.tables_fee_.merge(fee_.split({.memory=mm::page_size}));
                }
            }
            break;
        }
        }
    }
}
auto Space::links(u64 cursor, u64 epoch, LinkRecords& out) const noexcept -> status_t {
    sync::Lock guard{lock_};
    if (map_epoch_ == std::numeric_limits<u64>::max() || (epoch && epoch != map_epoch_)) return STATUS_STALE;
    out = {}; out.version = 1; out.size = sizeof(out); out.epoch = map_epoch_;
    auto* map = mappings_.lower_bound(cursor ? cursor : arch::IoBegin);
    while (map && out.count < LINK_RECORD_PAGE) {
        out.records[out.count++] = {map->mem_link.id(), map->memory.id().value, map->grant.id().value,
            map->first, map->count * mm::page_size, map->offset,
            LINK_IO, closing_.load() ? LINK_INVALIDATING : LINK_LIVE, map->perms.raw(), 0};
        out.cursor = map->first + map->count * mm::page_size;
        map = mappings_.next(*map);
    }
    out.more = map != nullptr;
    if (!out.more) out.cursor = 0;
    return STATUS_OK;
}

void Space::close() noexcept { closing_.store(true, std::memory_order_release); work_.post(job_); }
void Space::retire(object::cleanup&& cleanup) noexcept {
    {
        sync::Lock lock{lock_};
        cleanup_ = std::move(cleanup);
    }
    close();
}
auto Space::release(Map& m) noexcept -> bool {
    // No hardware can use these pins after the acknowledged invalidation.
    (void)m.mem_link.detach();
    (void)m.grant.detach();
    mm::MemWork memory;
    object::LinkWork grant;
    { sync::Lock lock{lock_}; memory = std::move(m.mem_work); grant = std::move(m.grant_work); }
    memory.reset();
    grant.reset();
    if (m.mem_link.busy() || m.grant.busy()) return false;
    for (usize step = 0; m.pages && step < 8; ++step) {
        auto* page = m.pages; m.pages = page->next; pins_.destroy(*page);
    }
    if (m.pages) return false;
    return true;
}
void Space::progress() noexcept {
    { sync::Lock guard{lock_}; if (active_ && active_->state_.schedule()) return; }
    advance();
}
void Space::advance() noexcept {
    object::ref<> execution;
    IoOp* op{};
    bool changed;
    { sync::Lock lock{lock_}; changed = observed_epoch_ != map_epoch_; observed_epoch_ = map_epoch_; }
    if (changed) object::changed(job_.key());
    {
        sync::Lock lock{lock_};
        if (preparing_) return;
        if (phase_ == Phase::Closed) { lock.restore(); complete(); return; }
        if (failed_.load(std::memory_order_acquire)) {
            phase_ = Phase::Failed;
            lock.restore(); publish_state(); return; // Hardware ownership is quarantined.
        }
        if (!active_) {
            if (phase_ == Phase::Opening) transaction(true);
            else if (closing_.load()) {
                phase_ = Phase::Closing;
                transaction(false).retired_ = std::move(mappings_);
                if (map_epoch_ != std::numeric_limits<u64>::max()) ++map_epoch_;
            } else return;
        }
        op = active_;
        execution = operation_;
        if (op->edit_) {
            preparing_ = true;
            lock.restore();
            advance_edit(*op);
            { sync::Lock guard{lock_}; preparing_ = false; }
            op->state_.post(); return;
        }
        if (op->result_ == STATUS_OK && op->state_.query().sequence == OP_ACCEPTED && !op->state_.commit()) {
            closing_.store(true); op->enable_ = false; phase_ = Phase::Closing;
        }
        if (!op->receipt_.acknowledged) {
            const auto status = attached_ ? controller_->sync(op->receipt_, op->enable_) : STATUS_OK;
            if (status == STATUS_PENDING) { op->state_.post(); return; }
            if (status != STATUS_OK) { phase_ = Phase::Failed; lock.restore(); publish_state(); return; }
            op->receipt_.acknowledged = true;
        }
        op->state_.drain();
        preparing_ = true;
    }
    libk::scope_exit prepared{[&]() noexcept {
        { sync::Lock guard{lock_}; preparing_ = false; }
        publish_state();
    }};
    for (usize step = 0; !op->retired_.empty() && step < 8; ++step) {
        auto* map = op->retired_.minimum();
        if (!release(*map)) { work_.post(job_); return; }
        op->retired_.erase(*map);
        maps_.destroy(*map);
    }
    if (!op->retired_.empty()) { work_.post(job_); return; }
    if (!op->enable_) {
        (void)ctl_grant_.detach();
        object::LinkWork work;
        { sync::Lock lock{lock_}; work = std::move(ctl_work_); }
        work.reset();
        if (ctl_grant_.busy()) { work_.post(job_); return; }
        if (attached_) { controller_->detach(*this); attached_ = false; }
        if (root_) {
            op->pages_.append(root_->release()); root_.reset();
            op->pages_.append(std::move(tables_));
            op->tables_fee_.merge(std::move(fee_));
        }
    }
    if (op->enable_) {
        sync::Lock locked{lock_};
        tables_.append(std::move(op->pages_)); fee_.merge(std::move(op->tables_fee_));
    } else if (!maps_.drain(8) || !pins_.drain(8)) { post(); return; }
    for (usize i = 0; i < 8 && op->pages_.page_count(); ++i) {
        auto page = op->pages_.take(); libk_assert(page); page->reset();
    }
    if (op->pages_.page_count()) { post(); return; }
    op->pages_.reset(); op->tables_fee_.reset();
    if (!op->state_.detach()) return;
    const auto value = op->value_;
    const bool internal = !execution, closing = !op->enable_;
    const auto result = op->result_;
    if (internal) op->state_.finish(result, value);
    {
        sync::Lock lock{lock_};
        active_ = nullptr;
        execution = std::move(operation_);
        if (internal) control_.reset();
        phase_ = closing ? Phase::Closed : Phase::Ready;
        preparing_ = false;
    }
    (void)prepared.release();
    if (!internal) op->state_.finish(result, value);
    publish_state();
    if (closing) complete();
    else if (closing_.load()) work_.post(job_);
}

void Space::complete() noexcept {
    bool retiring;
    { sync::Lock lock{lock_}; retiring = bool(cleanup_); }
    if (retiring) {
        events_.close();
        if (!events_.drained()) return;
        work_.close(job_);
    }
    object::cleanup cleanup;
    object::ref<> self;
    { sync::Lock lock{lock_}; cleanup = std::move(cleanup_); self = std::move(self_); }
    if (cleanup) cleanup.complete();
}
} // namespace io
