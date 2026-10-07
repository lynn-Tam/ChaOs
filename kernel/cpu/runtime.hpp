#pragma once

#include <cpu.hpp>
#include <cpu/local.hpp>
#include <mm/kspace.hpp>
#include <libk/assert.hpp>
#include <libk/noncopyable.hpp>
#include <optional>
#include <mm/tlb.hpp>
#include <object/ref.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>

class CpuRegistry;
class KernelState;
struct PanicSlot;
namespace trace { struct Ring; }

// Stable owner for every resource whose lifetime is tied to one CPU runtime.
// Registry metadata only publishes a borrow to this object after construction.
struct CpuRuntime final : private libk::noncopyable_nonmovable {
    CpuRuntime() noexcept = default;
    ~CpuRuntime() noexcept;
    [[nodiscard]] auto init_log(mm::Pmm&, CpuId, CpuHwId, CpuRegistry&) noexcept -> bool;

    [[nodiscard]] auto idle() noexcept -> Thread& { return idle_thread.get(); }
    [[nodiscard]] auto idle() const noexcept -> const Thread& {
        return idle_thread.get();
    }
    [[nodiscard]] auto dispatcher() noexcept -> sched::Dispatcher& {
        return *dispatcher_storage;
    }
    [[nodiscard]] auto dispatcher() const noexcept
        -> const sched::Dispatcher& {
        return *dispatcher_storage;
    }

    CpuLocal local{};
    std::optional<mm::Root> initial_translation{};
    libk::ManualLifetime<mm::Stack> init_stack{}, irq_stack{}, emergency_stack{};
    mm::OwnedPage panic_page{}, trace_page{};
    PanicSlot* panic{};
    trace::Ring* log{};
    object::ref<Thread> idle_thread{};
    libk::ManualLifetime<sched::Dispatcher> dispatcher_storage{};
    arch::Start start{};
    CpuRegistry* owner_registry{};
    KernelState* kernel{};
};

static_assert(sizeof(CpuRuntime) <= mm::page_size,
    "CpuRuntime must remain allocatable by the page-bounded meta arena");
