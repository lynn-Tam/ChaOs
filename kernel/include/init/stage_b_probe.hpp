#pragma once

#include <core/types.hpp>
#include <cpu/topology.hpp>

#ifndef MYOS_CONCURRENCY_PROBE
#define MYOS_CONCURRENCY_PROBE 0
#endif

#if MYOS_CONCURRENCY_PROBE == 13

namespace kernel {
class CpuRegistry;
class KernelState;
namespace init {
class RootTask;
namespace stage_b {

enum class Gate : u32 {
    None,
    CompletionReady,
    ShootdownFinal,
    GrantFinal,
    PoolFirstService,
    PoolReentrant,
    VSpaceQuiescent,
};

using Action = void (*)(void*) noexcept;

// Secondary harts execute one action at a time. The action may pause at a
// production linearization point until the boot hart has observed canonical
// state and released it.  The subject identity prevents an unrelated live
// object of the same kind from stealing the rendezvous gate.
void dispatch(
    Action action,
    void* argument,
    CpuId target,
    Gate gate,
    u64 subject) noexcept;
[[nodiscard]] auto pause(Gate gate, u64 subject) noexcept -> bool;
void release(Gate gate) noexcept;
void join() noexcept;
[[nodiscard]] auto reached(Gate gate) noexcept -> bool;
void mark(Gate gate, u64 subject) noexcept;
[[noreturn]] void worker() noexcept;

[[nodiscard]] auto run(
    KernelState& kernel,
    CpuRegistry& cpus,
    RootTask& root) noexcept -> bool;

} // namespace stage_b
} // namespace init
} // namespace kernel

#endif
