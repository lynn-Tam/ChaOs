#pragma once

#include <expected>


#include <cpu/registry.hpp>
#include <libk/noncopyable.hpp>
#include <mm/tlb.hpp>
#include <task/thread.hpp>

namespace mm {
class KSpace;
class Pmm;
}

namespace mm { class Stack; }
class KernelState;

namespace object {
template<class> class pool;
}
namespace time {
class Clock;
}

// Phase-scoped composition authority for unpublished per-CPU runtimes.
// Registry state and final storage remain authoritative in CpuRegistry.
// The boot or future hotplug orchestrator serializes each provisioning phase.
class CpuSetup final : private libk::noncopyable_nonmovable {
public:
    enum class Error : u8 {
        MetadataAllocation,
        StackAllocation,
        ObjectAllocation,
        InvalidState,
    };

    using Result = std::expected<void, Error>;

    CpuSetup(
        CpuRegistry& registry,
        mm::Pmm& pmm,
        object::pool<Thread>& threads,
        time::Clock& clock,
        KernelState* kernel = nullptr) noexcept
        : registry_(registry),
          pmm_(pmm),
          threads_(threads),
          clock_(clock),
          kernel_(kernel) {}

    [[nodiscard]] auto prepare(
        CpuId id,
        mm::KSpace& vspace,
        Thread::Entry idle_entry) noexcept -> Result;

    // The boot CPU is already executing on this stack. Ownership transfers
    // only after every fallible provisioning step has succeeded.
    [[nodiscard]] auto prepare_boot(
        CpuId id,
        mm::KSpace& vspace,
        mm::Stack& init_stack,
        Thread::Entry idle_entry) noexcept -> Result;

private:
    [[nodiscard]] auto prepare_impl(
        CpuId id,
        mm::KSpace& vspace,
        mm::Stack* init_stack,
        Thread::Entry idle_entry) noexcept -> Result;

    CpuRegistry& registry_;
    mm::Pmm& pmm_;
    object::pool<Thread>& threads_;
    time::Clock& clock_;
    KernelState* kernel_{};
};
