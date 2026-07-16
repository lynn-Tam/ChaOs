// kernel/include/cpu/cpu_local.hpp
// Owns the single CPU-local root object seen through layered kernel/arch views.

#pragma once

#include <arch/cpu.hpp>
#include <core/debug.hpp>
#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/typetraits.hpp>
#include <mm/translation.hpp>
#include <stddef.h>

namespace kernel {

class ExecutionBinding;

class CpuDescriptor;
struct CpuRuntime;
class Thread;
struct CpuLocal;
namespace sched {
class CpuDispatcher;
}
namespace cap {
class CSpace;
}

}

namespace kernel::mm {
class KernelVSpace;
class VSpace;
}

namespace kernel {

[[nodiscard]] auto current_cpu() noexcept -> CpuLocal&;

struct CpuLocal final {
    CpuLocal() noexcept = default;
    CpuLocal(const CpuLocal&) = delete;
    auto operator=(const CpuLocal&) -> CpuLocal& = delete;
    CpuLocal(CpuLocal&&) = delete;
    auto operator=(CpuLocal&&) -> CpuLocal& = delete;

    arch::CpuEntryState arch_state{};
    const CpuDescriptor* descriptor{};

    void initialize(
        const CpuDescriptor& identity,
        CpuRuntime& runtime) noexcept {
        descriptor = &identity;
        runtime_ = &runtime;
        current_thread_ = nullptr;
        dispatcher_ = nullptr;
        active_translation_ = nullptr;
        active_root_ = {};
        observed_epoch_ = {};
        arch::initialize_cpu_entry(arch_state, this);
    }

    [[nodiscard]] auto current_thread() noexcept -> Thread* {
        return current_thread_;
    }
    [[nodiscard]] auto current_thread() const noexcept -> const Thread* {
        return current_thread_;
    }
    [[nodiscard]] auto dispatcher() noexcept -> sched::CpuDispatcher* {
        return dispatcher_;
    }
    [[nodiscard]] auto dispatcher() const noexcept
        -> const sched::CpuDispatcher* {
        return dispatcher_;
    }
    [[nodiscard]] auto runtime() noexcept -> CpuRuntime& {
        KASSERT(runtime_ != nullptr);
        return *runtime_;
    }
    [[nodiscard]] auto active_translation() const noexcept
        -> const kernel::mm::TranslationState* {
        return active_translation_;
    }
    [[nodiscard]] auto observed_epoch() const noexcept
        -> kernel::mm::TranslationEpoch {
        return observed_epoch_;
    }
    [[nodiscard]] auto kernel_vspace() const noexcept -> kernel::mm::KernelVSpace*;
    [[nodiscard]] auto vspace() const noexcept -> kernel::mm::VSpace*;
    [[nodiscard]] auto cspace() const noexcept -> cap::CSpace*;

    // Dispatcher-owned runtime caches. Other CPUs observe execution through
    // explicit snapshots/events, never by mutating these fields.
    Thread* current_thread_{};
    sched::CpuDispatcher* dispatcher_{};
    CpuRuntime* runtime_{};
    kernel::mm::TranslationState* active_translation_{};
    arch::RootToken active_root_{};
    kernel::mm::TranslationEpoch observed_epoch_{};
};

static_assert(libk::is_standard_layout_v<CpuLocal>);
static_assert(libk::is_trivially_destructible_v<CpuLocal>);
static_assert(offsetof(CpuLocal, arch_state) == 0);

} // namespace kernel
