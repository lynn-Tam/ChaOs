#include <core/kernel_state.hpp>
#include <arch/interrupt.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <libk/utility.hpp>
#include <libk/limits.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/vspace.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>

namespace kernel {
namespace {

[[nodiscard]] auto advance_sat(libk::Atomic<u64>& value) noexcept -> u64 {
    u64 current = value.load<libk::MemoryOrder::Relaxed>();
    for (;;) {
        if (current == libk::numeric_limits<u64>::max()) {
            return current;
        }
        if (value.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Relaxed>(current, current + 1)) {
            return current + 1;
        }
    }
}

} // namespace

auto KernelState::initialize_in(
    libk::ManualLifetime<KernelState>& storage,
    kernel::mm::RegionList&& memory_map,
    kernel::mm::DirectMapLayout direct_map) noexcept
    -> InitializationResult {
    KernelState& kernel = storage.emplace(ConstructionKey{});
    const auto direct = kernel::mm::DirectMap::initialize_in(
        kernel.direct_map_,
        memory_map,
        direct_map);
    if (!direct) {
        storage.reset();
        return libk::unexpected(kernel::mm::PmmInitError::InvalidRegion);
    }
    auto result = kernel::mm::Pmm::initialize_in(
        kernel.pmm_,
        *kernel.direct_map_,
        libk::move(memory_map));

    if (result) {
        return libk::expected();
    }

    const kernel::mm::PmmInitError error = result.error();
    storage.reset();
    return libk::unexpected(error);
}

KernelState::~KernelState() noexcept {
    diag::concurrency::unbind_report_notifier();
    if (objects_) {
        objects().unbind_reclaim_notifier();
    }
    if (grants_) {
        grants().unbind_work_notifier();
    }
    vspace_work_.unbind_notifier();
    memory_work_.unbind_notifier();
    cpus_.reset();
    release_scheduler_objects();
    grants_.reset();
    objects_.reset();
    clock_.reset();
    kernel_vspace_.reset();
    pmm_.reset();
    direct_map_.reset();
}

auto KernelState::initialize_kernel_domain(usize cpu_count) noexcept -> bool {
    if (kernel_domain_ || cpu_count == 0) {
        return false;
    }
    auto capacity = kernel::sched::DomainCapacity::create(pmm(), cpu_count);
    if (!capacity) {
        return false;
    }
    auto pending = objects().create_domain(
        libk::move(capacity).value(),
        kernel::sched::SchedulingDomain::share_scale,
        100'000U);
    if (!pending) {
        return false;
    }
    kernel_domain_ = libk::move(pending).value().publish();
    return true;
}

auto KernelState::initialize_root_pool(
    kernel::resource::Budget limit) noexcept -> bool {
    if (root_pool_ || limit.memory == 0 || limit.caps == 0) {
        return false;
    }
    auto pending = objects().create_resource(limit);
    if (!pending) {
        return false;
    }
    root_pool_ = libk::move(pending).value().publish();
    return true;
}

auto KernelState::start_reclaimer(
    kernel::CpuRuntime& runtime) noexcept -> bool {
    if (!kernel_domain_ || reclaimer_thread_ || reclaimer_context_
        || arch::interrupts_enabled()) {
        return false;
    }

    auto stack = kernel::KernelStack::create(kernel_vspace());
    if (!stack) {
        return false;
    }
    auto pending_thread = objects().create_thread(
        libk::move(stack).value(),
        kernel::ExecutionBinding::kernel(kernel_vspace()),
        kernel::Thread::KernelStart{reclaimer_entry, this});
    if (!pending_thread) {
        return false;
    }
    auto thread = libk::move(pending_thread).value().publish();

    const auto budget = clock().duration_from_nanoseconds(1'000'000);
    const auto period = clock().duration_from_nanoseconds(10'000'000);
    const auto urgency = kernel::sched::Urgency::make(31);
    if (!budget || !period || !urgency) {
        KASSERT(thread.retire());
        thread.reset();
        objects().drain_reclaim();
        return false;
    }
    auto pending_context = objects().create_context(
        kernel::sched::SchedulingContext::Config{
            .budget = *budget,
            .period = *period,
            .urgency = *urgency,
        },
        clock().now());
    if (!pending_context) {
        KASSERT(thread.retire());
        thread.reset();
        objects().drain_reclaim();
        return false;
    }
    auto context = libk::move(pending_context).value().publish();

    auto admitted = kernel_domain_.get().admit(
        context.get(), runtime.local.descriptor->logical_id());
    auto target = thread.clone();
    if (!admitted || !target
        || !context->bind(
            target ? libk::move(target).value()
                   : kernel::object::ObjectStore::ThreadHold{})) {
        if (context->admitted()) {
            KASSERT(kernel_domain_.get().unadmit(context.get()));
        }
        KASSERT(context.retire());
        context.reset();
        KASSERT(thread.retire());
        thread.reset();
        objects().drain_reclaim();
        return false;
    }

    KASSERT(runtime.dispatcher().make_ready(*context->binding()));
    reclaimer_thread_ = libk::move(thread);
    reclaimer_context_ = libk::move(context);
    reclaimer_observation_ =
        diag::concurrency::ObservationLease::reserve_on(
            runtime.local.descriptor->logical_id(),
            diag::concurrency::RecordKind::ServiceActor,
            reinterpret_cast<u64>(this),
            1,
            diag::concurrency::Expectation::SchedulerControlled);
    reclaimer_observation_.watch(true);
    objects().bind_reclaim_notifier(
        kernel::object::ObjectStore::ReclaimNotifier::bind<
            &KernelState::wake_reclaimer>(*this));
    memory_work_.bind_notifier(
        kernel::mm::MemoryExecutor::Notifier::bind<
            &KernelState::wake_reclaimer>(*this));
    vspace_work_.bind_notifier(
        kernel::mm::VSpaceExecutor::Notifier::bind<
            &KernelState::wake_reclaimer>(*this));
    grants().bind_work_notifier(
        kernel::cap::GrantGraph::WorkNotifier::bind<
            &KernelState::wake_reclaimer>(*this));
    if (diag::concurrency::enabled(diag::concurrency::Level::Watch)) {
        KASSERT(diag::concurrency::bind_report_notifier(
            diag::concurrency::ReportCallback::bind<
                &KernelState::wake_report_consumer>(*this)));
    }

    return true;
}


[[noreturn]] void KernelState::reclaimer_entry(void* argument) noexcept {
    auto& kernel = *static_cast<KernelState*>(argument);
    u64 cycle{};
    u64 object_total{};
    u64 grant_total{};
    u64 vspace_total{};
    u64 memory_total{};
    const auto add_progress = [](
                                  u64& total,
                                  usize delta) noexcept {
        const u64 amount = static_cast<u64>(delta);
        total = total > libk::numeric_limits<u64>::max() - amount
            ? libk::numeric_limits<u64>::max()
            : total + amount;
    };
    for (;;) {
        const u64 admitted = kernel.reclaimer_enqueues_.load<
            libk::MemoryOrder::Acquire>();
        const auto work = diag::concurrency::ObservationKey{
            kernel.reclaimer_observation_key_.load<
                libk::MemoryOrder::Acquire>()};
        if (cycle != libk::numeric_limits<u64>::max()) {
            ++cycle;
        }
        kernel::CpuLocal& cpu = kernel::current_cpu();
        KASSERT(cpu.runtime().owner_registry != nullptr);
        static_cast<void>(diag::concurrency::drain_reports(kernel.cpus()));
        const auto driver = kernel.reclaimer_context_
            && kernel.reclaimer_context_->binding() != nullptr
            ? kernel.reclaimer_context_->binding()->actor_ref()
            : diag::concurrency::NodeRef::cpu(
                  cpu.descriptor->logical_id());
        kernel.reclaimer_observation_.attempt(
            1,
            diag::concurrency::WaitKind::ObjectReclaim,
            driver);
        auto work_observation =
            diag::concurrency::ObservationLease::borrow(work);
        work_observation.attempt(
            static_cast<u32>(diag::concurrency::ServicePhase::Running),
            diag::concurrency::WaitKind::ObjectReclaim,
            driver);
        const usize object_count = kernel.objects().drain_reclaim();
        add_progress(object_total, object_count);
        kernel.reclaimer_observation_.advance(object_count);
        mm::WaitClaim pressure_ready[mm::PageReclaimer::pass_budget]{};
        const usize ready_count = kernel.pressure_work_.wake(
            kernel.pmm().frame_progress_generation(),
            pressure_ready,
            mm::PageReclaimer::pass_budget);
        /*luna change: consume already-finalized pressure claims without a relation callback unlink, reason: PageReclaimer closes relation reuse before publishing readiness*/
        for (usize index = 0; index < ready_count; ++index) {
            mm::WaitClaim claim = libk::move(pressure_ready[index]);
            /*luna change: assert finalized pressure claim delivery, reason: wake already completed host unlink and cannot leave an unconsumed snapshot*/
            KASSERT(claim);
            KASSERT(claim.publish());
            KASSERT(claim.release());
            claim.reset();
        }
        work_observation.advance(object_count);
        diag::concurrency::ObservationBatch object_update{
            .phase = 1,
            .semantic_stamp = 1,
            .wait = diag::concurrency::WaitKind::ObjectReclaim,
            .driver = driver,
            .site = diag::concurrency::SourceSite::current(),
            .detail_mask = 0xfU,
            .update_progress = false};
        object_update.detail[0] = cycle;
        object_update.detail[1] = object_total;
        object_update.detail[2] = grant_total;
        object_update.detail[3] = vspace_total;
        kernel.reclaimer_observation_.publish(object_update);
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Object,
            diag::concurrency::FlightEvent::Reclaim,
            driver.identity,
            object_count);
        kernel.reclaimer_observation_.attempt(
            2,
            diag::concurrency::WaitKind::GrantWork,
            driver);
        const auto grant = kernel.grants().service(8);
        add_progress(grant_total, grant.progressed);
        kernel.reclaimer_observation_.advance(grant.progressed);
        work_observation.advance(grant.progressed);
        diag::concurrency::ObservationBatch grant_update{
            .phase = 3,
            .semantic_stamp = 3,
            .wait = diag::concurrency::WaitKind::VSpaceWork,
            .driver = driver,
            .site = diag::concurrency::SourceSite::current(),
            .detail_mask = 0xfU,
            .update_progress = false};
        grant_update.detail[0] = cycle;
        grant_update.detail[1] = object_total;
        grant_update.detail[2] = grant_total;
        grant_update.detail[3] = vspace_total;
        kernel.reclaimer_observation_.publish(grant_update);
        const auto vspace = kernel.vspace_work_.run(
            kernel::mm::VmContext{
                .cpus = cpu.runtime().owner_registry,
                .local = cpu.descriptor->logical_id(),
            },
            8);
        add_progress(vspace_total, vspace.progressed);
        kernel.reclaimer_observation_.advance(vspace.progressed);
        work_observation.advance(vspace.progressed);
        diag::concurrency::ObservationBatch vspace_update{
            .phase = 4,
            .semantic_stamp = 4,
            .wait = diag::concurrency::WaitKind::SchedulerReady,
            .driver = driver,
            .site = diag::concurrency::SourceSite::current(),
            .detail_mask = 0xfU,
            .update_progress = false};
        vspace_update.detail[0] = cycle;
        vspace_update.detail[1] = object_total;
        vspace_update.detail[2] = grant_total;
        vspace_update.detail[3] = vspace_total;
        kernel.reclaimer_observation_.publish(vspace_update);
        const auto memory = kernel.memory_work_.run(8);
        add_progress(memory_total, memory.progressed);
        kernel.reclaimer_observation_.advance(memory.progressed);
        work_observation.advance(memory.progressed);
        if (grant.more || vspace.more || memory.more
            || !kernel.close_reclaimer_work(admitted)) {
            kernel.reclaimer_observation_.watch(true);
            kernel::sched::yield();
        } else {
            kernel.reclaimer_observation_.attempt(
                5,
                diag::concurrency::WaitKind::SchedulerWake,
                driver);
            kernel.reclaimer_observation_.watch(false);
            kernel::sched::block();
        }
    }
}

auto KernelState::wake_report_consumer() noexcept -> bool {
    if (!reclaimer_context_ || !cpus_) {
        return false;
    }
    kernel::sched::Binding* const binding = reclaimer_context_->binding();
    if (binding == nullptr) {
        return false;
    }
    const kernel::CpuDescriptor* const target =
        cpus().descriptor(binding->home_cpu());
    if (target == nullptr || target->state() != kernel::CpuState::Online) {
        return false;
    }
    // This is only a scheduler wake hint. It does not retain reclaimer work,
    // increment the canonical enqueue epoch, or publish a diagnostic key.
    return static_cast<bool>(kernel::sched::try_wake(cpus(), *binding));
}

auto KernelState::retain_reclaimer_work() noexcept
    -> diag::concurrency::ObservationKey {
    for (;;) {
        const u64 state = reclaimer_observation_key_.load<
            libk::MemoryOrder::Acquire>();
        if (state != 0) {
            return diag::concurrency::ObservationKey{state};
        }

        const u64 generation = advance_sat(
            reclaimer_observation_generation_);
        auto observation = diag::concurrency::ObservationLease::reserve(
            diag::concurrency::RecordKind::ServiceWork,
            reinterpret_cast<u64>(this),
            generation,
            diag::concurrency::Expectation::InternalFinite);
        const auto driver = reclaimer_context_
                && reclaimer_context_->binding() != nullptr
            ? reclaimer_context_->binding()->actor_ref()
            : diag::concurrency::NodeRef{};
        diag::concurrency::ObservationBatch update{
            .phase = static_cast<u32>(diag::concurrency::ServicePhase::Queued),
            .semantic_stamp = 0,
            .wait = diag::concurrency::WaitKind::SchedulerWake,
            .driver = driver,
            .site = diag::concurrency::SourceSite::current(),
            .update_progress = false,
            .update_watched = true,
            .watched = true};
        observation.publish(update);
        const auto key = observation.detach_key();
        if (!key) {
            return {};
        }
        u64 expected = 0;
        if (reclaimer_observation_key_.compare_exchange_strong<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(expected, key.raw)) {
            return key;
        }
        diag::concurrency::ObservationLease::borrow(key).finish(
            static_cast<u32>(diag::concurrency::ServicePhase::Completed));
    }
}

auto KernelState::close_reclaimer_work(
    u64 admitted) noexcept -> bool {
    // The epoch is the only canonical close predicate. Binding::wake_credit
    // retains an enqueue that races the final epoch sample, so a diagnostic
    // key must not be exchanged into a closing/reopen protocol here.
    if (reclaimer_enqueues_.load<libk::MemoryOrder::Acquire>()
        != admitted) {
        return false;
    }

    const auto work = diag::concurrency::ObservationKey{
        reclaimer_observation_key_.exchange<libk::MemoryOrder::AcqRel>(0)};
    if (work) {
        diag::concurrency::ObservationLease::borrow(work)
            .finish(static_cast<u32>(diag::concurrency::ServicePhase::Completed));
    }
    // A concurrent enqueue after the final sample leaves canonical wake credit
    // on the Binding and forces the caller to run another service pass.
    return reclaimer_enqueues_.load<libk::MemoryOrder::Acquire>() == admitted;
}

auto KernelState::wake_reclaimer() noexcept
    -> diag::concurrency::ObservationKey {
    static_cast<void>(advance_sat(reclaimer_enqueues_));
    const auto work = retain_reclaimer_work();
    KASSERT(reclaimer_context_);
    kernel::sched::Binding* const binding = reclaimer_context_->binding();
    KASSERT(binding != nullptr);

    const kernel::CpuDescriptor* const target =
        cpus().descriptor(binding->home_cpu());
    KASSERT(target != nullptr);
    if (target->state() != kernel::CpuState::Online) {
        // Before the first dispatch the already-Ready reclaimer will observe
        // all queued work without a wake. Teardown removes the notifier first.
        return work;
    }
    diag::concurrency::ObservationKey delivery{};
    KASSERT(kernel::sched::wake(cpus(), *binding, work, &delivery));
    auto observation =
        diag::concurrency::ObservationLease::borrow(work);
    observation.attempt(
        static_cast<u32>(diag::concurrency::ServicePhase::WakeIssued),
        diag::concurrency::WaitKind::SchedulerWake,
        delivery
            ? diag::concurrency::NodeRef::observation(delivery)
            : binding->actor_ref());
    return work;
}

void KernelState::release_scheduler_objects() noexcept {
    if (reclaimer_context_) {
        if (reclaimer_context_->binding() != nullptr) {
            KASSERT(reclaimer_context_->unbind());
        }
        if (reclaimer_context_->admitted()) {
            KASSERT(kernel_domain_.get().unadmit(reclaimer_context_.get()));
        }
        KASSERT(reclaimer_context_.retire());
        reclaimer_context_.reset();
    }
    if (reclaimer_thread_) {
        KASSERT(reclaimer_thread_.retire());
        reclaimer_thread_.reset();
    }
    if (kernel_domain_) {
        KASSERT(kernel_domain_.retire());
        kernel_domain_.reset();
    }
    if (objects_) {
        objects().drain_reclaim();
    }
}


auto KernelState::initialize_object_store() noexcept -> bool {
    if (objects_) {
        return false;
    }
    /*luna change: construct ObjectStore with the shared MemoryExecutor, reason: MemoryObject admission must use one runtime service owner*/
    [[maybe_unused]] auto& store = objects_.emplace(
        pmm(), vspace_work_, memory_work_);
    return true;
}

auto KernelState::initialize_grants() noexcept -> bool {
    if (!objects_ || grants_) {
        return false;
    }
    [[maybe_unused]] auto& graph = grants_.emplace(pmm());
    return true;
}

auto KernelState::initialize_clock(u64 ticks_per_second) noexcept -> bool {
    if (ticks_per_second == 0 || clock_) {
        return false;
    }
    auto& configured = clock_.emplace(ticks_per_second);
    return configured.valid();
}

auto KernelState::initialize_kernel_vspace() noexcept
    -> KernelVSpaceInitResult {
    return kernel::mm::KernelVSpace::initialize_in(kernel_vspace_, pmm());
}

auto KernelState::begin_cpus(
    kernel::CpuTopologySummary summary) noexcept -> CpuBeginResult {
    return kernel::CpuRegistry::begin(
        cpus_,
        pmm(),
        summary);
}

} // namespace kernel
