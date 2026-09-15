#include <operation/page_access.hpp>

#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <mm/memory_object.hpp>
#include <mm/vspace.hpp>
#include <arch/trap.hpp>
#include <object/memory_pool.hpp>

namespace kernel::operation {

/*luna change: publish page outcomes through atomic lifecycle phases, reason: a Pager callback may race Wait admission without a second continuation state*/
PageAccess::PageAccess() noexcept
    : completion_(Completion::bind_resume<
          PageAccess,
          &PageAccess::complete,
          &PageAccess::read,
          &PageAccess::release,
          &PageAccess::cancel,
          &PageAccess::resume,
          &PageAccess::arm>(*this)) {
    completion_.set_policy(diag::concurrency::OperationPolicy{
        .kind = diag::concurrency::WaitKind::Pager,
        .expectation = diag::concurrency::Expectation::ExternalUnbounded,
        .driver = diag::concurrency::NodeRef::external(
            reinterpret_cast<u64>(this), 1),
    });
}

PageAccess::~PageAccess() noexcept {
    KASSERT(!completion_.attached());
    KASSERT(!relation_.attached());
    KASSERT(memory_.load<libk::MemoryOrder::Acquire>() == nullptr);
    KASSERT(!demand_);
    KASSERT(phase_.load<libk::MemoryOrder::Acquire>() == Phase::Idle
        || phase_.load<libk::MemoryOrder::Acquire>() == Phase::Terminal
        || phase_.load<libk::MemoryOrder::Acquire>() == Phase::Canceled);
}

auto PageAccess::active() const noexcept -> bool {
    return phase_.load<libk::MemoryOrder::Acquire>() != Phase::Idle;
}

auto PageAccess::terminal() const noexcept -> bool {
    return phase_.load<libk::MemoryOrder::Acquire>() == Phase::Terminal;
}

auto PageAccess::kind() const noexcept -> mm::FaultKind {
    return static_cast<mm::FaultKind>(kind_.load<libk::MemoryOrder::Acquire>());
}

auto PageAccess::complete() const noexcept -> bool {
    return phase_.load<libk::MemoryOrder::Acquire>() == Phase::Ready;
}

auto PageAccess::read() noexcept -> Result {
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
            : kind == mm::FaultKind::Busy ? MYOS_STATUS_BUSY
            : kind == mm::FaultKind::BackingFailed
                ? MYOS_STATUS_PEER_FAULT
                : MYOS_STATUS_CANCELED,
        .value = 0,
    };
}

void PageAccess::drop_pin() noexcept {
    mm::MemoryObject* const memory = memory_.exchange<
        libk::MemoryOrder::AcqRel>(nullptr);
    if (memory != nullptr) {
        memory->release_fault();
    }
}

void PageAccess::release() noexcept {
    drop_pin();
    if (target_) {
        demand_.reset();
        target_.reset();
        phase_.store<libk::MemoryOrder::Release>(Phase::Idle);
        return;
    }
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
    case Phase::Armed:
        KASSERT(false);
        return;
    }
}

void PageAccess::publish(
    void* owner,
    mm::PageWaitResult result) noexcept {
    auto& fault = *static_cast<PageAccess*>(owner);
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
    // Capture the callback before publishing Ready: the early-completion
    // receiver may release or rearm this owner immediately after the exchange.
    Completion* const completion = &fault.completion_;
    const Phase previous_phase = fault.phase_.exchange<libk::MemoryOrder::AcqRel>(Phase::Ready);
    KASSERT(previous_phase == Phase::Attaching || previous_phase == Phase::Pending
        || previous_phase == Phase::Armed);
    if (previous_phase == Phase::Armed) completion->signal();
}

void PageAccess::arm() noexcept {
    Phase expected = Phase::Pending;
    if (phase_.compare_exchange_strong<libk::MemoryOrder::AcqRel, libk::MemoryOrder::Acquire>(
            expected, Phase::Armed)) return;
    KASSERT(expected == Phase::Ready);
    completion_.signal();
}

/*luna change: remove the duplicate PageAccess error classifier, reason: VSpace owns the shared fault boundary mapping*/

auto PageAccess::resolve() noexcept -> mm::FaultKind {
    if (target_) {
        auto object = target_.pin<mm::MemoryObject>();
        if (!object) return mm::FaultKind::BackingFailed;
        auto& memory = object.value().get();
        auto result = memory.materialize(page_, &relation_, this, &PageAccess::publish, &demand_);
        if (result) return mm::FaultKind::Ready;
        if (result.error() == mm::MemoryError::Pending || result.error() == mm::MemoryError::Pressure)
            memory_.store<libk::MemoryOrder::Release>(&memory);
        return mm::fault_kind(result.error());
    }
    const auto result = vspace_->fault(
        mm::VmContext{.cpus = cpus_, .local = local_}, address_, access_,
        &relation_, this, &PageAccess::publish, &demand_);
    if (!result) return mm::fault_kind(result.error());
    if (result.value().kind == mm::FaultKind::Pending || result.value().kind == mm::FaultKind::Pressure)
        memory_.store<libk::MemoryOrder::Release>(result.value().memory);
    return result.value().kind;
}

auto PageAccess::populate(object::ObjectRef&& memory, usize page) noexcept -> mm::FaultKind {
    KASSERT(!active() && !completion_.attached() && !relation_.attached());
    KASSERT(memory && memory_.load<libk::MemoryOrder::Acquire>() == nullptr && !demand_);
    target_ = libk::move(memory);
    page_ = page;
    return admit();
}

auto PageAccess::admit() noexcept -> mm::FaultKind {
    KASSERT(target_ || (vspace_ != nullptr && cpus_ != nullptr));
    KASSERT(!relation_.attached());
    phase_.store<libk::MemoryOrder::Release>(Phase::Attaching);
    kind_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(mm::FaultKind::Pending));
    const mm::FaultKind next = resolve();
    if (next == mm::FaultKind::Pending || next == mm::FaultKind::Pressure) {
        if (memory_.load<libk::MemoryOrder::Acquire>() == nullptr) {
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

auto PageAccess::start(
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
    target_.reset();
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

auto PageAccess::cancel() noexcept -> bool {
    const auto phase = phase_.load<libk::MemoryOrder::Acquire>();
    if (phase != Phase::Pending && phase != Phase::Armed) {
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

void PageAccess::reset() noexcept {
    KASSERT(!completion_.attached() && !relation_.attached());
    KASSERT(memory_.load<libk::MemoryOrder::Acquire>() == nullptr);
    KASSERT(!demand_);
    phase_.store<libk::MemoryOrder::Release>(Phase::Idle);
    target_.reset();
    page_ = 0;
    vspace_ = nullptr;
    cpus_ = nullptr;
    local_ = {};
    address_ = {};
    access_ = mm::Access::Read;
    generation_.store<libk::MemoryOrder::Relaxed>(0);
    kind_.store<libk::MemoryOrder::Relaxed>(
        static_cast<u8>(mm::FaultKind::NoMapping));
}

auto PageAccess::resume(arch::TrapContext& trap) noexcept
    -> Completion::ResumeResult {
    static_cast<void>(trap);
    KASSERT(phase_.load<libk::MemoryOrder::Acquire>() == Phase::Ready);
    drop_pin();
    if (kind() == mm::FaultKind::OutOfMemory
        || kind() == mm::FaultKind::ResourceExhausted
        || kind() == mm::FaultKind::BackingFailed) {
        demand_.reset();
        phase_.store<libk::MemoryOrder::Release>(Phase::Terminal);
        if (target_) trap.set_result(0, static_cast<usize>(static_cast<isize>(status())));
        return Completion::ResumeResult::Done;
    }
    phase_.store<libk::MemoryOrder::Release>(Phase::Idle);
    const mm::FaultKind next = admit();
    const Completion::ResumeResult result =
        next == mm::FaultKind::Pending || next == mm::FaultKind::Pressure
        ? Completion::ResumeResult::Rearm
        : Completion::ResumeResult::Done;
    if (target_ && result == Completion::ResumeResult::Done)
        trap.set_result(0, static_cast<usize>(static_cast<isize>(status())));
    return result;
}

} // namespace kernel::operation
