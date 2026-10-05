// kernel/cpu/local.hpp
// Owns the single CPU-local root object seen through layered kernel/arch views.

#pragma once

#include <arch/cpu.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <cpu/types.hpp>
#include <type_traits>
#include <mm/tlb.hpp>
#include <stddef.h>

class Env;

class CpuDescriptor;
struct CpuRuntime;
class Thread;
struct CpuLocal;

namespace sched {
class Dispatcher;
}
namespace cap {
class CSpace;
}

namespace mm {
class KSpace;
class VSpace;
}

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
        current_ = nullptr;
        dispatcher_ = nullptr;
        active_tlb_ = nullptr;
        active_root_ = {};
        arch::initialize_cpu_entry(arch_state, this);
    }

    [[nodiscard]] auto current_thread() noexcept -> Thread* {
        return current_;
    }
    [[nodiscard]] auto current_thread() const noexcept -> const Thread* {
        return current_;
    }
    [[nodiscard]] auto dispatcher() noexcept -> sched::Dispatcher* {
        return dispatcher_;
    }
    [[nodiscard]] auto dispatcher() const noexcept
        -> const sched::Dispatcher* {
        return dispatcher_;
    }
    [[nodiscard]] auto runtime() noexcept -> CpuRuntime& {
        libk_assert(runtime_ != nullptr);
        return *runtime_;
    }
    [[nodiscard]] auto active_root() const noexcept
        -> const mm::Tlb* {
        return active_tlb_;
    }
    [[nodiscard]] auto kernel_vspace() const noexcept -> mm::KSpace*;
    [[nodiscard]] auto vspace() const noexcept -> mm::VSpace*;
    [[nodiscard]] auto cspace() const noexcept -> cap::CSpace*;

    // Dispatcher-owned runtime caches. Other CPUs observe execution through
    // explicit snapshots/events, never by mutating these fields.
    usize locks{};
    Thread* current_{};
    sched::Dispatcher* dispatcher_{};
    CpuRuntime* runtime_{};
    mm::Tlb* active_tlb_{};
    usize active_root_{};
};

static_assert(std::is_standard_layout_v<CpuLocal>);
static_assert(std::is_trivially_destructible_v<CpuLocal>);
static_assert(offsetof(CpuLocal, arch_state) == 0);
