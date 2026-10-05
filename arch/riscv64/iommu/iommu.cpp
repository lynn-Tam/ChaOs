#include <expected>
#include <optional>
#include <utility>
#include <arch/iommu.hpp>

#include <libk/assert.hpp>
#include <base/types.hpp>
#include <limits>
#include <mm/table.hpp>
#include <sync.hpp>

namespace arch {
namespace {
constexpr usize Ddtp = 0x10;
constexpr usize Fctl = 0x8;
constexpr usize Cqb = 0x18;
constexpr usize Cqh = 0x20;
constexpr usize Cqt = 0x24;
constexpr usize Fqb = 0x28;
constexpr usize Fqh = 0x30;
constexpr usize Fqt = 0x34;
constexpr usize Cqcsr = 0x48;
constexpr usize Fqcsr = 0x4c;
constexpr usize Ipsr = 0x54;
constexpr usize Icvec = 0x2f8;
constexpr u32 FaultInterrupt = 1 << 1;
constexpr u32 FaultQueueErrors = (1 << 8) | (1 << 9);
constexpr u32 On = 1 << 16;
constexpr u32 Busy = 1 << 17;
constexpr u32 CommandErrors = (1 << 8) | (1 << 9) | (1 << 10);

auto ppn(mm::Page page) noexcept -> u64 {
    return page.raw();
}
} // namespace

template<class T>
auto Iommu::read(usize offset) const noexcept -> T {
    asm volatile("fence iorw, iorw" ::: "memory");
    const T value = *reinterpret_cast<volatile const T*>(base_ + offset);
    asm volatile("fence iorw, iorw" ::: "memory");
    return value;
}
template<class T>
void Iommu::write(usize offset, T value) const noexcept {
    asm volatile("fence iorw, iorw" ::: "memory");
    *reinterpret_cast<volatile T*>(base_ + offset) = value;
    asm volatile("fence iorw, iorw" ::: "memory");
}

Iommu::~Iommu() noexcept {
    // Controller shutdown is a machine-lifetime operation. Returning these
    // frames while hardware can still access them is never a recovery path.
    libk_assert(state_ == State::Idle);
}

auto Iommu::start(mm::Pmm& pmm) noexcept
    -> std::expected<void, IommuError> {
    if (state_ != State::Idle) return std::unexpected(IommuError::Busy);
    const u64 capabilities = read<u64>(0);
    if (capabilities == 0 || capabilities == ~u64{0})
        return std::unexpected(IommuError::Absent);
    // Version 1.x, coherent little-endian Sv39 and extended device contexts.
    // MSI translation itself remains disabled in each context.
    if ((capabilities & 0xf0) != 0x10
        || (capabilities & (u64{1} << 9)) == 0
        || (capabilities & (u64{1} << 22)) == 0
        || ((capabilities >> 28) & 3) == 0
        || ((capabilities >> 28) & 3) == 3
        || (read<u32>(Fctl) & 5) != 0)
        return std::unexpected(IommuError::Unsupported);
    storage_ = pmm.group();
    {
        auto pending = storage_.owner().group();
        mm::Page* pages[] = {&directory_, &commands_, &faults_,
            &contexts_[0], &contexts_[1], &contexts_[2], &contexts_[3]};
        for (auto* slot : pages) {
            auto allocated = pending.allocate();
            if (!allocated) return std::unexpected(IommuError::InsufficientMemory);
            *slot = allocated.value();
            if (ppn(*slot) >= (u64{1} << 44))
                return std::unexpected(IommuError::Unsupported);
            auto* bytes = pending.bytes(*slot);
            for (usize index = 0; index < mm::page_size; ++index)
                bytes[index] = byte{};
        }
        storage_.append(std::move(pending));
    }
    auto* directory = reinterpret_cast<u64*>(storage_.bytes(directory_));
    for (usize i = 0; i < ContextPages; ++i)
        directory[i] = (ppn(contexts_[i]) << 10) | 1;
    // Do not release storage after this point, including failed initialization.
    state_ = State::Disabling;
    write<u64>(Ddtp, 0);
    write<u32>(Cqcsr, 0);
    write<u32>(Fqcsr, 0);
    return {};
}

auto Iommu::failed() noexcept -> IoStatus {
    state_ = State::Failed;
    return IoStatus::Failed;
}

auto Iommu::initialize() noexcept -> IoStatus {
    switch (state_) {
    case State::Idle:
    case State::Failed:
        return IoStatus::Failed;
    case State::Disabling:
        if ((read<u64>(Ddtp) & 16) != 0
            || (read<u32>(Cqcsr) & (On | Busy)) != 0
            || (read<u32>(Fqcsr) & (On | Busy)) != 0)
            return IoStatus::Pending;
        if ((read<u64>(Ddtp) & 15) != 0) return failed();
        // QEMU virt wires FQ to PLIC source 37. Select WSI while the IOMMU
        // is Off and its queues are disabled, as required by fctl.
        write<u32>(Fctl, 2);
        if ((read<u32>(Fctl) & 7) != 2) return failed();
        write<u64>(Icvec, (read<u64>(Icvec) & ~u64{0xf0}) | (u64{1} << 4));
        if (((read<u64>(Icvec) >> 4) & 15) != 1) return failed();
        write<u64>(Cqb, (ppn(commands_) << 10) | 7); // 256 commands
        write<u64>(Fqb, (ppn(faults_) << 10) | 6); // 128 fault records
        write<u32>(Cqt, 0);
        write<u32>(Fqh, 0);
        write<u32>(Cqcsr, 1);
        write<u32>(Fqcsr, 1);
        state_ = State::Queues;
        return IoStatus::Pending;
    case State::Queues: {
        const u32 commands = read<u32>(Cqcsr);
        const u32 faults = read<u32>(Fqcsr);
        if ((commands & CommandErrors) != 0 || (faults & (1 << 8)) != 0)
            return failed();
        if ((commands & Busy) != 0 || (faults & Busy) != 0)
            return IoStatus::Pending;
        if ((commands & On) == 0 || (faults & On) == 0) return failed();
        if (read<u64>(Cqb) != ((ppn(commands_) << 10) | 7)
            || read<u64>(Fqb) != ((ppn(faults_) << 10) | 6)) return failed();
        write<u64>(Ddtp, (ppn(directory_) << 10) | 3);
        state_ = State::Directory;
        return IoStatus::Pending;
    }
    case State::Directory: {
        const u64 directory = read<u64>(Ddtp);
        if ((directory & 16) != 0) return IoStatus::Pending;
        if (directory != ((ppn(directory_) << 10) | 3)) return failed();
        write<u32>(Ipsr, FaultInterrupt);
        write<u32>(Fqcsr, 3); // FQEN | FIE
        state_ = State::Ready;
        return IoStatus::Complete;
    }
    case State::Ready:
        return IoStatus::Complete;
    }
    __builtin_unreachable();
}

auto Iommu::replace(u16 requester, std::optional<mm::Page> root) noexcept
    -> std::expected<u64, IommuError> {
    sync::Lock guard{lock_};
    if (state_ != State::Ready) return std::unexpected(IommuError::Failed);
    if (requester >= RootBusRequesters)
        return std::unexpected(IommuError::Unsupported);
    if (root && overflow_) return std::unexpected(IommuError::Busy);
    if (issued_ != completed_) return std::unexpected(IommuError::Busy);
    if (issued_ == std::numeric_limits<u64>::max()
        || (read<u32>(Cqcsr) & CommandErrors) != 0
        || read<u32>(Cqh) != tail_) {
        static_cast<void>(failed());
        return std::unexpected(IommuError::Failed);
    }
    if (root && ppn(*root) >= (u64{1} << 44))
        return std::unexpected(IommuError::Unsupported);
    auto* context = reinterpret_cast<u64*>(storage_.bytes(contexts_[requester >> 6]))
        + (requester & 63) * 8;
    __atomic_store_n(&context[0], u64{0}, __ATOMIC_RELEASE);
    context[1] = 0; // no second-stage translation
    // A PSCID identifies one first-stage address space. Distinct live DIDs
    // may map the same IOVA differently, so sharing PSCID would alias IOATC
    // entries even when their device-directory contexts are distinct.
    context[2] = root ? u64{requester} << 12 : 0;
    context[3] = root ? (u64{8} << 60) | ppn(*root) : 0;
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
    ++issued_;
    // Ownership lasts through the null-context fence and fault-mailbox
    // drain. An overflow observed there must still account for this DID.
    if (root) active_[requester] = true;
    write<u32>(Cqt, tail_);
    return (issued_);
}

auto Iommu::poll(u64 ticket) noexcept -> IoStatus {
    sync::Lock guard{lock_};
    if (state_ != State::Ready || ticket == 0 || ticket > issued_)
        return IoStatus::Failed;
    if (ticket <= completed_) return IoStatus::Complete;
    if (ticket != issued_) return IoStatus::Failed;
    if ((read<u32>(Cqcsr) & CommandErrors) != 0) return failed();
    // CQH alone doesn't complete ordinary commands. This batch ends with a
    // fence, whose CQH advancement is the architectural completion boundary.
    if (read<u32>(Cqh) != tail_) return IoStatus::Pending;
    completed_ = ticket;
    return IoStatus::Complete;
}

auto Iommu::drain_faults() noexcept -> IoStatus {
    if (state_ != State::Ready) return IoStatus::Failed;
    const u32 tail = read<u32>(Fqt);
    const u32 status = read<u32>(Fqcsr);
    if (tail >= 128 || (status & (1 << 8)) != 0) {
        return failed();
    }
    if ((status & (1 << 9)) != 0) {
        begin_overflow();
        if (recovery_count_ == 0) return failed();
    }
    while (fault_head_ != tail) {
        const auto* record = reinterpret_cast<volatile const u64*>(
            storage_.bytes(faults_)) + fault_head_ * 4;
        const u64 header = record[0];
        const io::Fault fault{
            .cause = static_cast<u16>(header & 0xfff),
            .requester = static_cast<u32>(header >> 40), .address = record[2]};
        if (fault.requester < RootBusRequesters
            && !pending_faults_[fault.requester])
            pending_faults_[fault.requester] = fault;
        fault_head_ = (fault_head_ + 1) & 127;
    }
    write<u32>(Fqh, fault_head_);
    return IoStatus::Complete;
}

void Iommu::begin_overflow() noexcept {
    if (overflow_) return;
    overflow_ = true;
    // A dropped record has no trustworthy DID. Stop every context that could
    // have generated it and suppress the level IRQ until all have fenced.
    // Request FQEN with FIE masked and no FQOF clear. Some emulators clear
    // FQOF on this write anyway; overflow_ remains latched until every
    // affected context has completed its invalidation fence.
    write<u32>(Fqcsr, 1);
    for (u16 requester = 0; requester < RootBusRequesters; ++requester) {
        recovering_[requester] = active_[requester];
        if (active_[requester]) ++recovery_count_;
    }
}

auto Iommu::handle_fault_irq() noexcept -> IoStatus {
    sync::Lock guard{lock_};
    if (state_ != State::Ready) return IoStatus::Failed;
    // Clear the interrupt before reading the queue. A newly appended record
    // after this edge raises FIP again; records already present are drained.
    write<u32>(Ipsr, FaultInterrupt);
    const auto status = drain_faults();
    if (overflow_) write<u32>(Ipsr, FaultInterrupt);
    return status;
}

auto Iommu::fault_pending(u16 requester) noexcept -> bool {
    sync::Lock guard{lock_};
    return requester < RootBusRequesters && pending_faults_[requester].has_value();
}

auto Iommu::fault_overflow() noexcept -> bool {
    sync::Lock guard{lock_};
    return overflow_;
}

auto Iommu::take_fault(u16 requester) noexcept -> std::optional<io::Fault> {
    sync::Lock guard{lock_};
    if (requester >= RootBusRequesters || drain_faults() != IoStatus::Complete)
        return std::nullopt;
    auto fault = std::move(pending_faults_[requester]);
    pending_faults_[requester].reset();
    return fault;
}

auto Iommu::clear_faults(u16 requester) noexcept -> IoStatus {
    sync::Lock guard{lock_};
    // The caller already awaited this DID's invalidation fence. Another DID
    // may now have a command in flight; its ticket does not gate this mailbox.
    if (state_ != State::Ready) return IoStatus::Failed;
    if (requester >= RootBusRequesters || drain_faults() != IoStatus::Complete)
        return IoStatus::Failed;
    const u32 status = read<u32>(Fqcsr);
    if ((status & (1 << 8)) != 0 || (status & On) == 0)
        return failed();
    pending_faults_[requester].reset();
    if (recovering_[requester]) {
        if (recovery_count_ == 1) {
            if ((status & Busy) != 0) return IoStatus::Pending;
            if (!rearming_) {
                write<u32>(Fqcsr, 3 | (1 << 9)); // clear FQOF, restore FIE
                rearming_ = true;
                return IoStatus::Pending;
            }
            const u32 resumed = read<u32>(Fqcsr);
            if ((resumed & Busy) != 0) return IoStatus::Pending;
            if ((resumed & (FaultQueueErrors | On | 3)) != (On | 3))
                return failed();
            write<u32>(Ipsr, FaultInterrupt);
            overflow_ = false;
            rearming_ = false;
        }
        recovering_[requester] = false;
        --recovery_count_;
    }
    active_[requester] = false;
    return IoStatus::Complete;
}

} // namespace arch
