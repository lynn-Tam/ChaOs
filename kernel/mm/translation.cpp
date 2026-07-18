#include <mm/translation.hpp>

#include <arch/instruction.hpp>
#include <arch/ipi.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <libk/limits.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

ShootdownTicket::~ShootdownTicket() noexcept {
    KASSERT(!initialized() || complete());
}

auto ShootdownTicket::complete() const noexcept -> bool {
    return initialized() && completion_.complete();
}

auto ShootdownTicket::epoch() const noexcept -> TranslationEpoch {
    KASSERT(initialized());
    return epoch_;
}

auto ShootdownTicket::instruction_epoch() const noexcept
    -> InstructionEpoch {
    KASSERT(initialized());
    return instruction_epoch_;
}

auto ShootdownTicket::targets() const noexcept -> const kernel::CpuSet& {
    KASSERT(initialized());
    return targets_;
}

auto ShootdownTicket::acknowledged(kernel::CpuId cpu) const noexcept -> bool {
    KASSERT(initialized());
    if (!targets_.contains(cpu)) {
        return false;
    }
    const usize word = cpu.raw / kernel::CpuSet::word_bits;
    const u64 bit = u64{1} << (cpu.raw % kernel::CpuSet::word_bits);
    return (acknowledgements_[word].load<libk::MemoryOrder::Acquire>()
        & bit) != 0;
}

void ShootdownTicket::initialize(
    TranslationState& owner,
    TranslationEpoch epoch,
    InstructionEpoch instruction_epoch,
    bool instruction_sync,
    kernel::CpuSet targets) noexcept {
    KASSERT(!initialized());
    KASSERT(epoch.raw != 0);
    for (auto& word : acknowledgements_) {
        word.store<libk::MemoryOrder::Relaxed>(0);
    }
    epoch_ = epoch;
    instruction_epoch_ = instruction_epoch;
    instruction_sync_ = instruction_sync;
    targets_ = targets;
    completion_.initialize(targets.size());
    owner_ = &owner;
    if (!targets.empty()) {
        static_cast<void>(owner.pending_tickets_.fetch_add<
            libk::MemoryOrder::Relaxed>(1));
    }
}

void ShootdownTicket::acknowledge(kernel::CpuId cpu) noexcept {
    KASSERT(initialized());
    KASSERT(targets_.contains(cpu));
    const usize word = cpu.raw / kernel::CpuSet::word_bits;
    const u64 bit = u64{1} << (cpu.raw % kernel::CpuSet::word_bits);
    const u64 previous = acknowledgements_[word].fetch_or<
        libk::MemoryOrder::AcqRel>(bit);
    if ((previous & bit) == 0) {
        completion_.acknowledge([&]() noexcept {
            const usize owner_pending = owner_->pending_tickets_.fetch_sub<
                libk::MemoryOrder::Release>(1);
            KASSERT(owner_pending != 0);
        });
    }
}

RetireBatch::~RetireBatch() noexcept {
    KASSERT(ticket_ == nullptr || ticket_->complete());
    pages_.reset();
    charge_.reset();
    if (owner_ != nullptr) {
        const usize pending = owner_->pending_retires_.fetch_sub<
            libk::MemoryOrder::Release>(1);
        KASSERT(pending != 0);
    }
}

void RetireBatch::bind(ShootdownTicket& ticket) noexcept {
    KASSERT(ticket_ == nullptr);
    KASSERT(ticket.initialized());
    ticket_ = &ticket;
    owner_ = ticket.owner_;
    static_cast<void>(owner_->pending_retires_.fetch_add<
        libk::MemoryOrder::Relaxed>(1));
}

auto RetireBatch::ready() const noexcept -> bool {
    return ticket_ == nullptr || ticket_->complete();
}

auto RetireBatch::release() noexcept -> bool {
    if (!ready()) {
        return false;
    }
    pages_.reset();
    charge_.reset();
    ticket_ = nullptr;
    if (owner_ != nullptr) {
        const usize pending = owner_->pending_retires_.fetch_sub<
            libk::MemoryOrder::Release>(1);
        KASSERT(pending != 0);
        owner_ = nullptr;
    }
    return true;
}

auto ShootdownQueue::reserve() noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (requests_.size() + reserved_ >= capacity) {
        return false;
    }
    ++reserved_;
    return true;
}

void ShootdownQueue::cancel() noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(reserved_ != 0);
    --reserved_;
}

void ShootdownQueue::publish(ShootdownRequest request) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(request.ticket != nullptr);
    KASSERT(reserved_ != 0);
    --reserved_;
    KASSERT(requests_.try_push_back(request));
    delivery_.publish();
}

auto ShootdownQueue::claim_transport() noexcept
    -> libk::optional<kernel::IpiDelivery::Token> {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(!requests_.empty() || !delivery_.pending());
    return delivery_.claim();
}

void ShootdownQueue::transport_failed(
    kernel::IpiDelivery::Token token) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    delivery_.fail(token);
}

void ShootdownQueue::take_all(
    libk::InplaceRing<ShootdownRequest, capacity>& batch) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(batch.empty());
    while (!requests_.empty()) {
        KASSERT(batch.try_push_back(requests_.front()));
        requests_.pop_front();
    }
    delivery_.consume();
}

ShootdownPlan::ShootdownPlan(ShootdownPlan&& other) noexcept
    : cpus_(libk::exchange(other.cpus_, nullptr)),
      local_(other.local_),
      targets_(other.targets_),
      reserved_(other.reserved_) {
    other.reserved_ = {};
}

ShootdownPlan::~ShootdownPlan() noexcept {
    cancel();
}

auto ShootdownPlan::prepare(
    kernel::CpuRegistry& cpus,
    kernel::CpuId local,
    const kernel::CpuSet& targets) noexcept
    -> libk::Expected<ShootdownPlan, ShootdownError> {
    kernel::CpuSet reserved{};
    ShootdownError error = ShootdownError::TargetUnavailable;
    bool failed{};
    targets.for_each([&](kernel::CpuId target) noexcept {
        if (failed || target == local) {
            return;
        }
        kernel::CpuRuntime* const runtime = cpus.runtime(target);
        if (runtime == nullptr) {
            failed = true;
            return;
        }
        if (!runtime->shootdowns.reserve()) {
            error = ShootdownError::QueueFull;
            failed = true;
            return;
        }
        KASSERT(reserved.insert(target));
    });
    if (failed) {
        reserved.for_each([&](kernel::CpuId target) noexcept {
            kernel::CpuRuntime* const runtime = cpus.runtime(target);
            KASSERT(runtime != nullptr);
            runtime->shootdowns.cancel();
        });
        return libk::unexpected(error);
    }
    return libk::expected(ShootdownPlan{
        &cpus, local, targets, reserved});
}

auto ShootdownPlan::local(
    kernel::CpuId local,
    const kernel::CpuSet& targets) noexcept
    -> libk::Expected<ShootdownPlan, ShootdownError> {
    bool remote{};
    targets.for_each([&](kernel::CpuId target) noexcept {
        remote = remote || target != local;
    });
    if (remote) {
        return libk::unexpected(ShootdownError::TargetUnavailable);
    }
    return libk::expected(ShootdownPlan{
        nullptr, local, targets, kernel::CpuSet{}});
}

auto ShootdownPlan::publish(ShootdownTicket& ticket) noexcept
    -> kernel::CpuSet {
    reserved_.for_each([&](kernel::CpuId target) noexcept {
        KASSERT(cpus_ != nullptr);
        kernel::CpuRuntime* const runtime = cpus_->runtime(target);
        KASSERT(runtime != nullptr);
        runtime->shootdowns.publish(ShootdownRequest{&ticket, target});
    });
    const kernel::CpuSet published = reserved_;
    reserved_ = {};
    return published;
}

auto ShootdownPlan::kick(const kernel::CpuSet& targets) noexcept -> bool {
    bool success = true;
    targets.for_each([&](kernel::CpuId target) noexcept {
        KASSERT(cpus_ != nullptr);
        const kernel::CpuDescriptor* const descriptor = cpus_->descriptor(target);
        kernel::CpuRuntime* const runtime = cpus_->runtime(target);
        KASSERT(descriptor != nullptr);
        KASSERT(runtime != nullptr);
        const auto transport = runtime->shootdowns.claim_transport();
        if (!transport) {
            return;
        }
        if (!arch::send_ipi(descriptor->hardware_id())) {
            runtime->shootdowns.transport_failed(*transport);
            success = false;
        }
    });
    return success;
}

void ShootdownPlan::cancel() noexcept {
    if (reserved_.empty()) {
        return;
    }
    KASSERT(cpus_ != nullptr);
    reserved_.for_each([&](kernel::CpuId target) noexcept {
        kernel::CpuRuntime* const runtime = cpus_->runtime(target);
        KASSERT(runtime != nullptr);
        runtime->shootdowns.cancel();
    });
    reserved_ = {};
}

TranslationState::Mutation::Mutation(Mutation&& other) noexcept
    : owner_(libk::exchange(other.owner_, nullptr)),
      interrupts_(other.interrupts_),
      targets_(other.targets_) {}

TranslationState::Mutation::~Mutation() noexcept {
    unlock();
}

void TranslationState::Mutation::unlock() noexcept {
    if (owner_ == nullptr) {
        return;
    }
    owner_->lock_.unlock();
    arch::restore_interrupts(interrupts_);
    owner_ = nullptr;
}

auto TranslationState::Mutation::commit(
    ShootdownPlan&& plan,
    ShootdownTicket& ticket,
    RetireBatch* retire,
    bool instruction_sync) noexcept -> ShootdownStatus {
    KASSERT(owner_ != nullptr);
    KASSERT(!ticket.initialized());
    KASSERT(plan.targets_ == targets_);
    KASSERT(owner_->epoch_.raw != libk::numeric_limits<u64>::max());

    const TranslationEpoch issued{owner_->epoch_.raw + 1};
    owner_->epoch_ = issued;
    if (instruction_sync) {
        KASSERT(owner_->instruction_epoch_.raw
            != libk::numeric_limits<u64>::max());
        owner_->instruction_epoch_ = InstructionEpoch{
            owner_->instruction_epoch_.raw + 1};
    }
    ticket.initialize(
        *owner_, issued, owner_->instruction_epoch_, instruction_sync, targets_);
    if (retire != nullptr) {
        retire->bind(ticket);
    }

    const kernel::CpuSet kicks = plan.publish(ticket);
    if (targets_.contains(plan.local_)) {
        arch::flush_tlb_all();
        if (instruction_sync) {
            arch::sync_instruction_stream();
        }
        ticket.acknowledge(plan.local_);
    }

    // Activation may now observe the new epoch. Interrupts remain masked until
    // every retained request is published and each required transport kick is
    // attempted, so a stack-resident ticket cannot be descheduled midway.
    owner_->lock_.unlock();
    owner_ = nullptr;
    static_cast<void>(plan.kick(kicks));
    arch::restore_interrupts(interrupts_);

    if (ticket.complete()
        || (ticket.notifiable() && !ticket.arm())) {
        return ShootdownStatus::Complete;
    }
    return ShootdownStatus::Pending;
}

void TranslationState::Mutation::publish_fresh(bool instruction_sync) noexcept {
    KASSERT(owner_ != nullptr);
    KASSERT(owner_->epoch_.raw != libk::numeric_limits<u64>::max());
    owner_->epoch_ = TranslationEpoch{owner_->epoch_.raw + 1};
    if (instruction_sync) {
        KASSERT(owner_->instruction_epoch_.raw
            != libk::numeric_limits<u64>::max());
        owner_->instruction_epoch_ = InstructionEpoch{
            owner_->instruction_epoch_.raw + 1};
    }

    // The local hart may have speculatively cached an invalid walk. Remote
    // harts cannot have named this monotonically allocated range yet.
    arch::flush_tlb_all();
    if (instruction_sync) {
        arch::sync_instruction_stream();
    }
    unlock();
}

TranslationState::~TranslationState() noexcept {
    KASSERT(active_.empty());
    KASSERT(pending_tickets() == 0);
    KASSERT(pending_retires() == 0);
}

auto TranslationState::enter(kernel::CpuId cpu) noexcept
    -> TranslationObservation {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(cpu.raw < kernel::max_cpu_count);
    KASSERT(active_.insert(cpu));
    return TranslationObservation{epoch_, instruction_epoch_};
}

void TranslationState::leave(kernel::CpuId cpu) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(active_.erase(cpu));
}

auto TranslationState::begin() noexcept
    -> libk::Expected<Mutation, ShootdownError> {
    const arch::InterruptState interrupts = arch::disable_interrupts();
    lock_.lock();
    if (epoch_.raw == libk::numeric_limits<u64>::max()) {
        lock_.unlock();
        arch::restore_interrupts(interrupts);
        return libk::unexpected(ShootdownError::EpochExhausted);
    }
    return libk::expected(Mutation{*this, interrupts, active_});
}

auto TranslationState::epoch() const noexcept -> TranslationEpoch {
    kernel::sync::IrqLockGuard guard{lock_};
    return epoch_;
}

auto TranslationState::observation() const noexcept
    -> TranslationObservation {
    kernel::sync::IrqLockGuard guard{lock_};
    return TranslationObservation{epoch_, instruction_epoch_};
}

auto TranslationState::active_cpus() const noexcept -> kernel::CpuSet {
    kernel::sync::IrqLockGuard guard{lock_};
    return active_;
}

void TranslationView::activate(kernel::CpuLocal& cpu) const noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(cpu.descriptor != nullptr);
    const kernel::CpuId id = cpu.descriptor->logical_id();
    TranslationState* const outgoing = cpu.active_translation_;
    if (outgoing == state_) {
        KASSERT(cpu.active_root_ && cpu.active_root_ == root_);
        const TranslationObservation observed = state_->observation();
        cpu.observed_epoch_ = observed.translation;
        if (cpu.observed_instruction_epoch_.raw
            < observed.instruction.raw) {
            arch::sync_instruction_stream();
            cpu.observed_instruction_epoch_ = observed.instruction;
        }
        return;
    }

    const TranslationObservation observed = state_->enter(id);
    arch::activate_root(root_);
    if (observed.instruction.raw != 0) {
        arch::sync_instruction_stream();
    }
    cpu.active_translation_ = state_;
    cpu.active_root_ = root_;
    cpu.observed_epoch_ = observed.translation;
    cpu.observed_instruction_epoch_ = observed.instruction;
    if (outgoing != nullptr) {
        outgoing->leave(id);
    }
}

void TranslationView::adopt(kernel::CpuLocal& cpu) const noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(cpu.descriptor != nullptr);
    KASSERT(cpu.active_translation_ == nullptr);
    KASSERT(!cpu.active_root_);
    KASSERT(arch::root_active(root_));
    const TranslationObservation observed = state_->enter(
        cpu.descriptor->logical_id());
    if (observed.instruction.raw != 0) {
        arch::sync_instruction_stream();
    }
    cpu.active_translation_ = state_;
    cpu.active_root_ = root_;
    cpu.observed_epoch_ = observed.translation;
    cpu.observed_instruction_epoch_ = observed.instruction;
}

void drain_shootdowns(kernel::CpuRuntime& runtime) noexcept {
    KASSERT(!arch::interrupts_enabled());
    libk::InplaceRing<ShootdownRequest, ShootdownQueue::capacity> batch{};
    runtime.shootdowns.take_all(batch);
    if (batch.empty()) {
        return;
    }

    arch::flush_tlb_all();
    bool instruction_synced{};
    while (!batch.empty()) {
        const ShootdownRequest request = batch.front();
        batch.pop_front();
        KASSERT(request.ticket != nullptr);
        KASSERT(request.target == runtime.local.descriptor->logical_id());
        if (request.ticket->instruction_sync_ && !instruction_synced) {
            arch::sync_instruction_stream();
            instruction_synced = true;
        }
        if (runtime.local.active_translation_ == request.ticket->owner_
            && runtime.local.observed_epoch_.raw < request.ticket->epoch_.raw) {
            runtime.local.observed_epoch_ = request.ticket->epoch_;
        }
        if (runtime.local.active_translation_ == request.ticket->owner_
            && runtime.local.observed_instruction_epoch_.raw
                < request.ticket->instruction_epoch_.raw) {
            runtime.local.observed_instruction_epoch_ =
                request.ticket->instruction_epoch_;
        }
        request.ticket->acknowledge(request.target);
    }
}

auto retry_shootdowns(
    kernel::CpuRegistry& cpus,
    const ShootdownTicket& ticket) noexcept -> ShootdownRetry {
    bool attempted{};
    bool delivered = true;
    ticket.targets().for_each([&](kernel::CpuId target) noexcept {
        if (ticket.acknowledged(target)) {
            return;
        }
        kernel::CpuRuntime* const runtime = cpus.runtime(target);
        const kernel::CpuDescriptor* const descriptor = cpus.descriptor(target);
        KASSERT(runtime != nullptr);
        KASSERT(descriptor != nullptr);
        const auto transport = runtime->shootdowns.claim_transport();
        if (!transport) {
            return;
        }
        attempted = true;
        if (!arch::send_ipi(descriptor->hardware_id())) {
            runtime->shootdowns.transport_failed(*transport);
            delivered = false;
        }
    });
    if (!attempted) {
        return ShootdownRetry::Idle;
    }
    return delivered
        ? ShootdownRetry::Delivered
        : ShootdownRetry::TransportFailure;
}

} // namespace kernel::mm
