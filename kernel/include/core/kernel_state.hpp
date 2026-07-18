#pragma once

#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/manual_lifetime.hpp>
#include <cap/grant_graph.hpp>
#include <cpu/cpu_registry.hpp>
#include <mm/direct_map.hpp>
#include <mm/kernel_vspace.hpp>
#include <mm/pmm.hpp>
#include <mm/vspace_work.hpp>
#include <object/object_store.hpp>
#include <resource/pool.hpp>
#include <sched/domain.hpp>
#include <time/clock.hpp>

namespace kernel {

class KernelState final : private libk::noncopyable_nonmovable {
    class ConstructionKey{
        friend class KernelState;
        constexpr ConstructionKey() noexcept = default;
    };

public:
    using InitializationResult =
        libk::Expected<void, kernel::mm::PmmInitError>;
    using KernelVSpaceInitResult = kernel::mm::KernelVSpace::InitResult;
    using CpuBeginResult = kernel::CpuRegistry::BeginResult;

    [[nodiscard]] static auto initialize_in(
        libk::ManualLifetime<KernelState>& storage,
        kernel::mm::RegionList&& memory_map,
        kernel::mm::DirectMapLayout direct_map) noexcept
        -> InitializationResult;
    explicit KernelState([[maybe_unused]] ConstructionKey key) noexcept {}

    ~KernelState() noexcept;

    [[nodiscard]] auto initialize_kernel_vspace() noexcept
        -> KernelVSpaceInitResult;
    [[nodiscard]] auto initialize_object_store() noexcept -> bool;
    [[nodiscard]] auto initialize_grants() noexcept -> bool;
    [[nodiscard]] auto initialize_clock(u64 ticks_per_second) noexcept
        -> bool;
    [[nodiscard]] auto begin_cpus(
        kernel::CpuTopologySummary summary) noexcept -> CpuBeginResult;
    [[nodiscard]] auto initialize_kernel_domain(usize cpu_count) noexcept
        -> bool;
    [[nodiscard]] auto initialize_root_pool(
        kernel::resource::Budget limit) noexcept -> bool;
    [[nodiscard]] auto start_reclaimer(kernel::CpuRuntime& runtime) noexcept
        -> bool;

    [[nodiscard]] auto pmm(this auto& self) noexcept
        -> decltype(auto){
        return (*self.pmm_);
    }

    [[nodiscard]] auto direct_map(this auto& self) noexcept
        -> decltype(auto) {
        return (*self.direct_map_);
    }

    [[nodiscard]] auto kernel_vspace(this auto& self) noexcept
        -> decltype(auto) {
        return (*self.kernel_vspace_);
    }

    [[nodiscard]] auto cpus(this auto& self) noexcept -> decltype(auto) {
        return (*self.cpus_);
    }

    [[nodiscard]] auto clock(this auto& self) noexcept -> decltype(auto) {
        return (*self.clock_);
    }

    [[nodiscard]] auto objects(this auto& self) noexcept -> decltype(auto) {
        return (*self.objects_);
    }
    [[nodiscard]] auto grants(this auto& self) noexcept -> decltype(auto) {
        return (*self.grants_);
    }
    [[nodiscard]] auto kernel_domain(this auto& self) noexcept
        -> decltype(auto) {
        return self.kernel_domain_.get();
    }
    [[nodiscard]] auto kernel_domain_ref() const noexcept {
        return kernel_domain_.ref();
    }
    [[nodiscard]] auto root_pool(this auto& self) noexcept
        -> decltype(auto) {
        return self.root_pool_.get();
    }
    [[nodiscard]] auto root_pool_ref() const noexcept {
        return root_pool_.ref();
    }
    [[nodiscard]] auto clone_root_pool() const noexcept {
        return root_pool_.clone();
    }
private:
    [[noreturn]] static void reclaimer_entry(void* argument) noexcept;
    void wake_reclaimer() noexcept;
    void release_scheduler_objects() noexcept;

    libk::ManualLifetime<kernel::mm::DirectMap> direct_map_{};
    libk::ManualLifetime<kernel::mm::Pmm> pmm_{};
    libk::ManualLifetime<kernel::mm::KernelVSpace> kernel_vspace_{};
    libk::ManualLifetime<kernel::time::Clock> clock_{};
    kernel::mm::VSpaceExecutor vspace_work_{};
    libk::ManualLifetime<kernel::object::ObjectStore> objects_{};
    libk::ManualLifetime<kernel::cap::GrantGraph> grants_{};
    libk::ManualLifetime<kernel::CpuRegistry> cpus_{};
    kernel::object::ObjectStore::SchedulingDomainHold kernel_domain_{};
    kernel::object::ObjectStore::SchedulingContextHold reclaimer_context_{};
    kernel::object::ObjectStore::ThreadHold reclaimer_thread_{};
    kernel::object::ObjectStore::ResourceHold root_pool_{};
};

} // namespace kernel
