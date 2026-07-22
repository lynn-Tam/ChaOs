#pragma once

#include <arch/trap.hpp>
#include <arch/diagnostics.hpp>
#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/sync/atomic.hpp>
#include <sync/trace.hpp>

namespace kernel {
class CpuRegistry;
}

namespace kernel::diag {

enum class PanicKind : u8 {
    Assertion,
    ExplicitFatal,
    UnhandledTrap,
    PeerStop,
};

enum class Facility : u8 {
    Core,
    Architecture,
    Trap,
    Scheduler,
    Object,
    Capability,
    Memory,
    Syscall,
    Synchronization,
};

struct EventId final {
    u32 raw{};
};

struct SourceLocation final {
    const char* file{};
    const char* function{};
    const char* expression{};
    u32 line{};
};

struct FatalEvent final {
    Facility facility{Facility::Core};
    EventId id{};
    usize arguments[6]{};
    u8 argument_count{};
};

struct PanicRequest final {
    PanicKind kind{PanicKind::ExplicitFatal};
    FatalEvent event{};
    SourceLocation source{};
    const arch::TrapContext* trap{};
};

enum class PanicSlotState : u32 {
    Empty,
    Capturing,
    Captured,
    Stopped,
};

struct PanicSlot final {
    libk::Atomic<PanicSlotState> state{PanicSlotState::Empty};
    CpuId cpu{};
    CpuHardwareId hardware{};
    PanicRequest request{};
    arch::CallSiteSnapshot call_site{};
    arch::TrapSnapshot trap{};
    CpuRegistry* registry{};
    usize current_thread{};
    usize active_root{};
    u64 observed_epoch{};
    usize trap_depth{};
    usize stack_base{};
    usize stack_top{};
    bool has_full_trap{};
    bool interrupts_enabled{};
};

struct CpuDiagnostics final {
    PanicSlot panic{};
#if MYOS_LOCK_DIAG >= 1
    sync::CpuLockTrace locks{};
#endif
};

static_assert(sizeof(CpuDiagnostics) <= 4096);

[[noreturn]] void panic(PanicRequest request) noexcept;
[[noreturn]] void assert_fail(
    const char* expression,
    const char* file,
    const char* function,
    u32 line) noexcept;
[[noreturn]] void fatal(
    FatalEvent event,
    SourceLocation source = {}) noexcept;
[[noreturn]] void panic_unhandled(
    FatalEvent event,
    const arch::TrapContext& trap) noexcept;

[[nodiscard]] auto stop_requested() noexcept -> bool;
[[noreturn]] void stop_peer(const arch::TrapContext& trap) noexcept;

} // namespace kernel::diag

#if defined(__GNUC__) || defined(__clang__)
#define KASSERT_FUNCTION_NAME __PRETTY_FUNCTION__
#else
#define KASSERT_FUNCTION_NAME __func__
#endif

#define KERNEL_SOURCE_LOCATION(expression_text) \
    ::kernel::diag::SourceLocation{ \
        __FILE__, KASSERT_FUNCTION_NAME, expression_text, \
        static_cast<u32>(__LINE__)}

#define KASSERT(expr) \
    do { \
        if (!(expr)) [[unlikely]] { \
            ::kernel::diag::assert_fail( \
                #expr, __FILE__, KASSERT_FUNCTION_NAME, \
                static_cast<u32>(__LINE__)); \
        } \
    } while (false)

#define KASSERT_EVENT(expr, event_value) \
    do { \
        if (!(expr)) [[unlikely]] { \
            ::kernel::diag::fatal( \
                (event_value), KERNEL_SOURCE_LOCATION(#expr)); \
        } \
    } while (false)

#define KPANIC(event_value) \
    ::kernel::diag::fatal( \
        (event_value), KERNEL_SOURCE_LOCATION(nullptr))

#if defined(MYOS_DEBUG_ASSERTS)
#define KDEBUG_ASSERT(expr) KASSERT(expr)
#else
#define KDEBUG_ASSERT(expr) do { static_cast<void>(sizeof(expr)); } while (false)
#endif
