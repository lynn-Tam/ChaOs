#pragma once

#include <arch/cpu.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_stack_set.hpp>
#include <diag/panic.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <mm/translation.hpp>
#include <object/object_store.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>

namespace kernel {

class CpuRegistry;

// Stable owner for every resource whose lifetime is tied to one CPU runtime.
// Registry metadata only publishes a borrow to this object after construction.
struct CpuRuntime final : private libk::noncopyable_nonmovable {
    CpuRuntime() noexcept = default;
    ~CpuRuntime() noexcept {
        dispatcher_storage.reset();
        if (idle_thread) {
            KASSERT(idle_thread.retire());
            idle_thread.reset();
        }
        if (diagnostics != nullptr) {
            libk::destroy_at(diagnostics);
            diagnostics = nullptr;
        }
    }

    [[nodiscard]] auto idle() noexcept -> Thread& { return idle_thread.get(); }
    [[nodiscard]] auto idle() const noexcept -> const Thread& {
        return idle_thread.get();
    }
    [[nodiscard]] auto dispatcher() noexcept -> sched::CpuDispatcher& {
        return *dispatcher_storage;
    }
    [[nodiscard]] auto dispatcher() const noexcept
        -> const sched::CpuDispatcher& {
        return *dispatcher_storage;
    }

    CpuLocal local{};
    kernel::mm::ShootdownQueue shootdowns{};
    libk::optional<kernel::mm::TranslationView> initial_translation{};
    CpuStackSet stacks{};
    kernel::mm::OwnedPage diagnostics_page{};
    diag::CpuDiagnostics* diagnostics{};
    object::ObjectStore::ThreadHold idle_thread{};
    libk::ManualLifetime<sched::CpuDispatcher> dispatcher_storage{};
    arch::CpuStartContext start_context{};
    CpuRegistry* owner_registry{};
};

static_assert(sizeof(CpuRuntime) <= kernel::mm::page_size,
    "CpuRuntime must remain allocatable by the page-bounded meta arena");

} // namespace kernel
