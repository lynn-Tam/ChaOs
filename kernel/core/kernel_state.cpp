#include <core/kernel_state.hpp>
#include <arch/interrupt.hpp>
#include <cpu/cpu_runtime.hpp>
#include <libk/utility.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/vspace.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>

namespace kernel {

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
    if (objects_) {
        objects().unbind_reclaim_notifier();
    }
    if (grants_) {
        grants().unbind_work_notifier();
    }
    vspace_work_.unbind_notifier();
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
    objects().bind_reclaim_notifier(
        kernel::object::ObjectStore::ReclaimNotifier::bind<
            &KernelState::wake_reclaimer>(*this));
    vspace_work_.bind_notifier(
        kernel::mm::VSpaceExecutor::Notifier::bind<
            &KernelState::wake_reclaimer>(*this));
    grants().bind_work_notifier(
        kernel::cap::GrantGraph::WorkNotifier::bind<
            &KernelState::wake_reclaimer>(*this));

    return true;
}

[[noreturn]] void KernelState::reclaimer_entry(void* argument) noexcept {
    auto& kernel = *static_cast<KernelState*>(argument);
    for (;;) {
        kernel.objects().drain_reclaim();
        kernel::CpuLocal& cpu = kernel::current_cpu();
        KASSERT(cpu.runtime().owner_registry != nullptr);
        const bool grant_more = kernel.grants().service(8);
        const bool vspace_more = kernel.vspace_work_.run(
            kernel::mm::VmContext{
                .cpus = cpu.runtime().owner_registry,
                .local = cpu.descriptor->logical_id(),
            },
            8);
        if (grant_more || vspace_more) {
            kernel::sched::yield();
        } else {
            kernel::sched::block();
        }
    }
}

void KernelState::wake_reclaimer() noexcept {
    KASSERT(reclaimer_context_);
    kernel::sched::Binding* const binding = reclaimer_context_->binding();
    KASSERT(binding != nullptr);

    const kernel::CpuDescriptor* const target =
        cpus().descriptor(binding->home_cpu());
    KASSERT(target != nullptr);
    if (target->state() != kernel::CpuState::Online) {
        // Before the first dispatch the already-Ready reclaimer will observe
        // all queued work without a wake. Teardown removes the notifier first.
        return;
    }
    KASSERT(kernel::sched::wake(cpus(), *binding));
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
    [[maybe_unused]] auto& store = objects_.emplace(pmm(), vspace_work_);
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
