#include <core/kernel_state.hpp>
#include <arch/address_layout.hpp>
#include <arch/interrupt.hpp>
#include <cap/authority.hpp>
#include <cap/rights.hpp>
#include <cpu/cpu_runtime.hpp>
#include <libk/span.hpp>
#include <libk/utility.hpp>
#include <mm/address_region.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/memory_object.hpp>
#include <mm/vspace.hpp>
#include <platform/memory_layout.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>

#if MYOS_INITIAL_USER_PROOF
extern "C" {
extern char user_image_start[];
extern char user_image_end[];
extern char initial_user_entry[];
extern char initial_user_peer[];
}

namespace {

constexpr kernel::mm::VirtAddr first_user_code{0x0000'0000'0020'0000ULL};
constexpr kernel::mm::VirtAddr first_user_stack{0x0000'0000'0040'0000ULL};
constexpr kernel::mm::VirtAddr user_peer_stack{0x0000'0000'0050'0000ULL};

} // namespace
#endif

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

    if(result){
        return libk::expected();
    }

    const kernel::mm::PmmInitError error = result.error();
    storage.reset();
    return libk::unexpected(error);
}

KernelState::~KernelState() noexcept{
    if (objects_) {
        objects().unbind_reclaim_notifier();
    }
    vspace_work_.unbind_notifier();
    cpus_.reset();
#if MYOS_INITIAL_USER_PROOF
    release_first_user();
#endif
    release_scheduler_objects();
    grants_.reset();
    objects_.reset();
    clock_.reset();
    kernel_vspace_.reset();
    pmm_.reset();
    direct_map_.reset();
}

#if MYOS_INITIAL_USER_PROOF
auto KernelState::start_first_user(kernel::CpuRuntime& runtime) noexcept
    -> bool {
    if (!kernel_domain_ || first_user_thread_ || first_user_context_
        || user_peer_thread_ || user_peer_context_
        || first_user_vspace_ || first_user_cspace_ || first_user_code_
        || first_user_stack_ || user_peer_stack_
        || arch::interrupts_enabled()) {
        return false;
    }
    const kernel::CpuId cpu = runtime.local.descriptor->logical_id();
    kernel::CpuRuntime* peer_runtime{};
    kernel::CpuId peer_cpu{};
    for (usize index = 0; index < cpus().count(); ++index) {
        const kernel::CpuId candidate{index};
        if (candidate == cpu) {
            continue;
        }
        const kernel::CpuDescriptor* const descriptor =
            cpus().descriptor(candidate);
        kernel::CpuRuntime* const candidate_runtime = cpus().runtime(candidate);
        if (descriptor != nullptr && candidate_runtime != nullptr
            && descriptor->state() == kernel::CpuState::Prepared) {
            peer_runtime = candidate_runtime;
            peer_cpu = candidate;
            break;
        }
    }
    const usize linked_start = reinterpret_cast<usize>(user_image_start);
    const usize linked_end = reinterpret_cast<usize>(user_image_end);
    const usize linked_entry = reinterpret_cast<usize>(initial_user_entry);
    const usize linked_peer = reinterpret_cast<usize>(initial_user_peer);
    if (linked_end <= linked_start || linked_entry < linked_start
        || linked_entry >= linked_end
        || linked_peer < linked_start || linked_peer >= linked_end
        || (linked_start & (kernel::mm::page_size - 1)) != 0
        || (linked_end - linked_start) != kernel::mm::page_size) {
        return false;
    }
    const auto physical =
        platform::memory::linked_physical(kernel::mm::VirtAddr{linked_start});
    const auto image_pages = physical
        ? kernel::mm::PageRange::from_aligned_bytes(
              *physical, linked_end - linked_start)
        : libk::nullopt;
    if (!image_pages) {
        return false;
    }
    const kernel::mm::MemoryExtent image_extent{
        .object = kernel::mm::ObjectRange{0, image_pages->page_count()},
        .physical = *image_pages,
        .access = kernel::mm::AccessMask::of(kernel::mm::Access::Read, kernel::mm::Access::Execute),
        .type = kernel::mm::MemoryType::Normal,
    };
    auto code = objects().create_boot_image(
        linked_end - linked_start,
        libk::Span<const kernel::mm::MemoryExtent>{&image_extent, 1},
        kernel::mm::BootOwnership::Borrowed);
    if (!code) {
        return false;
    }
    first_user_code_ = libk::move(code).value().publish();

    auto stack = objects().create_anonymous(
        kernel::mm::page_size,
        kernel::mm::AnonymousConfig{
            .access = kernel::mm::AccessMask::of(kernel::mm::Access::Read, kernel::mm::Access::Write),
            .eager = true,
        });
    if (!stack) {
        release_first_user();
        return false;
    }
    first_user_stack_ = libk::move(stack).value().publish();
    if (peer_runtime != nullptr) {
        auto peer_stack = objects().create_anonymous(
            kernel::mm::page_size,
            kernel::mm::AnonymousConfig{
                .access = kernel::mm::AccessMask::of(
                    kernel::mm::Access::Read, kernel::mm::Access::Write),
                .eager = true,
            });
        if (!peer_stack) {
            release_first_user();
            return false;
        }
        user_peer_stack_ = libk::move(peer_stack).value().publish();
    }
    auto space = objects().create_vspace(kernel_vspace());
    if (!space) {
        release_first_user();
        return false;
    }
    first_user_vspace_ = libk::move(space).value().publish();
    auto cspace = objects().create_cspace();
    if (!cspace) {
        release_first_user();
        return false;
    }
    first_user_cspace_ = libk::move(cspace).value().publish();

    const kernel::mm::VmContext vm{.local = cpu};
    auto code_ref = first_user_code_.ref();
    auto stack_ref = first_user_stack_.ref();
    if (!code_ref || !stack_ref) {
        release_first_user();
        return false;
    }
    const kernel::mm::MemoryTypes normal = kernel::mm::MemoryTypes::of(kernel::mm::MemoryType::Normal);
    const kernel::mm::AccessMask rx = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Execute);
    const kernel::mm::AccessMask rw = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    auto mapped_code = first_user_vspace_->map_kernel(
        vm,
        first_user_vspace_->root_key(),
        kernel::mm::MapRequest{
            kernel::mm::VirtRange{first_user_code, kernel::mm::page_size},
            kernel::mm::ObjectRange{0, 1},
            rx},
        libk::move(code_ref).value(),
        first_user_code_.get(),
        kernel::cap::MemoryAuthority{
            kernel::mm::ObjectRange{0, 1}, rx, normal});
    auto mapped_stack = first_user_vspace_->map_kernel(
        vm,
        first_user_vspace_->root_key(),
        kernel::mm::MapRequest{
            kernel::mm::VirtRange{first_user_stack, kernel::mm::page_size},
            kernel::mm::ObjectRange{0, 1},
            rw},
        libk::move(stack_ref).value(),
        first_user_stack_.get(),
        kernel::cap::MemoryAuthority{
            kernel::mm::ObjectRange{0, 1}, rw, normal});
    if (!mapped_code || !mapped_stack) {
        release_first_user();
        return false;
    }
    if (peer_runtime != nullptr) {
        auto peer_stack_ref = user_peer_stack_.ref();
        if (!peer_stack_ref) {
            release_first_user();
            return false;
        }
        const auto mapped_peer_stack = first_user_vspace_->map_kernel(
            vm,
            first_user_vspace_->root_key(),
            kernel::mm::MapRequest{
                kernel::mm::VirtRange{user_peer_stack, kernel::mm::page_size},
                kernel::mm::ObjectRange{0, 1},
                rw},
            libk::move(peer_stack_ref).value(),
            user_peer_stack_.get(),
            kernel::cap::MemoryAuthority{
                kernel::mm::ObjectRange{0, 1}, rw, normal});
        if (!mapped_peer_stack) {
            release_first_user();
            return false;
        }
    }

    const kernel::cap::Rights vspace_rights = kernel::cap::Rights::of(
        kernel::cap::Right::Duplicate,
        kernel::cap::Right::Delegate,
        kernel::cap::Right::Reserve,
        kernel::cap::Right::CreateRegion,
        kernel::cap::Right::Map,
        kernel::cap::Right::Unmap,
        kernel::cap::Right::Protect,
        kernel::cap::Right::Destroy,
        kernel::cap::Right::Inspect,
        kernel::cap::Right::Manage,
        kernel::cap::Right::Revoke);
    const kernel::cap::VSpaceAuthority root_authority{
        .region = first_user_vspace_->root_key(),
        .range = kernel::mm::VirtRange{
            kernel::mm::VirtAddr{arch::layout::low_guard_end},
            arch::layout::user_end - arch::layout::low_guard_end},
        .access = kernel::mm::AccessMask::of(
            kernel::mm::Access::Read, kernel::mm::Access::Write, kernel::mm::Access::Execute),
        .types = normal,
    };
    auto vspace_ref = first_user_vspace_.ref();
    auto vspace_grant = vspace_ref
        ? grants().create_root(
              libk::move(vspace_ref).value(),
              kernel::cap::GrantCeiling{vspace_rights, root_authority})
        : libk::Expected<kernel::cap::GrantRef, kernel::cap::GrantError>{
              libk::unexpected(kernel::cap::GrantError::InvalidKey)};
    auto vspace_cap = vspace_grant
        ? first_user_cspace_->insert(
              libk::move(vspace_grant).value(),
              kernel::cap::CapView{vspace_rights, root_authority})
        : libk::Expected<kernel::cap::CapHandle, kernel::cap::CSpaceError>{
              libk::unexpected(kernel::cap::CSpaceError::GrantUnavailable)};
    if (!vspace_cap) {
        release_first_user();
        return false;
    }

    const kernel::cap::Rights memory_rights = kernel::cap::Rights::of(
        kernel::cap::Right::Duplicate,
        kernel::cap::Right::Delegate,
        kernel::cap::Right::Map,
        kernel::cap::Right::Inspect,
        kernel::cap::Right::Revoke);
    const kernel::mm::MemoryObject* memories[] = {
        &first_user_code_.get(), &first_user_stack_.get()};
    const kernel::mm::AccessMask memory_access[] = {rx, rw};
    kernel::cap::CapHandle memory_caps[2]{};
    for (usize index = 0; index < 2; ++index) {
        auto reference = index == 0
            ? first_user_code_.ref()
            : first_user_stack_.ref();
        if (!reference) {
            release_first_user();
            return false;
        }
        const kernel::cap::MemoryAuthority authority{
            .range = kernel::mm::ObjectRange{0, memories[index]->page_count()},
            .access = memory_access[index],
            .types = normal,
        };
        auto grant = grants().create_root(
            libk::move(reference).value(),
            kernel::cap::GrantCeiling{memory_rights, authority});
        auto installed = grant
            ? first_user_cspace_->insert(
                  libk::move(grant).value(),
                  kernel::cap::CapView{memory_rights, authority})
            : libk::Expected<
                  kernel::cap::CapHandle,
                  kernel::cap::CSpaceError>{
                  libk::unexpected(
                      kernel::cap::CSpaceError::GrantUnavailable)};
        if (!installed) {
            release_first_user();
            return false;
        }
        memory_caps[index] = installed.value();
    }

    auto home = kernel::KernelStack::create(kernel_vspace());
    if (!home) {
        release_first_user();
        return false;
    }
    {
        auto execution_vspace = first_user_vspace_.ref();
        auto execution_cspace = first_user_cspace_.ref();
        if (execution_vspace && execution_cspace) {
            auto execution = kernel::ExecutionBinding::user(
                libk::move(execution_vspace).value(),
                libk::move(execution_cspace).value());
            if (execution) {
                auto pending_thread = objects().create_thread(
                    libk::move(home).value(),
                    libk::move(execution).value(),
                    kernel::Thread::UserStart{
                        .entry = kernel::mm::VirtAddr{
                            first_user_code.raw()
                                + linked_entry - linked_start},
                        .stack = kernel::mm::VirtAddr{
                            first_user_stack.raw() + kernel::mm::page_size},
                        .arguments = {
                            vspace_cap.value().raw(),
                            first_user_stack.raw(),
                            memory_caps[1].raw(),
                            peer_runtime != nullptr},
                    });
                if (pending_thread) {
                    first_user_thread_ =
                        libk::move(pending_thread).value().publish();
                }
            }
        }
    }
    if (!first_user_thread_) {
        release_first_user();
        return false;
    }
    if (peer_runtime != nullptr) {
        auto peer_home = kernel::KernelStack::create(kernel_vspace());
        if (peer_home) {
            auto execution_vspace = first_user_vspace_.ref();
            auto execution_cspace = first_user_cspace_.ref();
            if (execution_vspace && execution_cspace) {
                auto execution = kernel::ExecutionBinding::user(
                    libk::move(execution_vspace).value(),
                    libk::move(execution_cspace).value());
                if (execution) {
                    auto pending_peer = objects().create_thread(
                        libk::move(peer_home).value(),
                        libk::move(execution).value(),
                        kernel::Thread::UserStart{
                            .entry = kernel::mm::VirtAddr{
                                first_user_code.raw()
                                    + linked_peer - linked_start},
                            .stack = kernel::mm::VirtAddr{
                                user_peer_stack.raw() + kernel::mm::page_size},
                            .arguments = {first_user_stack.raw()},
                        });
                    if (pending_peer) {
                        user_peer_thread_ =
                            libk::move(pending_peer).value().publish();
                    }
                }
            }
        }
        if (!user_peer_thread_) {
            release_first_user();
            return false;
        }
    }

    const auto budget = clock().duration_from_nanoseconds(2'000'000);
    const auto period = clock().duration_from_nanoseconds(10'000'000);
    const auto urgency = kernel::sched::Urgency::make(20);
    if (!budget || !period || !urgency) {
        release_first_user();
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
        release_first_user();
        return false;
    }
    first_user_context_ = libk::move(pending_context).value().publish();
    bool first_bound{};
    {
        auto target_thread = first_user_thread_.clone();
        const auto admitted =
            kernel_domain_.get().admit(first_user_context_.get(), cpu);
        first_bound = admitted && target_thread
            && first_user_context_->bind(
                libk::move(target_thread).value(), cpu);
    }
    if (!first_bound) {
        release_first_user();
        return false;
    }

    if (peer_runtime != nullptr) {
        // The peer's long budget keeps it running throughout this bounded
        // proof while preserving the domain's per-CPU reserved capacity.
        const auto peer_budget =
            clock().duration_from_nanoseconds(800'000'000);
        const auto peer_period =
            clock().duration_from_nanoseconds(1'000'000'000);
        if (!peer_budget || !peer_period) {
            release_first_user();
            return false;
        }
        auto pending_peer_context = objects().create_context(
            kernel::sched::SchedulingContext::Config{
                .budget = *peer_budget,
                .period = *peer_period,
                .urgency = *urgency,
            },
            clock().now());
        if (!pending_peer_context) {
            release_first_user();
            return false;
        }
        user_peer_context_ =
            libk::move(pending_peer_context).value().publish();
        bool peer_bound{};
        {
            auto peer_target = user_peer_thread_.clone();
            const auto peer_admitted =
                kernel_domain_.get().admit(
                    user_peer_context_.get(), peer_cpu);
            peer_bound = peer_admitted && peer_target
                && user_peer_context_->bind(
                    libk::move(peer_target).value(), peer_cpu);
        }
        if (!peer_bound) {
            release_first_user();
            return false;
        }
        KASSERT(peer_runtime->dispatcher().make_ready(
            *user_peer_context_->binding()));
    }
    KASSERT(runtime.dispatcher().make_ready(
        *first_user_context_->binding()));
    return true;
}
#endif

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
                   : kernel::object::ObjectStore::ThreadHold{},
            runtime.local.descriptor->logical_id())) {
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

    return true;
}

[[noreturn]] void KernelState::reclaimer_entry(void* argument) noexcept {
    auto& kernel = *static_cast<KernelState*>(argument);
    for (;;) {
        kernel.objects().drain_reclaim();
        kernel::CpuLocal& cpu = kernel::current_cpu();
        KASSERT(cpu.runtime().owner_registry != nullptr);
        const bool more = kernel.vspace_work_.run(
            kernel::mm::VmContext{
                .cpus = cpu.runtime().owner_registry,
                .local = cpu.descriptor->logical_id(),
            },
            8);
        if (more) {
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

#if MYOS_INITIAL_USER_PROOF
void KernelState::release_first_user() noexcept {
    if (!objects_) {
        return;
    }
    if (user_peer_context_) {
        if (user_peer_context_->binding() != nullptr) {
            KASSERT(user_peer_context_->unbind());
        }
        if (user_peer_context_->admitted()) {
            KASSERT(kernel_domain_.get().unadmit(user_peer_context_.get()));
        }
        KASSERT(user_peer_context_.retire());
        user_peer_context_.reset();
    }
    if (first_user_context_) {
        if (first_user_context_->binding() != nullptr) {
            KASSERT(first_user_context_->unbind());
        }
        if (first_user_context_->admitted()) {
            KASSERT(kernel_domain_.get().unadmit(first_user_context_.get()));
        }
        KASSERT(first_user_context_.retire());
        first_user_context_.reset();
    }
    if (user_peer_thread_) {
        KASSERT(user_peer_thread_.retire());
        user_peer_thread_.reset();
    }
    if (first_user_thread_) {
        KASSERT(first_user_thread_.retire());
        first_user_thread_.reset();
    }
    // Destroying Thread drops the ExecutionBinding relations before either
    // effective root is asked to retire.
    objects().drain_reclaim();

    if (first_user_cspace_) {
        KASSERT(first_user_cspace_.retire());
        first_user_cspace_.reset();
    }
    if (first_user_vspace_) {
        KASSERT(first_user_vspace_.retire());
        first_user_vspace_.reset();
    }
    if (first_user_code_) {
        KASSERT(first_user_code_.retire());
        first_user_code_.reset();
    }
    if (first_user_stack_) {
        KASSERT(first_user_stack_.retire());
        first_user_stack_.reset();
    }
    if (user_peer_stack_) {
        KASSERT(user_peer_stack_.retire());
        user_peer_stack_.reset();
    }
    objects().drain_reclaim();
}
#endif

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
