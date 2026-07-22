#pragma once

#include <core/types.hpp>
#include <libk/sync/atomic.hpp>

#ifndef MYOS_LOCK_DIAG
#define MYOS_LOCK_DIAG 0
#endif
#ifndef MYOS_LOCK_PROBE
#define MYOS_LOCK_PROBE 0
#endif

namespace kernel {
namespace trap {
class Event;
}
}

namespace kernel::sync {

inline constexpr usize lock_diag_level = MYOS_LOCK_DIAG;
inline constexpr bool lock_verify = lock_diag_level >= 1;
inline constexpr bool lock_trace = lock_diag_level >= 2;
inline constexpr bool lock_profile = lock_diag_level >= 3;

enum class LockClass : u8 {
    Pmm,
    KernelStack,
    Shootdown,
    Translation,
    ObjectPool,
    NodePool,
    GrantGraph,
    GrantWork,
    CSpace,
    ResourcePool,
    VSpace,
    VSpaceWork,
    MemoryObject,
    BackingTree,
    BackingStorage,
    PhysicalAlias,
    ExecutionAuthority,
    SchedAuthority,
    SchedContext,
    SchedDomain,
    RemoteQueue,
    ThreadStop,
    Vproc,
    Wait,
    Endpoint,
    NotificationSource,
    Notification,
    Tunnel,
#if MYOS_LOCK_PROBE
    ProbeA,
    ProbeB,
#endif
    Count,
};

enum class SameClassPolicy : u8 {
    Forbidden,
    AddressAscending,
};

struct LockSite final {
    const char* file{};
    const char* function{};
    u32 line{};

    [[nodiscard]] static consteval auto current(
        const char* file = __builtin_FILE(),
        const char* function = __builtin_FUNCTION(),
        u32 line = __builtin_LINE()) noexcept -> LockSite {
        return LockSite{file, function, line};
    }
};

struct LockRef final {
    const void* address{};
    libk::Atomic<u64>* owner{};
    LockClass lock_class{LockClass::Pmm};
    SameClassPolicy same_class{SameClassPolicy::Forbidden};
};

struct LockCookie final {
    bool tracked{};
    bool owner_tracked{};
    u64 wait_started{};
    bool contended{};
};

struct IrqCookie final {
    usize cpu{};
    bool tracked{};
};

enum class LockEvent : u8 {
    Acquire,
    Release,
    Contended,
    IrqOff,
    IrqOn,
};

enum class ExecContext : u8 {
    EarlyBoot,
    KernelThread,
    UserSyscall,
    UserFault,
    TimerIrq,
    SoftwareIpi,
    ExternalIrq,
    TrapExit,
    Panic,
};

inline constexpr usize local_held_capacity = 12;
inline constexpr usize remote_held_capacity = local_held_capacity;
inline constexpr usize lock_event_capacity = 7;
inline constexpr usize wait_cycle_capacity = 8;
inline constexpr usize dep_cycle_capacity = 2;
inline constexpr usize context_capacity = 8;

struct HeldEntry final {
    LockRef lock{};
    LockSite site{};
    u64 acquired_at{};
};

struct LocalLockState final {
    HeldEntry held[local_held_capacity]{};
    ExecContext contexts[context_capacity]{ExecContext::EarlyBoot};
    u8 held_count{};
    u8 context_depth{1};
    u16 explicit_irq_depth{};
    u64 explicit_irq_start{};
    LockSite explicit_irq_site{};
};

// Every field in these records is atomic because a live peer or the panic CPU
// may read it while the owning CPU continues to mutate its local verifier.
struct RemoteHeld final {
    libk::Atomic<u64> sequence{};
    libk::Atomic<usize> address{};
    libk::Atomic<u32> lock_class{};
    libk::Atomic<u32> line{};
    libk::Atomic<u64> acquired_at{};
};

struct RemoteWait final {
    libk::Atomic<u64> sequence{};
    libk::Atomic<usize> address{};
    libk::Atomic<libk::Atomic<u64>*> owner{};
    libk::Atomic<u32> lock_class{};
    libk::Atomic<u32> line{};
    libk::Atomic<u64> generation{};
    libk::Atomic<bool> active{};
};

struct RemoteEvent final {
    libk::Atomic<u64> sequence{};
    libk::Atomic<u64> tick{};
    libk::Atomic<usize> address{};
    libk::Atomic<u32> data{};
};

struct WaitLink final {
    libk::Atomic<u64> sequence{};
    libk::Atomic<usize> lock{};
    libk::Atomic<u64> owner_word{};
    libk::Atomic<u32> cpu{};
};

struct DepLink final {
    libk::Atomic<u64> sequence{};
    libk::Atomic<const char*> from_file{};
    libk::Atomic<const char*> to_file{};
    libk::Atomic<u64> lines{};
    libk::Atomic<u32> classes{};
};

struct ClassStats final {
    libk::Atomic<u64> acquisitions{};
    libk::Atomic<u64> contentions{};
    libk::Atomic<u64> wait_ticks{};
    libk::Atomic<u64> hold_ticks{};
    libk::Atomic<u64> max_wait{};
    libk::Atomic<u64> max_hold{};
    libk::Atomic<u64> max_ticket_distance{};
    libk::Atomic<u64> context_mask{};
};

struct CpuLockTrace final {
    LocalLockState local{};
    RemoteWait waiting{};
    RemoteHeld held[remote_held_capacity]{};
    RemoteEvent events[lock_event_capacity]{};
    WaitLink cycle[wait_cycle_capacity]{};
    DepLink dep_cycle[dep_cycle_capacity]{};
    libk::Atomic<u64> event_head{};
    libk::Atomic<u64> wait_generation{};
    libk::Atomic<u64> cycle_size{};
    libk::Atomic<u64> dep_cycle_size{};
    libk::Atomic<u32> held_count{};
    libk::Atomic<u32> context{static_cast<u32>(ExecContext::EarlyBoot)};
    libk::Atomic<u32> degraded{};
    libk::Atomic<u32> hardware_irq_depth{};
    libk::Atomic<u64> hardware_irq_start{};
    libk::Atomic<u64> hardware_irq_max{};
    libk::Atomic<u64> explicit_irq_max{};
#if MYOS_LOCK_DIAG >= 3
    ClassStats stats[static_cast<usize>(LockClass::Count)]{};
#endif
};

[[nodiscard]] auto before_acquire(LockRef lock, LockSite site) noexcept
    -> LockCookie;
#if MYOS_LOCK_PROBE
[[nodiscard]] auto before_wait_probe(LockRef lock, LockSite site) noexcept
    -> LockCookie;
#endif
[[nodiscard]] auto after_acquire(
    LockRef lock, LockSite site, LockCookie cookie) noexcept -> LockCookie;
[[nodiscard]] auto before_try(LockRef lock, LockSite site) noexcept
    -> LockCookie;
[[nodiscard]] auto after_try(
    LockRef lock, LockSite site, LockCookie cookie) noexcept -> LockCookie;
void cancel_try(LockCookie cookie) noexcept;
void before_release(LockRef lock, LockSite site, LockCookie cookie) noexcept;
void on_spin(
    LockRef lock, LockSite site, u32 ticket, u32 serving, u32 polls) noexcept;

[[nodiscard]] auto irq_disabled(LockSite site) noexcept -> IrqCookie;
void irq_restoring(IrqCookie cookie) noexcept;
void trap_enter(const kernel::trap::Event& event, u64 entry_tick) noexcept;
void trap_exiting() noexcept;
void trap_exit(u64 exit_tick) noexcept;
void panic_enter() noexcept;
void assert_no_locks(LockSite site = LockSite::current()) noexcept;
void assert_held(LockRef lock, LockSite site = LockSite::current()) noexcept;
void dump_diagnostics() noexcept;
void run_probe(u32 probe) noexcept;

[[nodiscard]] auto lock_class_name(LockClass lock_class) noexcept
    -> const char*;

} // namespace kernel::sync
