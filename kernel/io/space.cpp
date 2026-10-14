#include <io/space.hpp>
#include <cpu/cpu.hpp>
#include <ipc/notification.hpp>
#include <memory>
#include <algorithm>

namespace io {
static constexpr usize Ddtp = 0x10, Cqb = 0x18, Cqh = 0x20, Cqt = 0x24;
static constexpr usize Fqb = 0x28, Fqh = 0x30, Fqt = 0x34, Cqcsr = 0x48, Fqcsr = 0x4c, Ipsr = 0x54;
static constexpr u32 On = 1 << 16, Busy = 1 << 17, CqErrors = 7 << 8, FqErrors = 3 << 8;

template<class T> auto Iommu::read(usize offset) const noexcept -> T {
    arch::io_fence();
    const auto value = *reinterpret_cast<volatile const T*>(mapping_.address() + offset);
    arch::io_fence();
    return value;
}
template<class T> void Iommu::write(usize offset, T value) noexcept {
    arch::io_fence();
    *reinterpret_cast<volatile T*>(mapping_.address() + offset) = value;
    arch::io_fence();
}
Iommu::Iommu(mm::Pmm& pmm, mm::KSpace& vm, Cpus& cpus, WorkQueue& work, const time::Clock& clock,
         object::ref<> payer, mm::Phys::Lease&& claim, object::ref<irq::Ctl> irq_owner,
         irq::Line line, IommuConfig config) noexcept
    : vm_(vm), cpus_(cpus), work_(work), clock_(clock), payer_(std::move(payer)),
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
    auto fee = resource::acquire(payer_, {.memory = mm::page_size});
    if (!fee) return std::unexpected(STATUS_NO_MEMORY);
    auto page = storage_.allocate();
    if (!page) return std::unexpected(STATUS_NO_MEMORY);
    std::fill_n(storage_.bytes(*page), mm::page_size, byte{});
    fee_.merge(std::move(*fee));
    return *page;
}
auto Iommu::context(u32 did, bool create) noexcept -> u64* {
    // Extended contexts: 6 DID bits per leaf, 9 per directory level.
    const unsigned levels = config_.device_bits <= 6 ? 0 : config_.device_bits <= 15 ? 1 : 2;
    auto page = directory_;
    for (unsigned level = levels; level; --level) {
        auto* entry = reinterpret_cast<u64*>(storage_.bytes(page)) + ((did >> (6 + 9 * (level - 1))) & 511);
        auto value = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
        if (!value) {
            if (!create) return nullptr;
            auto child = this->page();
            if (!child) return nullptr;
            value = (child->raw() << 10) | 1;
            __atomic_store_n(entry, value, __ATOMIC_RELEASE);
        }
        page = mm::Page{value >> 10};
    }
    return reinterpret_cast<u64*>(storage_.bytes(page)) + (did & 63) * 8;
}
auto Iommu::open() noexcept -> status_t {
    const auto range = mm::Pages::from_aligned_bytes(mm::pa_t{config_.address}, 4096);
    if (!range || !claim_.covers(*range)) return STATUS_BAD_ARGS;
    if (!vm_.map(mapping_, *range)) return STATUS_NO_MEMORY;
    mapped_ = true;
    const auto caps = read<u64>(0);
    // Coherent little-endian Sv39, extended contexts, wired interrupts.
    if ((caps & 0xf0) != 0x10 || !(caps & (u64{1} << 9)) || !(caps & (u64{1} << 22)) ||
        ((caps >> 28) & 3) == 0 || ((caps >> 28) & 3) == 3 || (read<u32>(8) & 5))
        return STATUS_INVALID_OP;
    mm::Page* slots[]{&directory_, &commands_, &faults_};
    for (auto* slot : slots) {
        auto allocated = page();
        if (!allocated) return allocated.error();
        *slot = *allocated;
    }
    if (!irq_.connect()) return STATUS_BUSY;
    work_.open(job_, Work::Fn::bind<&Iommu::progress>(*this));
    phase_ = Phase::Disabling;
    deadline_ = clock_.rd_ticks() + clock_.rd_freq();
    write<u64>(Ddtp, 0);
    write<u32>(Cqcsr, 0);
    write<u32>(Fqcsr, 0);
    work_.post(job_);
    return STATUS_OK;
}
auto Iommu::attach(Space& domain) noexcept -> status_t {
    sync::Lock lock{lock_};
    if (phase_ != Phase::Ready || closing_) return STATUS_BUSY;
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
    libk_assert(inflight_ != &domain);
    auto** link = &domains_;
    while (*link != &domain) link = &(*link)->next_;
    *link = domain.next_;
    domain.next_ = nullptr;
    if (closing_) work_.post(job_);
}
auto Iommu::sync(Space& domain, bool enable) noexcept -> status_t {
    sync::Lock lock{lock_};
    if (phase_ != Phase::Ready) return STATUS_BACKING_FAILED;
    if (read<u32>(Cqcsr) & CqErrors) { fail(); return STATUS_BACKING_FAILED; }
    if (inflight_) {
        if (read<u32>(Cqh) != tail_) {
            if (clock_.rd_ticks() >= deadline_) { fail(); return STATUS_BACKING_FAILED; }
            return STATUS_PENDING;
        }
        if (inflight_ != &domain) return STATUS_PENDING;
        inflight_ = nullptr;
        return STATUS_OK;
    }
    auto* dc = context(domain.requester_, false);
    libk_assert(dc);
    __atomic_store_n(&dc[0], u64{0}, __ATOMIC_RELEASE);
    dc[1] = 0;
    dc[2] = u64{domain.pscid_} << 12; // unique translation tag per live domain
    dc[3] = enable ? (u64{8} << 60) | domain.root_.raw() : 0;
    if (enable) __atomic_store_n(&dc[0], u64{1}, __ATOMIC_RELEASE);
    auto* commands = reinterpret_cast<u64*>(storage_.bytes(commands_));
    const u64 batch[]{u64{3} | (u64{1} << 33) | (u64{domain.requester_} << 40),
                      u64{1}, u64{2} | (u64{1} << 12) | (u64{1} << 13)};
    for (const auto command : batch) {
        commands[tail_ * 2] = command;
        commands[tail_ * 2 + 1] = 0;
        tail_ = (tail_ + 1) & 255;
    }
    inflight_ = &domain;
    deadline_ = clock_.rd_ticks() + clock_.rd_freq();
    write<u32>(Cqt, tail_);
    return STATUS_PENDING;
}
void Iommu::fail() noexcept {
    phase_ = Phase::Failed;
    // Keep all potentially reachable storage and Claims. Failure is not drain.
    for (auto* d = domains_; d; d = d->next_) {
        d->failed_.store(true, std::memory_order_release);
        work_.post(d->job_);
    }
}
auto Iommu::interrupt() noexcept -> bool {
    irq_pending_.store(true, std::memory_order_release);
    work_.post(job_);
    return false;
}
void Iommu::disconnected() noexcept {
    disconnected_.store(true, std::memory_order_release);
    work_.post(job_);
}
void Iommu::retire(object::cleanup&& cleanup) noexcept {
    {
        sync::Lock lock{lock_};
        closing_ = true;
        cleanup_ = std::move(cleanup);
        for (auto* d = domains_; d; d = d->next_) {
            d->closing_.store(true, std::memory_order_release);
            work_.post(d->job_);
        }
        // Construction can fail before the protocol worker was opened.
        if (phase_ == Phase::Empty) work_.open(job_, Work::Fn::bind<&Iommu::progress>(*this));
    }
    work_.post(job_);
}
void Iommu::progress() noexcept {
    bool rearm{};
    {
        sync::Lock lock{lock_};
        if (disconnected_.load(std::memory_order_acquire)) fail();
        if (phase_ == Phase::Failed) return;
        if (phase_ == Phase::Ready && irq_pending_.exchange(false, std::memory_order_acq_rel)) {
            write<u32>(Ipsr, 3);
            const auto tail = read<u32>(Fqt), status = read<u32>(Fqcsr);
            if (tail >= 128 || (status & FqErrors)) { fail(); return; }
            while (fault_head_ != tail) {
                const auto* record = reinterpret_cast<volatile const u64*>(storage_.bytes(faults_)) + fault_head_ * 4;
                const auto did = static_cast<u32>(record[0] >> 40);
                for (auto* d = domains_; d; d = d->next_) if (d->requester_ == did) {
                    d->closing_.store(true, std::memory_order_release);
                    work_.post(d->job_);
                }
                fault_head_ = (fault_head_ + 1) & 127;
            }
            write<u32>(Fqh, fault_head_);
            rearm = true;
        }
        if (closing_ && domains_) return;
        if (closing_ && phase_ == Phase::Ready) {
            write<u64>(Ddtp, 0);
            write<u32>(Cqcsr, 0);
            write<u32>(Fqcsr, 0);
            deadline_ = clock_.rd_ticks() + clock_.rd_freq();
            phase_ = Phase::Stopping;
        }
        switch (phase_) {
        case Phase::Empty: phase_ = Phase::Unmapping; break;
        case Phase::Disabling:
            if ((read<u64>(Ddtp) & 16) || (read<u32>(Cqcsr) & (On | Busy)) || (read<u32>(Fqcsr) & (On | Busy))) break;
            if (read<u64>(Ddtp) & 15) { fail(); return; }
            write<u32>(8, 2);
            if ((read<u32>(8) & 7) != 2) { fail(); return; }
            write<u64>(0x2f8, read<u64>(0x2f8) & ~u64{0xff});
            write<u64>(Cqb, (commands_.raw() << 10) | 7);
            write<u64>(Fqb, (faults_.raw() << 10) | 6);
            write<u32>(Cqt, 0);
            write<u32>(Fqh, 0);
            write<u32>(Cqcsr, 1);
            write<u32>(Fqcsr, 1);
            phase_ = Phase::Queues;
            break;
        case Phase::Queues: {
            const auto cq = read<u32>(Cqcsr), fq = read<u32>(Fqcsr);
            if ((cq & CqErrors) || (fq & FqErrors)) { fail(); return; }
            if ((cq | fq) & Busy) break;
            if (!(cq & On) || !(fq & On)) { fail(); return; }
            const u64 mode = config_.device_bits <= 6 ? 2 : config_.device_bits <= 15 ? 3 : 4;
            write<u64>(Ddtp, (directory_.raw() << 10) | mode);
            phase_ = Phase::Directory;
            break;
        }
        case Phase::Directory:
            if (read<u64>(Ddtp) & 16) break;
            if (read<u64>(Ddtp) != ((directory_.raw() << 10) | (config_.device_bits <= 6 ? 2 : config_.device_bits <= 15 ? 3 : 4))) {
                fail(); return;
            }
            write<u32>(Ipsr, 3);
            write<u32>(Fqcsr, 3);
            phase_ = Phase::Ready;
            break;
        case Phase::Stopping:
            if ((read<u64>(Ddtp) & 16) || (read<u32>(Cqcsr) & (On | Busy)) || (read<u32>(Fqcsr) & (On | Busy))) break;
            if (read<u64>(Ddtp) & 15) { fail(); return; }
            phase_ = Phase::Unmapping;
            break;
        default: break;
        }
        if (phase_ != Phase::Ready && phase_ != Phase::Unmapping && clock_.rd_ticks() >= deadline_) { fail(); return; }
        if (phase_ != Phase::Ready && phase_ != Phase::Unmapping) { work_.post(job_); return; }
        if (phase_ == Phase::Ready) { if (closing_) work_.post(job_); }
    }
    // Route lock never nests inside the controller lock: IRQ dispatch posts work.
    if (rearm) { irq_.reset(); if (!irq_.connect()) disconnected(); }
    if (phase_ != Phase::Unmapping) return;
    irq_.reset();
    if (mapped_) {
        if (!flush_) { flush_.emplace(); vm_.unmap(mapping_, *flush_, cpus_, current_cpu().id); }
        if (!flush_->complete()) { (void)flush_->kick(cpus_); work_.post(job_); return; }
        vm_.release(mapping_, *flush_);
        flush_.reset();
        mapped_ = false;
    }
    storage_.reset();
    fee_.reset();
    { sync::Lock lock{lock_}; phase_ = Phase::Closed; }
    work_.close(job_);
    auto cleanup = std::move(cleanup_);
    cleanup.complete();
}

Space::Map::Map(mm::Pmm& pmm, Space& domain, usize iova, usize n) noexcept
    : owner(domain), first(iova), count(n), pins(pmm) {}
Space::Map::~Map() noexcept {
    libk_assert(!mem_link.busy() && !grant.busy());
    while (pages) { auto* p = pages; pages = p->next; pins.destroy(*p); }
}
void Space::Map::invalidate_mem(void* arg, mm::MemWork&& work) noexcept {
    auto& m = *static_cast<Map*>(arg);
    sync::Lock lock{m.owner.lock_};
    m.mem_work = std::move(work);
    m.owner.close();
}
void Space::Map::invalidate_grant(void* arg, cap::GrantWork&& work) noexcept {
    auto& m = *static_cast<Map*>(arg);
    sync::Lock lock{m.owner.lock_};
    m.grant_work = std::move(work);
    m.owner.close();
}
void Space::invalidated(void* arg, cap::GrantWork&& work) noexcept {
    auto& d = *static_cast<Space*>(arg);
    sync::Lock lock{d.lock_};
    d.ctl_work_ = std::move(work);
    d.close();
}
Space::Space(mm::Pmm& pmm, WorkQueue& work, object::ref<> payer, object::ref<Iommu> ctl, u32 requester) noexcept
    : pmm_(pmm), work_(work), payer_(std::move(payer)), controller_(std::move(ctl)),
      requester_(requester), maps_(pmm), tables_(pmm.group()) {
    work_.open(job_, Work::Fn::bind<&Space::progress>(*this));
}
Space::~Space() noexcept { libk_assert(!self_ && !mappings_ && !attached_ && !cleanup_); }
auto Space::open(object::ref<> self, cap::Resolved<Iommu>& controller) noexcept -> status_t {
    preparing_ = true;
    libk::scope_exit prepared{[&]() noexcept {
        { sync::Lock lock{lock_}; preparing_ = false; }
        work_.post(job_);
    }};
    self_ = std::move(self);
    auto fee = resource::acquire(payer_, {.memory = mm::page_size});
    if (!fee) return STATUS_NO_MEMORY;
    auto root = tables_.allocate();
    if (!root) return STATUS_NO_MEMORY;
    root_ = *root;
    fee_ = std::move(*fee);
    std::fill_n(tables_.bytes(root_), mm::page_size, byte{});
    if (!controller.attach(ctl_grant_)) return STATUS_CLOSED;
    const auto result = controller_->attach(*this);
    if (result != STATUS_OK) return result;
    attached_ = true;
    work_.post(job_);
    return STATUS_OK;
}
auto Space::state() const noexcept -> IoState {
    sync::Lock lock{lock_};
    switch (phase_) {
    case Phase::Ready: return closing_.load() ? IO_SPACE_CLOSING : preparing_ ? IO_SPACE_PENDING : IO_SPACE_READY;
    case Phase::Denying: case Phase::Retiring: return IO_SPACE_CLOSING;
    case Phase::Closed: return IO_SPACE_CLOSED;
    case Phase::Failed: return IO_SPACE_FAILED;
    default: return closing_.load() ? IO_SPACE_CLOSING : IO_SPACE_PENDING;
    }
}
auto Space::watch(ipc::Notification& notice, u64 badge) noexcept -> status_t {
    sync::Lock lock{lock_};
    if (!badge || notice_.attached() || !notice.bind(notice_, badge)) return STATUS_BAD_ARGS;
    return STATUS_OK;
}
auto Space::leaf(usize iova, bool create) noexcept -> u64* {
    auto page = root_;
    for (int level = 2; level; --level) {
        auto* entry = reinterpret_cast<u64*>(tables_.bytes(page)) + ((iova >> (12 + 9 * level)) & 511);
        auto value = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
        if (!value) {
            if (!create) return nullptr;
            auto fee = resource::acquire(payer_, {.memory = mm::page_size});
            if (!fee) return nullptr;
            auto child = tables_.allocate();
            if (!child) return nullptr;
            fee_.merge(std::move(*fee));
            std::fill_n(tables_.bytes(*child), mm::page_size, byte{});
            value = (child->raw() << 10) | 1;
            __atomic_store_n(entry, value, __ATOMIC_RELEASE);
        }
        page = mm::Page{value >> 10};
    }
    return reinterpret_cast<u64*>(tables_.bytes(page)) + ((iova >> 12) & 511);
}
auto Space::map(cap::Resolved<mm::Mem>& source, mm::ObjectRange range, usize iova, mm::Perms perms) noexcept -> std::expected<usize, status_t> {
    constexpr usize limit = usize{1} << 38;
    const auto view = source.view();
    const auto* authorized = std::get_if<cap::MemLimit>(&view.data);
    if (!range.valid() || !range.within(source->page_count()) || !authorized || !authorized->range.contains(range) ||
        !authorized->perms.contains(perms) || !mm::valid_perms(perms) || perms.contains(mm::Perm::Execute) ||
        !perms.contains(mm::Perm::Read) || source->kind() != mm::BackingKind::Anonymous ||
        iova % 4096 || iova >= limit || range.size() > (limit - iova) / 4096) return std::unexpected(STATUS_BAD_ARGS);
    {
        sync::Lock lock{lock_};
        if (phase_ != Phase::Ready || preparing_ || closing_.load()) return std::unexpected(STATUS_BUSY);
        if (!iova) {
            iova = 4096;
            for (auto* m = mappings_; m; m = m->next) {
                if (iova <= m->first && range.size() * 4096 <= m->first - iova) break;
                iova = std::max(iova, m->first + m->count * 4096);
            }
            if (range.size() > (limit - iova) / 4096) return std::unexpected(STATUS_NO_MEMORY);
        }
        for (auto* m = mappings_; m; m = m->next)
            if (iova < m->first + m->count * 4096 && m->first < iova + range.size() * 4096) return std::unexpected(STATUS_BUSY);
        preparing_ = true;
    }
    auto made = maps_.create(payer_, pmm_, *this, iova, range.size());
    auto* m = made ? *made : nullptr;
    status_t result = m ? STATUS_OK : STATUS_NO_MEMORY;
    if (m) {
        auto ref = source.reference();
        auto typed = ref ? std::move(*ref).as<mm::Mem>() : std::expected<object::ref<mm::Mem>, object::error>{std::unexpected(object::error::closed)};
        if (typed) m->memory = std::move(*typed);
        if (!m->memory || !source.attach(m->grant) || !source->attach(m->mem_link, perms)) result = STATUS_CLOSED;
        else {
            auto** tail = &m->pages;
            for (usize n = 0; n < range.size() && result == STATUS_OK; ++n) {
                auto page = source->materialize(range.base() + n);
                if (!page || !pmm_.is_ram(page->page()) || !page->perms().contains(perms)) result = STATUS_BACKING_FAILED;
                else {
                    auto pin = m->pins.create(payer_, std::move(*page));
                    if (!pin) result = STATUS_NO_MEMORY;
                    else { *tail = *pin; tail = &(*pin)->next; }
                }
            }
            for (usize n = 0; n < range.size() && result == STATUS_OK; ++n)
                if (!leaf(iova + n * 4096, true)) result = STATUS_NO_MEMORY;
        }
    }
    {
        sync::Lock lock{lock_};
        // Even failed preparation can have attached invalidation tokens.
        if (m) {
            auto** link = &mappings_;
            while (*link && (*link)->first < iova) link = &(*link)->next;
            m->next = *link;
            *link = m;
        }
        preparing_ = false;
        if (closing_.load()) result = STATUS_CLOSED;
        if (result != STATUS_OK) {
            if (m) m->removing = true;
        } else {
            auto* pin = m->pages;
            const u64 flags = 1 | 2 | 16 | 64 | (perms.contains(mm::Perm::Write) ? 4 | 128 : 0);
            for (usize n = 0; n < m->count; ++n, pin = pin->next)
                __atomic_store_n(leaf(iova + n * 4096, false), (pin->page.page().raw() << 10) | flags, __ATOMIC_RELEASE);
            phase_ = Phase::Editing;
        }
    }
    work_.post(job_);
    return result == STATUS_OK ? std::expected<usize, status_t>{iova} : std::unexpected(result);
}
auto Space::unmap(usize first, usize count) noexcept -> status_t {
    sync::Lock lock{lock_};
    if (phase_ != Phase::Ready || preparing_ || closing_.load()) return STATUS_BUSY;
    for (auto* m = mappings_; m; m = m->next) if (m->first == first && m->count == count) {
        m->removing = true;
        phase_ = Phase::Editing;
        for (usize n = 0; n < count; ++n) __atomic_store_n(leaf(first + n * 4096, false), u64{0}, __ATOMIC_RELEASE);
        work_.post(job_);
        return STATUS_PENDING;
    }
    return STATUS_NOT_FOUND;
}
void Space::close() noexcept { closing_.store(true, std::memory_order_release); work_.post(job_); }
void Space::retire(object::cleanup&& cleanup) noexcept {
    {
        sync::Lock lock{lock_};
        if (phase_ == Phase::Closed) { cleanup.complete(); return; }
        cleanup_ = std::move(cleanup);
    }
    close();
}
auto Space::release(Map& m) noexcept -> bool {
    // No hardware can use these pins after the acknowledged invalidation.
    (void)m.mem_link.detach();
    (void)m.grant.detach();
    mm::MemWork memory;
    cap::GrantWork grant;
    { sync::Lock lock{lock_}; memory = std::move(m.mem_work); grant = std::move(m.grant_work); }
    memory.reset();
    grant.reset();
    if (m.mem_link.busy() || m.grant.busy()) return false;
    maps_.destroy(m);
    return true;
}
void Space::progress() noexcept {
    Map* removed{};
    bool finish{};
    {
        sync::Lock lock{lock_};
        if (preparing_) return; // The admitted syscall posts after preparation.
        if (failed_.load(std::memory_order_acquire)) {
            phase_ = Phase::Failed;
            (void)notice_.signal();
            return; // quarantine, including the object cleanup reference
        }
        if (!attached_) phase_ = Phase::Retiring;
        if (phase_ == Phase::Opening || phase_ == Phase::Editing || phase_ == Phase::Denying) {
            const bool enable = phase_ != Phase::Denying;
            const auto result = controller_->sync(*this, enable);
            if (result == STATUS_BACKING_FAILED) { phase_ = Phase::Failed; (void)notice_.signal(); return; }
            if (result == STATUS_PENDING) { work_.post(job_); return; }
            phase_ = enable ? Phase::Ready : Phase::Retiring;
        }
        if (phase_ == Phase::Ready && closing_.load(std::memory_order_acquire)) {
            phase_ = Phase::Denying;
            work_.post(job_);
            return;
        }
        if (phase_ == Phase::Retiring) {
            for (auto* m = mappings_; m; m = m->next) m->removing = true;
            (void)ctl_grant_.detach();
        }
        auto** link = &mappings_;
        while (*link) {
            auto* m = *link;
            if (!m->removing) { link = &m->next; continue; }
            *link = m->next;
            m->next = removed;
            removed = m;
        }
        finish = phase_ == Phase::Retiring && !mappings_;
        preparing_ = true; // serialize slab destruction with syscall preparation
    }
    // Callback tokens cannot be released under the relation's owner lock.
    while (removed) {
        auto* m = removed;
        removed = m->next;
        if (!release(*m)) {
            sync::Lock lock{lock_};
            auto** link = &mappings_;
            while (*link && (*link)->first < m->first) link = &(*link)->next;
            m->next = *link;
            *link = m;
            finish = false;
            work_.post(job_);
        }
    }
    {
        sync::Lock lock{lock_};
        preparing_ = false;
        if (phase_ == Phase::Ready) (void)notice_.signal();
    }
    if (!finish) return;
    ctl_work_.reset();
    if (ctl_grant_.busy()) { work_.post(job_); return; }
    if (attached_) { controller_->detach(*this); attached_ = false; }
    tables_.reset();
    fee_.reset();
    { sync::Lock lock{lock_}; phase_ = Phase::Closed; (void)notice_.signal(); }
    notice_.reset();
    work_.close(job_);
    auto cleanup = std::move(cleanup_);
    auto self = std::move(self_);
    if (cleanup) cleanup.complete();
}
} // namespace io
