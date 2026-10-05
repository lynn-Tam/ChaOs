#include <expected>
#include <state.hpp>
#include <arch/interrupt.hpp>
#include <cpu/runtime.hpp>
#include <console.hpp>
#include <utility>
#include <limits>
#include <mm/kspace.hpp>
#include <mm/vspace.hpp>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>

auto KernelState::initialize_in(
    libk::ManualLifetime<KernelState>& storage,
    mm::RegionList&& memory_map,
    mm::Pmm::Window direct_map) noexcept
    -> InitializationResult {
    KernelState& kernel = storage.emplace(ConstructionKey{});
    auto result = mm::Pmm::initialize_in(
        kernel.pmm_, std::move(memory_map), direct_map);

    if (result) {
        return {};
    }

    const mm::PmmInitError error = result.error();
    storage.reset();
    return std::unexpected(error);
}

KernelState::~KernelState() noexcept {

    if (objects_) {
        cleanup_notify_.reset();
    }
    if (grants_) {
        grants().unbind_work_notifier();
    }
    io_work_.unbind_notifier();
    vspace_work_.unbind_notifier();
    cpus_.reset();
    release_scheduler_objects();
    grants_.reset();
    objects_.reset();
    clock_.reset();
    kernel_vspace_.reset();
    pmm_.reset();
}

auto KernelState::initialize_kernel_domain(usize cpu_count) noexcept -> bool {
    if (kernel_domain_ || cpu_count == 0) {
        return false;
    }
    auto capacity = sched::DomainCapacity::create(pmm(), cpu_count);
    if (!capacity) {
        return false;
    }
    auto pending = pool<sched::Domain>().create(
        std::move(capacity).value(),
        sched::Domain::share_scale,
        100'000U);
    if (!pending) {
        return false;
    }
    kernel_domain_ = std::move(pending).value().publish();
    return true;
}

auto KernelState::initialize_root_pool(
    resource::budget limit) noexcept -> bool {
    if (root_pool_ || limit.memory == 0 || limit.caps == 0) {
        return false;
    }
    auto pending = pool<object::group>().create(pmm(), limit);
    if (!pending) {
        return false;
    }
    root_pool_ = std::move(pending).value().publish();
    return true;
}

auto KernelState::start_cleanup(
    CpuRuntime& runtime) noexcept -> bool {
    if (!kernel_domain_ || cleanup_thread_ || cleanup_context_
        || arch::interrupts_enabled()) {
        return false;
    }

    auto stack = mm::Stack::create(kernel_vspace());
    if (!stack) {
        return false;
    }
    auto pending_thread = pool<Thread>().create(
        std::move(stack).value(),
        Env::kernel(kernel_vspace()),
        Thread::KernelStart{cleanup_entry, this});
    if (!pending_thread) {
        return false;
    }
    auto thread = std::move(pending_thread).value().publish();

    const auto budget = clock().duration_from_nanoseconds(1'000'000);
    const auto period = clock().duration_from_nanoseconds(10'000'000);
    const auto urgency = sched::Urgency::make(31);
    if (!budget || !period || !urgency) {
        libk_assert(thread.retire());
        thread.reset();
        drain_reclaim();
        return false;
    }
    auto pending_context = pool<sched::Sc>().create(
        sched::Sc::Config{
            .budget = *budget,
            .period = *period,
            .urgency = *urgency,
        },
        clock().now());
    if (!pending_context) {
        libk_assert(thread.retire());
        thread.reset();
        drain_reclaim();
        return false;
    }
    auto context = std::move(pending_context).value().publish();

    auto admitted = kernel_domain_.get().admit(
        context.get(), runtime.local.descriptor->logical_id());
    auto target = thread.clone();
    if (!admitted || !target
        || !context->bind(
            target ? std::move(target).value()
                   : object::ref<Thread>{})) {
        if (context->admitted()) {
            libk_assert(kernel_domain_.get().unadmit(context.get()));
        }
        libk_assert(context.retire());
        context.reset();
        libk_assert(thread.retire());
        thread.reset();
        drain_reclaim();
        return false;
    }

    libk_assert(runtime.dispatcher().make_ready(context.get()));
    cleanup_thread_ = std::move(thread);
    cleanup_context_ = std::move(context);
    cleanup_notify_ = notifier::bind<&KernelState::wake_cleanup>(*this);
    io_work_.bind_notifier(io::Executor::Notifier::bind<&KernelState::wake_cleanup>(*this));
    vspace_work_.bind_notifier(
        mm::SpaceWork::Notifier::bind<
            &KernelState::wake_cleanup>(*this));
    grants().bind_work_notifier(
        cap::GrantGraph::WorkNotifier::bind<
            &KernelState::wake_cleanup>(*this));

    return true;
}

[[noreturn]] void KernelState::cleanup_entry(void* argument) noexcept {
    auto& kernel = *static_cast<KernelState*>(argument);
    for (;;) {
        const u64 admitted = kernel.cleanup_enqueues_.load<
            libk::MemoryOrder::Acquire>();
        CpuLocal& cpu = current_cpu();
        libk_assert(cpu.runtime().owner_registry != nullptr);
        static_cast<void>(kernel.drain_reclaim());
        const auto grant = kernel.grants().service(8);
        const auto vspace = kernel.vspace_work_.run(
            mm::VmCtx{
                .cpus = cpu.runtime().owner_registry,
                .local = cpu.descriptor->logical_id(),
            },
            8);
        const bool io_more = kernel.io_work_.run(8);
        if (grant.more || vspace.more
            || io_more
            || !kernel.close_cleanup_work(admitted)) {

            sched::yield();
        } else {

            sched::block();
        }
    }
}

auto KernelState::close_cleanup_work(u64 admitted) noexcept -> bool {
    // A later enqueue retains scheduler wake credit before this thread blocks.
    return cleanup_enqueues_.load<libk::MemoryOrder::Acquire>() == admitted;
}

void KernelState::wake_cleanup() noexcept {
    static_cast<void>(cleanup_enqueues_.fetch_add<libk::MemoryOrder::AcqRel>(1));
    libk_assert(cleanup_context_);
    auto& sc = cleanup_context_.get();
    const auto* target = cpus().descriptor(sc.home_cpu());
    libk_assert(target);
    // Before first dispatch the Ready cleanup executor already owns the work.
    if (target->state() == CpuState::Online) libk_assert(sched::wake(cpus(), sc));
}

void KernelState::release_scheduler_objects() noexcept {
    if (cleanup_context_) {
        if (cleanup_context_->bound()) {
            libk_assert(cleanup_context_->unbind());
        }
        if (cleanup_context_->admitted()) {
            libk_assert(kernel_domain_.get().unadmit(cleanup_context_.get()));
        }
        libk_assert(cleanup_context_.retire());
        cleanup_context_.reset();
    }
    if (cleanup_thread_) {
        libk_assert(cleanup_thread_.retire());
        cleanup_thread_.reset();
    }
    if (kernel_domain_) {
        libk_assert(kernel_domain_.retire());
        kernel_domain_.reset();
    }
    if (objects_) {
        drain_reclaim();
    }
}

auto KernelState::initialize_objects() noexcept -> bool {
    if (objects_) {
        return false;
    }
    (void)objects_.emplace(pmm(), cleanup_notify_);
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

void KernelState::install_root(mm::PageTable&& root, usize stack_end) noexcept {
    libk_assert(!kernel_vspace_);
    (void)kernel_vspace_.emplace(pmm(), std::move(root), stack_end);
    arch::activate_root(kernel_vspace_->cpu_root());
}

auto KernelState::begin_cpus(
    CpuTopo summary) noexcept -> CpuBeginResult {
    return CpuRegistry::begin(
        cpus_,
        pmm(),
        summary);
}
