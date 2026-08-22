#include <operation/page_fault.hpp>

#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <mm/memory_object.hpp>
#include <mm/vspace.hpp>

namespace kernel::operation {

/*luna change: publish page outcomes through atomic lifecycle phases, reason: a Pager callback may race Wait admission without a second continuation state*/
PageFault::PageFault() noexcept
    : completion_(Completion::bind_resume<
          PageFault,
          &PageFault::complete,
          &PageFault::read,
          &PageFault::release,
          &PageFault::cancel,
          &PageFault::resume>(*this)) {
    completion_.set_policy(diag::concurrency::OperationPolicy{
        .kind = diag::concurrency::WaitKind::Pager,
        .expectation = diag::concurrency::Expectation::ExternalUnbounded,
        .driver = diag::concurrency::NodeRef::external(
            reinterpret_cast<u64>(this), 1),
    });
}

PageFault::~PageFault() noexcept {
    KASSERT(!completion_.attached());
    KASSERT(!relation_.attached());
    KASSERT(memory_.load<libk::MemoryOrder::Acquire>() == nullptr);
    KASSERT(!demand_);
    KASSERT(phase_.load<libk::MemoryOrder::Acquire>() == Phase::Idle
        || phase_.load<libk::MemoryOrder::Acquire>() == Phase::Terminal
        || phase_.load<libk::MemoryOrder::Acquire>() == Phase::Canceled);
}

auto PageFault::active() const noexcept -> bool {
    return phase_.load<libk::MemoryOrder::Acquire>() != Phase::Idle;
}

auto PageFault::terminal() const noexcept -> bool {
    return phase_.load<libk::MemoryOrder::Acquire>() == Phase::Terminal;
}

auto PageFault::kind() const noexcept -> mm::FaultKind {
    return static_cast<mm::FaultKind>(kind_.load<libk::MemoryOrder::Acquire>());
}

auto PageFault::complete() const noexcept -> bool {
    return phase_.load<libk::MemoryOrder::Acquire>() == Phase::Ready;
}

auto PageFault::read() noexcept -> Result {
    const auto kind = this->kind();
    /*luna change: expose terminal memory pressure failures as no-memory,
      reason: completion reads must preserve ResourceExhausted/OOM classes*/
    return Result{
        .status = kind == mm::FaultKind::Ready
                || kind == mm::FaultKind::Materialized
            ? MYOS_STATUS_OK
            : kind == mm::FaultKind::OutOfMemory
                || kind == mm::FaultKind::ResourceExhausted
                ? MYOS_STATUS_NO_MEMORY
            : kind == mm::FaultKind::BackingFailed
                ? MYOS_STATUS_PEER_FAULT
                : MYOS_STATUS_CANCELED,
        .value = 0,
    };
}

void PageFault::drop_pin() noexcept {
    mm::MemoryObject* const memory = memory_.exchange<
        libk::MemoryOrder::AcqRel>(nullptr);
    if (memory != nullptr) {
        memory->release_fault();
    }
}

void PageFault::release() noexcept {
    drop_pin();
    const Phase phase = phase_.load<libk::MemoryOrder::Acquire>();
    /*luna change: refund retained demand at terminal completion, reason:
      pressure retry keeps it only across a Rearm handoff*/
    switch (phase) {
    case Phase::Terminal:
        demand_.reset();
        return;
    case Phase::Ready:
    case Phase::Canceled:
        demand_.reset();
        phase_.store<libk::MemoryOrder::Release>(Phase::Idle);
        return;
    case Phase::Idle:
    case Phase::Attaching:
    case Phase::Pending:
        KASSERT(false);
        return;
    }
}

void PageFault::publish(
    void* owner,
    mm::PageWaitResult result) noexcept {
    auto& fault = *static_cast<PageFault*>(owner);
    const mm::FaultKind previous = fault.kind();
    const mm::FaultKind kind = result == mm::PageWaitResult::OutOfMemory
        ? mm::FaultKind::OutOfMemory
        : result == mm::PageWaitResult::Ready
            && previous == mm::FaultKind::Pressure
            ? mm::FaultKind::Pressure
            : result == mm::PageWaitResult::Ready
                ? mm::FaultKind::Ready
                : mm::FaultKind::BackingFailed;
    fault.kind_.store<libk::MemoryOrder::Release>(static_cast<u8>(kind));
    Phase expected = fault.phase_.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if (expected != Phase::Attaching && expected != Phase::Pending) {
            KASSERT(expected == Phase::Ready);
            return;
        }
        if (fault.phase_.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(expected, Phase::Ready)) {
            break;
        }
    }
    if (fault.completion_.attached()) {
        fault.completion_.signal();
    }
}

/*luna change: remove the duplicate PageFault error classifier, reason: VSpace owns the shared fault boundary mapping*/

auto PageFault::admit() noexcept -> mm::FaultKind {
    KASSERT(vspace_ != nullptr && cpus_ != nullptr);
    KASSERT(!relation_.attached());
    phase_.store<libk::MemoryOrder::Release>(Phase::Attaching);
    kind_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(mm::FaultKind::Pending));
    const auto result = vspace_->fault(
        mm::VmContext{.cpus = cpus_, .local = local_},
        address_,
        access_,
        &relation_,
        this,
        &PageFault::publish,
        &demand_);
    if (!result) {
        kind_.store<libk::MemoryOrder::Release>(
            static_cast<u8>(mm::fault_kind(result.error())));
        demand_.reset();
        phase_.store<libk::MemoryOrder::Release>(Phase::Terminal);
        return kind();
    }
    const mm::FaultKind next = result.value().kind;
    if (next == mm::FaultKind::Pending || next == mm::FaultKind::Pressure) {
        if (result.value().memory == nullptr) {
            kind_.store<libk::MemoryOrder::Release>(
                static_cast<u8>(mm::FaultKind::BackingFailed));
            demand_.reset();
            phase_.store<libk::MemoryOrder::Release>(Phase::Terminal);
            return kind();
        }
        if (next == mm::FaultKind::Pressure) {
            /*luna change: mark Pressure only while the attaching kind is
              still canonical, reason: an early callback's terminal kind must
              never be overwritten after MemoryObject relation admission*/
            u8 expected = static_cast<u8>(mm::FaultKind::Pending);
            static_cast<void>(kind_.compare_exchange_strong<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(
                expected,
                static_cast<u8>(mm::FaultKind::Pressure)));
        }
        /*luna change: consume the backing-owned relation handoff, reason:
          MemoryObject attached PageReclaimer before returning Pressure*/
        /*luna change: accept the pin handoff even after an early callback, reason: Attaching may already have published Ready before VSpace returns Pending*/
        memory_.store<libk::MemoryOrder::Release>(result.value().memory);
        generation_.store<libk::MemoryOrder::Release>(relation_.generation);
        Phase expected = Phase::Attaching;
        if (!phase_.compare_exchange_strong<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(expected, Phase::Pending)) {
            KASSERT(expected == Phase::Ready);
        }
        return next;
    }
    kind_.store<libk::MemoryOrder::Release>(static_cast<u8>(next));
    demand_.reset();
    phase_.store<libk::MemoryOrder::Release>(Phase::Terminal);
    return next;
}

auto PageFault::start(
    mm::VSpace& vspace,
    CpuRegistry& cpus,
    CpuId local,
    mm::VirtAddr address,
    mm::Access access) noexcept -> mm::FaultKind {
    const Phase phase = phase_.load<libk::MemoryOrder::Acquire>();
    KASSERT(phase == Phase::Idle || phase == Phase::Terminal
        || phase == Phase::Canceled);
    KASSERT(!completion_.attached() && !relation_.attached());
    drop_pin();
    vspace_ = &vspace;
    cpus_ = &cpus;
    local_ = local;
    address_ = address;
    access_ = access;
    generation_.store<libk::MemoryOrder::Relaxed>(0);
    demand_.reset();
    phase_.store<libk::MemoryOrder::Release>(Phase::Idle);
    return admit();
}

auto PageFault::cancel() noexcept -> bool {
    if (phase_.load<libk::MemoryOrder::Acquire>() != Phase::Pending) {
        return false;
    }
    mm::MemoryObject* const memory = memory_.load<
        libk::MemoryOrder::Acquire>();
    const u64 generation = generation_.load<libk::MemoryOrder::Acquire>();
    if (memory == nullptr || !relation_.attached()
        || relation_.generation != generation) {
        return false;
    }
    const bool canceled = kind() == mm::FaultKind::Pressure
        ? memory->release_pressure(relation_, generation)
        : memory->cancel_fault(relation_, generation);
    if (!canceled) {
        return false;
    }
    if (kind() == mm::FaultKind::Pressure) {
        memory->release_fault();
    }
    memory_.store<libk::MemoryOrder::Release>(nullptr);
    demand_.reset();
    kind_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(mm::FaultKind::BackingFailed));
    phase_.store<libk::MemoryOrder::Release>(Phase::Canceled);
    return true;
}

void PageFault::reset() noexcept {
    KASSERT(!completion_.attached() && !relation_.attached());
    KASSERT(memory_.load<libk::MemoryOrder::Acquire>() == nullptr);
    KASSERT(!demand_);
    phase_.store<libk::MemoryOrder::Release>(Phase::Idle);
    vspace_ = nullptr;
    cpus_ = nullptr;
    local_ = {};
    address_ = {};
    access_ = mm::Access::Read;
    generation_.store<libk::MemoryOrder::Relaxed>(0);
    kind_.store<libk::MemoryOrder::Relaxed>(
        static_cast<u8>(mm::FaultKind::NoMapping));
}

auto PageFault::resume(arch::TrapContext& trap) noexcept
    -> Completion::ResumeResult {
    static_cast<void>(trap);
    KASSERT(phase_.load<libk::MemoryOrder::Acquire>() == Phase::Ready);
    drop_pin();
    if (kind() == mm::FaultKind::OutOfMemory
        || kind() == mm::FaultKind::ResourceExhausted
        || kind() == mm::FaultKind::BackingFailed) {
        demand_.reset();
        phase_.store<libk::MemoryOrder::Release>(Phase::Terminal);
        return Completion::ResumeResult::Done;
    }
    phase_.store<libk::MemoryOrder::Release>(Phase::Idle);
    const mm::FaultKind next = admit();
    const Completion::ResumeResult result =
        next == mm::FaultKind::Pending || next == mm::FaultKind::Pressure
        ? Completion::ResumeResult::Rearm
        : Completion::ResumeResult::Done;
    return result;
}

} // namespace kernel::operation
