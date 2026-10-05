#pragma once

#include <expected>
#include <libk/noncopyable.hpp>
#include <libk/manual_lifetime.hpp>
#include <cap/graph.hpp>
#include <cpu/registry.hpp>
#include <mm/phys.hpp>
#include <mm/kspace.hpp>
#include <mm/pmm.hpp>
#include <object/pool.hpp>
#include <mm/mem.hpp>
#include <mm/vspace.hpp>
#include <mm/pager.hpp>
#include <kernel/task/objects.hpp>
#include <kernel/sched/objects.hpp>
#include <kernel/ipc/objects.hpp>
#include <cap/cspace.hpp>
#include <object/group.hpp>
#include <sched/domain.hpp>
#include <time/clock.hpp>
#include <io/objects.hpp>

class KernelState final : private libk::noncopyable_nonmovable {
    class ConstructionKey {
        friend class ::KernelState;
        constexpr ConstructionKey() noexcept = default;
    };

  public:
    using InitializationResult = std::expected<void, mm::PmmInitError>;
    using KSpaceInitResult = mm::KSpace::InitResult;
    using CpuBeginResult = CpuRegistry::BeginResult;

    [[nodiscard]] static auto initialize_in(libk::ManualLifetime<KernelState>& storage,
                                            mm::RegionList&& memory_map,
                                            mm::DirectMap::Layout direct_map) noexcept
        -> InitializationResult;
    explicit KernelState([[maybe_unused]] ConstructionKey key) noexcept {}

    ~KernelState() noexcept;

    [[nodiscard]] auto initialize_kernel_vspace() noexcept
        -> KSpaceInitResult;
    [[nodiscard]] auto initialize_objects() noexcept -> bool;
    [[nodiscard]] auto initialize_grants() noexcept -> bool;
    [[nodiscard]] auto initialize_clock(u64 ticks_per_second) noexcept -> bool;
    [[nodiscard]] auto begin_cpus(CpuTopo summary) noexcept -> CpuBeginResult;
    [[nodiscard]] auto initialize_kernel_domain(usize cpu_count) noexcept -> bool;
    [[nodiscard]] auto initialize_root_pool(resource::budget limit) noexcept -> bool;
    [[nodiscard]] auto start_cleanup(CpuRuntime& runtime) noexcept -> bool;

    [[nodiscard]] auto pmm(this auto& self) noexcept -> decltype(auto) { return (*self.pmm_); }

    [[nodiscard]] auto direct_map(this auto& self) noexcept -> decltype(auto) {
        return self.pmm().direct_map();
    }

    [[nodiscard]] auto kernel_vspace(this auto& self) noexcept -> decltype(auto) {
        return (*self.kernel_vspace_);
    }

    [[nodiscard]] auto cpus(this auto& self) noexcept -> decltype(auto) { return (*self.cpus_); }

    [[nodiscard]] auto clock(this auto& self) noexcept -> decltype(auto) { return (*self.clock_); }
    [[nodiscard]] auto io_work() noexcept -> io::Executor& { return io_work_; }
    [[nodiscard]] auto io() noexcept -> io::objects& { return *io_; }

    using notifier = libk::delegate<void() noexcept>;
    auto tasks() noexcept -> Tasks& { return *tasks_; }
    auto sched() noexcept -> sched::objects& { return *sched_; }
    auto ipc() noexcept -> ipc::objects& { return *ipc_; }
    auto cspaces() noexcept -> object::pool<cap::CSpace>& { return *cspaces_; }
    template <class T> auto pool() noexcept -> object::pool<T>& { return memory_objects_->get<T>(); }
    auto space_work() noexcept -> mm::SpaceWork& { return vspace_work_; }
    auto drain_reclaim() noexcept -> usize {
        const auto devices = io().drain();
        const auto memory = memory_objects_->drain();
        auto count = devices + memory;
        count += ipc().drain();
        count += cspaces().drain_reclaim();
        count += sched().drain();
        return count + tasks().drain();
    }
    [[nodiscard]] auto grants(this auto& self) noexcept -> decltype(auto) { return (*self.grants_); }
    [[nodiscard]] auto kernel_domain(this auto& self) noexcept -> decltype(auto) {
        return self.kernel_domain_.get();
    }
    [[nodiscard]] auto kernel_domain_ref() const noexcept { return kernel_domain_.erase(); }
    [[nodiscard]] auto root_pool(this auto& self) noexcept -> decltype(auto) { return self.root_pool_.get(); }
    [[nodiscard]] auto root_pool_ref() const noexcept { return root_pool_.erase(); }
    [[nodiscard]] auto clone_root_pool() const noexcept { return root_pool_.clone(); }

  private:
    [[noreturn]] static void cleanup_entry(void* argument) noexcept;
    void wake_cleanup() noexcept;
    [[nodiscard]] auto close_cleanup_work(u64 admitted) noexcept -> bool;
    void release_scheduler_objects() noexcept;

    libk::ManualLifetime<mm::Pmm> pmm_{};
    libk::ManualLifetime<mm::KSpace> kernel_vspace_{};
    libk::ManualLifetime<time::Clock> clock_{};
    libk::ManualLifetime<io::objects> io_{};
    io::Executor io_work_{};
    mm::SpaceWork vspace_work_{};
    notifier cleanup_notify_{};
    libk::ManualLifetime<Tasks> tasks_{};
    libk::ManualLifetime<sched::objects> sched_{};
    libk::ManualLifetime<ipc::objects> ipc_{};
    libk::ManualLifetime<object::pool<cap::CSpace>> cspaces_{};
    libk::ManualLifetime<object::store<mm::VSpace, mm::Mem, Pager>> memory_objects_{};
    libk::ManualLifetime<cap::GrantGraph> grants_{};
    libk::ManualLifetime<CpuRegistry> cpus_{};
    object::ref<sched::Domain> kernel_domain_{};
    object::ref<sched::Sc> cleanup_context_{};
    object::ref<Thread> cleanup_thread_{};
    object::ref<object::group> root_pool_{};
    libk::Atomic<u64> cleanup_enqueues_{};
};
