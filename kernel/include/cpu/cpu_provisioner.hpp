#pragma once

#include <arch/page_table.hpp>
#include <cpu/cpu_registry.hpp>
#include <libk/noncopyable.hpp>
#include <mm/translation.hpp>
#include <thread/thread.hpp>

namespace kernel::mm {
class KernelVSpace;
class Pmm;
}

namespace kernel {

class KernelStack;
class KernelState;

namespace object {
class ObjectStore;
}
namespace time {
class Clock;
}

// Phase-scoped composition authority for unpublished per-CPU runtimes.
// Registry state and final storage remain authoritative in CpuRegistry.
// The boot or future hotplug orchestrator serializes each provisioning phase.
class CpuProvisioner final : private libk::noncopyable_nonmovable {
public:
    enum class Error : u8 {
        MetadataAllocation,
        StackAllocation,
        ObjectAllocation,
        InvalidState,
    };

    using Result = libk::Expected<void, Error>;

    CpuProvisioner(
        CpuRegistry& registry,
        kernel::mm::Pmm& pmm,
        object::ObjectStore& objects,
        time::Clock& clock,
        KernelState* kernel = nullptr) noexcept
        : registry_(registry),
          pmm_(pmm),
          objects_(objects),
          clock_(clock),
          kernel_(kernel) {}

    [[nodiscard]] auto prepare(
        CpuId id,
        kernel::mm::KernelVSpace& vspace,
        Thread::Entry idle_entry) noexcept -> Result;

    // The boot CPU is already executing on this stack. Ownership transfers
    // only after every fallible provisioning step has succeeded.
    [[nodiscard]] auto prepare_boot(
        CpuId id,
        kernel::mm::KernelVSpace& vspace,
        KernelStack& init_stack,
        Thread::Entry idle_entry) noexcept -> Result;

private:
    [[nodiscard]] auto prepare_impl(
        CpuId id,
        kernel::mm::KernelVSpace& vspace,
        KernelStack* init_stack,
        Thread::Entry idle_entry) noexcept -> Result;

    CpuRegistry& registry_;
    kernel::mm::Pmm& pmm_;
    object::ObjectStore& objects_;
    time::Clock& clock_;
    KernelState* kernel_{};
};

} // namespace kernel
