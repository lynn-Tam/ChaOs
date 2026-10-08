#pragma once

#include <cpu.hpp>
#include <mm/kspace.hpp>
#include <object/ref.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>
#include <libk/inplace_vector.hpp>
#include <optional>
#include <libk/noncopyable.hpp>

class Cpus;
namespace cap { class Graph; }
struct PanicSlot;
namespace trace { struct Ring; }

// Stable from preparation until shutdown, including a delayed firmware start.
struct Cpu final : private libk::noncopyable_nonmovable {
    Cpu(CpuId id, CpuHwId hw, Cpus& cpus, object::Objects& objects, cap::Graph& grants) noexcept
        : id(id), hw(hw), cpus(&cpus), objects(objects), grants(grants) {}
    ~Cpu() noexcept;
    auto online() const noexcept -> bool { return online_.load<libk::MemoryOrder::Acquire>(); }
    auto enter() noexcept -> usize;
    void request_stop() noexcept;
    void install(usize hart) noexcept;
    auto idle(this auto& self) noexcept -> decltype(auto) { return self.idle_thread.get(); }
    auto dispatcher(this auto& self) noexcept -> decltype(auto) { return *self.dispatcher_storage; }

    const CpuId id;
    const CpuHwId hw;
    Cpus* const cpus;
    object::Objects& objects;
    cap::Graph& grants;
    // Owner-CPU runtime state; other CPUs use explicit events/snapshots.
    arch::Entry entry{};
    Thread* current{};
    mm::Tlb* tlb{};
    usize root{}, locks{};
    libk::delegate<void() noexcept> ext_irq{};
    std::optional<mm::Stack> init_stack{}, emergency_stack{};
    mm::OwnedPage panic_page{}, trace_page{};
    PanicSlot* panic{};
    trace::Ring* log{};
    object::ref<Thread> idle_thread{};
    std::optional<sched::Dispatcher> dispatcher_storage{};
    arch::Start start{};

private:
    friend class Cpus;
    auto prepare(mm::KSpace& vm, time::Clock& clock, mm::Stack* boot_stack) noexcept -> bool;
    libk::Atomic<bool> online_{};
};
static_assert(sizeof(Cpu) <= mm::page_size);

// Only successfully prepared CPUs receive logical ids. Firmware failure after
// publication never releases their start data or stacks; a hart may enter late.
class Cpus final : private libk::noncopyable_nonmovable {
public:
    explicit Cpus(mm::Pmm& pmm) noexcept : pages_(pmm.group()) {}
    ~Cpus() noexcept;
    auto add(CpuHwId hw, object::Objects&, cap::Graph&, mm::KSpace&, time::Clock&,
             mm::Stack* boot_stack = nullptr) noexcept -> bool;
    void start() noexcept;
    auto count() const noexcept -> usize { return cpus_.size(); }
    static constexpr auto boot_id() noexcept -> CpuId { return CpuId{0}; }
    auto get(CpuId id) noexcept -> Cpu* {
        return id.raw < cpus_.size() ? cpus_[id.raw] : nullptr;
    }
    auto get(CpuId id) const noexcept -> const Cpu* { return const_cast<Cpus*>(this)->get(id); }
private:
    friend struct Cpu;

    // RMW publication chains entrants; the last count observes every online CPU.
    libk::Atomic<usize> online_{};
    mm::PageGroup pages_;
    libk::InplaceVector<Cpu*, MaxCpus> cpus_;
};

[[nodiscard]] auto current_cpu() noexcept -> Cpu&;
