#include <cpu.hpp>
#include <utility>
#include "dma.hpp"
#include <libk/pci.hpp>

PciDma::PciDma(u16 requester, u16 did, const FwPci& pci, Iommu& iommu, const time::Clock& clock, irq::Line line) noexcept
    : requester_(requester), did_(did), pci_(pci), iommu_(iommu), clock_(clock), line_(line) {}
auto PciDma::take_fault() noexcept -> std::optional<io::Fault> { return iommu_.take_fault(*this); }

auto PciDma::reserve() noexcept -> bool {
    libk_assert((phase_ == Phase::Reserved || phase_ == Phase::Closed) && !root_);
    const pci::Cfg<arch::io_fence> cfg{mm::DirectBegin + pci_.cfg.pa + (usize{requester_} << 12)};
    if (cfg.read<u32>(0) != 0x1042'1af4 || (cfg.read<u8>(14) & 0x7f) != 0) return false;
    const auto bars = cfg.bars();
    if (!bars) return false;
    std::array<io::Reg, IO_REG_COUNT> regs{};
    for (usize i = 0; i < bars->size(); ++i) {
        const auto& bar = (*bars)[i];
        if (!bar.size) continue;
        // A full mapped page must stay inside the board-authorized window.
        const auto bytes = std::max<u64>(bar.size, mm::page_size);
        if (bytes > pci_.window.size || bar.pa % bytes || bar.pa < pci_.window.pa ||
            bar.pa > pci_.window.pa + pci_.window.size - bytes) return false;
        regs[i] = {bar.pa, bar.size, mm::Perms::of(mm::Perm::Read, mm::Perm::Write)};
    }
    regs[IO_PCI_CFG] = {cfg.base - mm::DirectBegin, mm::page_size, mm::Perms::of(mm::Perm::Read)};
    if ((cfg.read<u16>(6) & 0x10) == 0)
        return false;
    u8 capability = cfg.read<u8>(0x34);
    usize status{};
    for (usize count = 0; capability != 0 && count < 48; ++count) {
        if (capability < 0x40 || (capability & 3) != 0)
            return false;
        const usize cap = capability;
        const u8 id = cfg.read<u8>(cap);
        if (id == 9 && cfg.read<u8>(cap + 3) == 1) {
            if (capability > 0xf0 || cfg.read<u8>(cap + 2) < 16 || status != 0)
                return false;
            const u8 bar = cfg.read<u8>(cap + 4);
            const u32 offset = cfg.read<u32>(cap + 8);
            const u32 length = cfg.read<u32>(cap + 12);
            if (bar >= IO_PCI_CFG || length < 21
                || offset > regs[bar].size
                || length > regs[bar].size - offset)
                return false;
            status = mm::DirectBegin + regs[bar].pa + offset + 20;
        }
        capability = cfg.read<u8>(cap + 1);
    }
    if (capability != 0 || status == 0)
        return false;
    status_ = status;
    regs_ = regs;
    phase_ = Phase::Reserved;
    cfg.write<u16>(4, 2 | (1 << 10));
    return true;
}

auto PciDma::state() const noexcept -> State {
    switch (phase_) {
    case Phase::Reserved: return State::Reserved;
    case Phase::Opening: return State::Opening;
    case Phase::Active: return State::Active;
    case Phase::Closed: return State::Closed;
    case Phase::Failed: return State::Failed;
    default: return State::Closing;
    }
}

auto PciDma::deadline(u64 nanoseconds) noexcept -> bool {
    const auto duration = clock_.duration_from_nanoseconds(nanoseconds);
    const auto end = duration ? clock_.now().checked_add(*duration)
                              : std::nullopt;
    if (!end) {
        fail();
        return false;
    }
    deadline_ = *end;
    return true;
}

void PciDma::open(mm::PageTable&& root) noexcept {
    libk_assert(phase_ == Phase::Reserved);
    libk_assert(root.kind() == mm::PageTable::Kind::Io);
    root_.emplace(std::move(root));
    phase_ = Phase::Opening;
    if (!deadline(1'000'000'000)) return;

}

void PciDma::reset_device() noexcept {
    // BAR retirement precedes this call. A device-memory read drains prior CPU
    // posted writes while decode is still enabled; virtio reset drains the selected
    // QEMU virtio backend. IOFENCE alone cannot prove backend quiescence.
    const pci::Cfg<arch::io_fence> cfg{mm::DirectBegin + pci_.cfg.pa + (usize{requester_} << 12)};
    cfg.write<u16>(4, (cfg.read<u16>(4) & ~u16{4}) | u16{1 << 10});
    const pci::Cfg<arch::io_fence> status{status_};
    (void)status.read<u8>(0);
    status.write<u8>(0, 0);
    phase_ = Phase::Resetting;
    static_cast<void>(deadline(1'000'000'000));
}

void PciDma::close() noexcept {
    switch (phase_) {
    case Phase::Reserved: phase_ = Phase::Closed; break;
    case Phase::Opening: {
        phase_ = Phase::ClosingOpening;
        bool submitted;
        { sync::Lock guard{iommu_.lock_}; submitted = iommu_.inflight_ == this; }
        // No published context to await; closing must still progress during overflow.
        if (!submitted) reset_device();
        break;
    }
    case Phase::Active: reset_device(); break;
    default: break;
    }
}

auto PciDma::poll() noexcept -> State {
    switch (phase_.load()) {
    case Phase::Opening:
    case Phase::ClosingOpening:
    case Phase::Invalidating: {
        const auto completion = iommu_.advance(*this,
            phase_ == Phase::Invalidating ? std::nullopt : std::optional{root_->page()});
        if (completion == Iommu::Step::Failed) fail();
        else if (completion == Iommu::Step::Pending) {
            if (clock_.now() >= deadline_) fail();
        } else if (phase_ == Phase::Opening) {
            pci::Cfg<arch::io_fence>{mm::DirectBegin + pci_.cfg.pa + (usize{requester_} << 12)}
                .write<u16>(4, 6);
            phase_ = Phase::Active;
        } else if (phase_ == Phase::ClosingOpening) reset_device();
        else phase_ = Phase::Draining;
        break;
    }
    case Phase::Resetting:
        if (pci::Cfg<arch::io_fence>{status_}.read<u8>(0) != 0) {
            if (clock_.now() >= deadline_) fail();
        } else {
            phase_ = Phase::Invalidating;
            static_cast<void>(deadline(1'000'000'000));
        }
        break;
    case Phase::Draining: {
        const auto result = iommu_.clear_faults(*this);
        if (result == Iommu::Step::Failed) fail();
        else if (result == Iommu::Step::Complete) {
            root_.reset();
        } else if (clock_.now() >= deadline_) fail();
        break;
    }
    default: break;
    }
    return state();
}

static constexpr usize Ddtp = 0x10;
static constexpr usize Fctl = 0x8;
static constexpr usize Cqb = 0x18;
static constexpr usize Cqh = 0x20;
static constexpr usize Cqt = 0x24;
static constexpr usize Fqb = 0x28;
static constexpr usize Fqh = 0x30;
static constexpr usize Fqt = 0x34;
static constexpr usize Cqcsr = 0x48;
static constexpr usize Fqcsr = 0x4c;
static constexpr usize Ipsr = 0x54;
static constexpr usize Icvec = 0x2f8;
static constexpr u32 FaultInterrupt = 1 << 1;
static constexpr u32 FaultQueueErrors = (1 << 8) | (1 << 9);
static constexpr u32 On = 1 << 16;
static constexpr u32 Busy = 1 << 17;
static constexpr u32 CommandErrors = (1 << 8) | (1 << 9) | (1 << 10);

template<class T>
auto Iommu::read(usize offset) const noexcept -> T {
    arch::io_fence();
    const T value = *reinterpret_cast<volatile const T*>(base_ + offset);
    arch::io_fence();
    return value;
}
template<class T>
void Iommu::write(usize offset, T value) const noexcept {
    arch::io_fence();
    *reinterpret_cast<volatile T*>(base_ + offset) = value;
    arch::io_fence();
}

Iommu::~Iommu() noexcept {
    // Controller shutdown is a machine-lifetime operation. Returning these
    // frames while hardware can still access them is never a recovery path.
    libk_assert(state_ == State::Idle);
}

auto Iommu::start(mm::Pmm& pmm) noexcept
    -> std::expected<void, Error> {
    if (state_ != State::Idle) return std::unexpected(Error::Busy);
    const u64 capabilities = read<u64>(0);
    if (capabilities == 0 || capabilities == ~u64{0})
        return std::unexpected(Error::Absent);
    // Version 1.x, coherent little-endian Sv39 and extended device contexts.
    // MSI translation itself remains disabled in each context.
    if ((capabilities & 0xf0) != 0x10
        || (capabilities & (u64{1} << 9)) == 0
        || (capabilities & (u64{1} << 22)) == 0
        || ((capabilities >> 28) & 3) == 0
        || ((capabilities >> 28) & 3) == 3
        || (read<u32>(Fctl) & 5) != 0)
        return std::unexpected(Error::Unsupported);
    storage_ = pmm.group();
    {
        auto pending = storage_.owner().group();
        mm::Page* pages[] = {&directory_, &commands_, &faults_,
            &contexts_[0], &contexts_[1], &contexts_[2], &contexts_[3]};
        for (auto* slot : pages) {
            auto allocated = pending.allocate();
            if (!allocated) return std::unexpected(Error::NoMemory);
            *slot = allocated.value();
            if (slot->raw() >= (u64{1} << 44))
                return std::unexpected(Error::Unsupported);
            auto* bytes = pending.bytes(*slot);
            for (usize index = 0; index < mm::page_size; ++index)
                bytes[index] = byte{};
        }
        storage_.append(std::move(pending));
    }
    auto* directory = reinterpret_cast<u64*>(storage_.bytes(directory_));
    for (usize i = 0; i < ContextPages; ++i)
        directory[i] = (contexts_[i].raw() << 10) | 1;
    // Do not release storage after this point, including failed initialization.
    state_ = State::Disabling;
    write<u64>(Ddtp, 0);
    write<u32>(Cqcsr, 0);
    write<u32>(Fqcsr, 0);
    return {};
}

auto Iommu::failed() noexcept -> Step {
    state_ = State::Failed;
    return Step::Failed;
}

auto Iommu::initialize() noexcept -> Step {
    switch (state_) {
    case State::Idle:
    case State::Failed:
        return Step::Failed;
    case State::Disabling:
        if ((read<u64>(Ddtp) & 16) != 0
            || (read<u32>(Cqcsr) & (On | Busy)) != 0
            || (read<u32>(Fqcsr) & (On | Busy)) != 0)
            return Step::Pending;
        if ((read<u64>(Ddtp) & 15) != 0) return failed();
        // Select wired signaling while Off; queue/vector wiring is owned by this board backend.
        write<u32>(Fctl, 2);
        if ((read<u32>(Fctl) & 7) != 2) return failed();
        write<u64>(Icvec, read<u64>(Icvec) & ~u64{0xf0}); // fault queue uses firmware vector zero
        if (((read<u64>(Icvec) >> 4) & 15) != 0) return failed();
        write<u64>(Cqb, (commands_.raw() << 10) | 7); // 256 commands
        write<u64>(Fqb, (faults_.raw() << 10) | 6); // 128 fault records
        write<u32>(Cqt, 0);
        write<u32>(Fqh, 0);
        write<u32>(Cqcsr, 1);
        write<u32>(Fqcsr, 1);
        state_ = State::Queues;
        return Step::Pending;
    case State::Queues: {
        const u32 commands = read<u32>(Cqcsr);
        const u32 faults = read<u32>(Fqcsr);
        if ((commands & CommandErrors) != 0 || (faults & (1 << 8)) != 0)
            return failed();
        if ((commands & Busy) != 0 || (faults & Busy) != 0)
            return Step::Pending;
        if ((commands & On) == 0 || (faults & On) == 0) return failed();
        if (read<u64>(Cqb) != ((commands_.raw() << 10) | 7)
            || read<u64>(Fqb) != ((faults_.raw() << 10) | 6)) return failed();
        write<u64>(Ddtp, (directory_.raw() << 10) | 3);
        state_ = State::Directory;
        return Step::Pending;
    }
    case State::Directory: {
        const u64 directory = read<u64>(Ddtp);
        if ((directory & 16) != 0) return Step::Pending;
        if (directory != ((directory_.raw() << 10) | 3)) return failed();
        write<u32>(Ipsr, FaultInterrupt);
        write<u32>(Fqcsr, 3); // FQEN | FIE
        state_ = State::Ready;
        return Step::Complete;
    }
    case State::Ready:
        return Step::Complete;
    }
    __builtin_unreachable();
}

void Iommu::add(PciDma& d) noexcept {
    d.next = devices();
    devices_.store(&d, std::memory_order_release);
}

auto Iommu::advance(PciDma& d, std::optional<mm::Page> root) noexcept -> Step {
    sync::Lock guard{lock_};
    if (state_ != State::Ready) return Step::Failed;
    if ((read<u32>(Cqcsr) & CommandErrors) != 0) return failed();
    if (inflight_) {
        if (read<u32>(Cqh) != tail_) return Step::Pending;
        // The final command is IOFENCE.C: CQH advancement completes the batch.
        if (inflight_ == &d) { inflight_ = nullptr; return Step::Complete; }
        if (inflight_->phase_.load() != PciDma::Phase::Failed) return Step::Pending;
        // A timed-out owner retains its pages in quarantine. Its completed
        // command no longer needs to block unrelated device sessions.
        inflight_ = nullptr;
    }
    if (root && fault_mode_ != FaultMode::On) return Step::Pending;
    const u16 requester = d.device_id();
    if (requester >= RootBusRequesters || (root && root->raw() >= (u64{1} << 44)))
        return Step::Failed;
    if (read<u32>(Cqh) != tail_) return failed();
    auto* context = reinterpret_cast<u64*>(storage_.bytes(contexts_[requester >> 6]))
        + (requester & 63) * 8;
    __atomic_store_n(&context[0], u64{0}, __ATOMIC_RELEASE);
    context[1] = 0; // no second-stage translation
    // A PSCID identifies one first-stage address space. Distinct live DIDs
    // may map the same IOVA differently, so sharing PSCID would alias IOATC
    // entries even when their device-directory contexts are distinct.
    context[2] = root ? u64{requester} << 12 : 0;
    context[3] = root ? (u64{8} << 60) | root->raw() : 0;
    if (root) __atomic_store_n(&context[0], u64{1}, __ATOMIC_RELEASE);

    auto* commands = reinterpret_cast<u64*>(storage_.bytes(commands_));
    const u64 batch[] = {
        u64{3} | (u64{1} << 33) | (u64{requester} << 40),
        u64{1}, // global VMA invalidation also covers this DID's old generation
        u64{2} | (u64{1} << 12) | (u64{1} << 13), // IOFENCE.C PR/PW
    };
    for (const auto command : batch) {
        commands[tail_ * 2] = command;
        commands[tail_ * 2 + 1] = 0;
        tail_ = (tail_ + 1) & 255;
    }
    inflight_ = &d;
    write<u32>(Cqt, tail_);
    return Step::Pending;
}

auto Iommu::drain_faults() noexcept -> Step {
    if (state_ != State::Ready) return Step::Failed;
    const u32 tail = read<u32>(Fqt);
    const u32 status = read<u32>(Fqcsr);
    if (tail >= 128 || (status & (1 << 8)) != 0) {
        return failed();
    }
    if ((status & (1 << 9)) != 0) {
        begin_overflow();
        bool any{};
        for (auto* node = devices(); node; node = node->next) any |= node->recovering_;
        if (!any) return failed();
    }
    while (fault_head_ != tail) {
        const auto* record = reinterpret_cast<volatile const u64*>(
            storage_.bytes(faults_)) + fault_head_ * 4;
        const u64 header = record[0];
        const io::Fault fault{
            .cause = static_cast<u16>(header & 0xfff),
            .requester = static_cast<u32>(header >> 40), .address = record[2]};
        for (auto* node = devices(); node; node = node->next)
            if (node->device_id() == fault.requester && !node->fault_) {
                node->fault_ = fault; node->fault_->requester = node->requester(); break;
            }
        fault_head_ = (fault_head_ + 1) & 127;
    }
    write<u32>(Fqh, fault_head_);
    return Step::Complete;
}

void Iommu::begin_overflow() noexcept {
    if (fault_mode_ != FaultMode::On) return;
    fault_mode_ = FaultMode::Draining;
    // A dropped record has no trustworthy DID. Stop every context that could
    // have generated it and suppress the level IRQ until all have fenced.
    // Request FQEN with FIE masked and no FQOF clear. Some emulators clear
    // FQOF on this write anyway; FaultMode remains latched until every
    // affected context has completed its invalidation fence.
    write<u32>(Fqcsr, 1);
    for (auto* node = devices(); node; node = node->next) {
        auto& d = *node;
        const auto phase = d.phase_.load();
        d.recovering_ = phase != PciDma::Phase::Reserved && phase != PciDma::Phase::Closed;
    }
}

auto Iommu::interrupt() noexcept -> bool {
    bool healthy;
    {
        sync::Lock guard{lock_};
        // Clear before draining so a later record can raise a new edge.
        write<u32>(Ipsr, FaultInterrupt);
        healthy = drain_faults() == Step::Complete;
        if (fault_mode_ != FaultMode::On) write<u32>(Ipsr, FaultInterrupt);
    }
    for (auto* node = devices(); node; node = node->next) {
        auto& d = *node;
        bool signal;
        { sync::Lock guard{lock_}; signal = !healthy || fault_mode_ != FaultMode::On || d.fault_.has_value(); }
        // No controller lock while entering the capability/session owner.
        if (signal) d.signal_fault();
    }
    return healthy;
}

auto Iommu::take_fault(PciDma& d) noexcept -> std::optional<io::Fault> {
    sync::Lock guard{lock_};
    if (drain_faults() != Step::Complete) return std::nullopt;
    return std::exchange(d.fault_, std::nullopt);
}

auto Iommu::clear_faults(PciDma& d) noexcept -> Step {
    sync::Lock guard{lock_};
    // The caller already awaited this DID's invalidation fence. Another DID
    // may now own the command queue; it does not gate this mailbox.
    if (state_ != State::Ready) return Step::Failed;
    if (drain_faults() != Step::Complete)
        return Step::Failed;
    const u32 status = read<u32>(Fqcsr);
    if ((status & (1 << 8)) != 0 || (status & On) == 0)
        return failed();
    d.fault_.reset();
    if (d.recovering_) {
        bool last = true;
        for (auto* node = devices(); node; node = node->next) if (node != &d && node->recovering_) { last = false; break; }
        if (last) {
            if ((status & Busy) != 0) return Step::Pending;
            if (fault_mode_ != FaultMode::Rearming) {
                write<u32>(Fqcsr, 3 | (1 << 9)); // clear FQOF, restore FIE
                fault_mode_ = FaultMode::Rearming;
                return Step::Pending;
            }
            const u32 resumed = read<u32>(Fqcsr);
            if ((resumed & Busy) != 0) return Step::Pending;
            if ((resumed & (FaultQueueErrors | On | 3)) != (On | 3))
                return failed();
            write<u32>(Ipsr, FaultInterrupt);
            fault_mode_ = FaultMode::On;
        }
        d.recovering_ = false;
    }
    d.phase_ = PciDma::Phase::Closed;
    return Step::Complete;
}
