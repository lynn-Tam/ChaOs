#pragma once

#include <boot/info.hpp>
#include <boot/bundle.hpp>
#include <expected>
#include <libk/inplace_vector.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <mm/pmm.hpp>
#include <object/pool.hpp>
#include <mm/mem.hpp>
#include <object/group.hpp>
#include <uapi/bootstrap.h>

class RootTask;

class KernelState;
class CpuRegistry;
struct CpuRuntime;
class Completion;
class Wait;

enum class RootTaskError : u8 {
    InvalidModule,
    InvalidBundle,
    Ownership,
    OutOfMemory,
    InvalidState,
    MappingFailed,
    CapabilityFailed,
    SchedulingFailed,
};

// Owns the boot package and, later, the one root user deployment assembled
// from it. It is separate from KernelState because boot policy is not a
// permanent kernel service or a general process loader.
class RootTask final : private libk::noncopyable_nonmovable {
    class ConstructionKey {
        friend class RootTask;
        constexpr ConstructionKey() noexcept = default;
    };

public:
    [[nodiscard]] static auto initialize_in(
        libk::ManualLifetime<RootTask>& storage,
        object::pool<mm::Mem>& memory,
        mm::Pmm& pmm,
        object::ref<object::group>&& pool,
        BootModule module,
        mm::BootPages&& reservation) noexcept
        -> std::expected<void, RootTaskError>;

    RootTask(
        [[maybe_unused]] ConstructionKey key,
        mm::Pmm& pmm,
        BootModule module) noexcept
        : pmm_(&pmm), module_(module) {}

    [[nodiscard]] auto bundle() const noexcept
        -> std::expected<BootBundle, BundleError>;
    [[nodiscard]] auto start(
        KernelState& kernel,
        CpuRuntime& runtime) noexcept
        -> std::expected<void, RootTaskError>;
private:
    [[nodiscard]] auto load_segments(KernelState& kernel,
        const BootBundle& package, CpuId cpu) noexcept
        -> std::expected<void, RootTaskError>;
    [[nodiscard]] auto create_thread(KernelState& kernel, usize entry) noexcept
        -> std::expected<void, RootTaskError>;
    [[nodiscard]] auto reserve(resource::budget charge) noexcept
        -> std::expected<resource::Reservation, RootTaskError>;
    [[nodiscard]] auto prepare_bootstrap(KernelState& kernel) noexcept
        -> std::expected<void, RootTaskError>;
    void rollback(KernelState& kernel) noexcept;

    mm::Pmm* pmm_{};
    BootModule module_{};
    object::ref<object::group> pool_{};
    object::ref<mm::Mem> package_{};
    libk::InplaceVector<
        object::ref<mm::Mem>,
        max_boot_segments> segments_{};
    object::ref<mm::Mem> stack_{};
    object::ref<mm::Mem> info_{};
    object::ref<mm::Mem> ipc_{};
    object::ref<mm::VSpace> vspace_{};
    object::ref<cap::CSpace> cspace_{};
    object::ref<Thread> thread_{};
    object::ref<sched::Sc> context_{};
    // Platform-owned UART resources are retained here and delegated to the
    // user UART service through explicit bootstrap caps.
    object::ref<mm::Mem> uart_memory_{};
    object::ref<irq::Irq> uart_irq_{};
    bool started_{};
};

