#include <arch/iommu.hpp>
#include <arch/pci.hpp>

#include <core/debug.hpp>
#include <libk/limits.hpp>
#include <mm/virtual_layout.hpp>

namespace arch {
namespace {
constexpr usize Base = kernel::mm::layout::DirectMapBegin + virt_iommu_base;
constexpr usize Ddtp = 0x10;
constexpr usize Cqb = 0x18;
constexpr usize Cqh = 0x20;
constexpr usize Cqt = 0x24;
constexpr usize Fqb = 0x28;
constexpr usize Fqh = 0x30;
constexpr usize Fqt = 0x34;
constexpr usize Cqcsr = 0x48;
constexpr usize Fqcsr = 0x4c;
constexpr u32 On = 1 << 16;
constexpr u32 Busy = 1 << 17;
constexpr u32 CommandErrors = (1 << 8) | (1 << 9) | (1 << 10);

template<typename T>
auto read(usize offset) noexcept -> T {
    asm volatile("fence iorw, iorw" ::: "memory");
    const T value = *reinterpret_cast<volatile const T*>(Base + offset);
    asm volatile("fence iorw, iorw" ::: "memory");
    return value;
}
template<typename T>
void write(usize offset, T value) noexcept {
    asm volatile("fence iorw, iorw" ::: "memory");
    *reinterpret_cast<volatile T*>(Base + offset) = value;
    asm volatile("fence iorw, iorw" ::: "memory");
}
auto ppn(kernel::mm::Page page) noexcept -> u64 {
    return page.frame().raw();
}
} // namespace

Iommu::~Iommu() noexcept {
    // Controller shutdown is a machine-lifetime operation. Returning these
    // frames while hardware can still access them is never a recovery path.
    KASSERT(state_ == State::Idle);
}

auto Iommu::start(kernel::mm::Pmm& pmm, u16 requester) noexcept
    -> libk::Expected<void, IommuError> {
    if (state_ != State::Idle) return libk::unexpected(IommuError::Busy);
    const u64 capabilities = read<u64>(0);
    if (capabilities == 0 || capabilities == ~u64{0})
        return libk::unexpected(IommuError::Absent);
    // Version 1.x, coherent little-endian Sv39 and extended device contexts.
    // MSI translation itself remains disabled in each context.
    if ((capabilities & 0xf0) != 0x10
        || (capabilities & (u64{1} << 9)) == 0
        || (capabilities & (u64{1} << 22)) == 0
        || (read<u32>(8) & 5) != 0)
        return libk::unexpected(IommuError::Unsupported);
    if (requester >= (1 << 15))
        return libk::unexpected(IommuError::Unsupported);
    storage_ = pmm.make_page_group();
    {
        auto extension = storage_.extend();
        kernel::mm::Page* pages[] = {&directory_, &contexts_, &commands_, &faults_};
        for (auto* slot : pages) {
            auto allocated = extension.allocate_page();
            if (!allocated) return libk::unexpected(IommuError::InsufficientMemory);
            *slot = allocated.value();
            if (ppn(*slot) >= (u64{1} << 44))
                return libk::unexpected(IommuError::Unsupported);
            auto* bytes = extension.bytes(*slot);
            for (usize index = 0; index < kernel::mm::page_size; ++index)
                bytes[index] = byte{};
        }
        extension.commit();
    }
    requester_ = requester;
    auto* directory = reinterpret_cast<u64*>(storage_.bytes(directory_));
    directory[requester >> 6] = (ppn(contexts_) << 10) | 1;
    context_ = reinterpret_cast<u64*>(storage_.bytes(contexts_))
        + (requester & 63) * 8;
    // Do not release storage after this point, including failed initialization.
    state_ = State::Disabling;
    write<u64>(Ddtp, 0);
    write<u32>(Cqcsr, 0);
    write<u32>(Fqcsr, 0);
    return libk::expected();
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
        state_ = State::Ready;
        return IoStatus::Complete;
    }
    case State::Ready:
        return IoStatus::Complete;
    }
    __builtin_unreachable();
}

auto Iommu::replace(libk::optional<kernel::mm::Page> root) noexcept
    -> libk::Expected<u64, IommuError> {
    if (state_ != State::Ready) return libk::unexpected(IommuError::Failed);
    if (issued_ != completed_) return libk::unexpected(IommuError::Busy);
    if (issued_ == libk::numeric_limits<u64>::max()
        || (read<u32>(Cqcsr) & CommandErrors) != 0
        || read<u32>(Cqh) != tail_) {
        static_cast<void>(failed());
        return libk::unexpected(IommuError::Failed);
    }
    if (root && ppn(*root) >= (u64{1} << 44))
        return libk::unexpected(IommuError::Unsupported);
    __atomic_store_n(&context_[0], u64{0}, __ATOMIC_RELEASE);
    context_[1] = 0; // no second-stage translation
    context_[2] = 0; // PSCID zero; single isolated function
    context_[3] = root ? (u64{8} << 60) | ppn(*root) : 0;
    if (root) __atomic_store_n(&context_[0], u64{1}, __ATOMIC_RELEASE);

    auto* commands = reinterpret_cast<u64*>(storage_.bytes(commands_));
    const u64 batch[] = {
        u64{3} | (u64{1} << 33) | (u64{requester_} << 40),
        u64{1}, // all first-stage translation caches, including PSCID zero
        u64{2} | (u64{1} << 12) | (u64{1} << 13), // IOFENCE.C PR/PW
    };
    for (const auto command : batch) {
        commands[tail_ * 2] = command;
        commands[tail_ * 2 + 1] = 0;
        tail_ = (tail_ + 1) & 255;
    }
    ++issued_;
    write<u32>(Cqt, tail_);
    return libk::expected(issued_);
}

auto Iommu::poll(u64 ticket) noexcept -> IoStatus {
    if (state_ != State::Ready || ticket == 0 || ticket != issued_)
        return IoStatus::Failed;
    if ((read<u32>(Cqcsr) & CommandErrors) != 0) return failed();
    // CQH alone doesn't complete ordinary commands. This batch ends with a
    // fence, whose CQH advancement is the architectural completion boundary.
    if (read<u32>(Cqh) != tail_) return IoStatus::Pending;
    completed_ = ticket;
    return IoStatus::Complete;
}

auto Iommu::take_fault() noexcept -> libk::optional<IoFault> {
    if (state_ != State::Ready) return libk::nullopt;
    const u32 tail = read<u32>(Fqt);
    if (tail >= 128 || (read<u32>(Fqcsr) & (1 << 8)) != 0) {
        static_cast<void>(failed());
        return libk::nullopt;
    }
    if (tail == fault_head_) return libk::nullopt;
    const auto* record = reinterpret_cast<volatile const u64*>(
        storage_.bytes(faults_)) + fault_head_ * 4;
    const u64 header = record[0];
    const IoFault fault{
        .cause = static_cast<u16>(header & 0xfff),
        .requester = static_cast<u32>(header >> 40), .address = record[2]};
    fault_head_ = (fault_head_ + 1) & 127;
    write<u32>(Fqh, fault_head_);
    return libk::optional<IoFault>{fault};
}

auto Iommu::clear_faults() noexcept -> IoStatus {
    if (state_ != State::Ready || issued_ != completed_) return IoStatus::Failed;
    const u32 status = read<u32>(Fqcsr);
    const u32 tail = read<u32>(Fqt);
    if ((status & (1 << 8)) != 0 || (status & On) == 0 || tail >= 128)
        return failed();
    fault_head_ = tail;
    write<u32>(Fqh, tail);
    // FQOF is W1C. A full old-generation queue must not suppress subsequent
    // fault reporting after its consumer position has been reset.
    write<u32>(Fqcsr, (status & 3) | (1 << 9));
    return IoStatus::Complete;
}

} // namespace arch
