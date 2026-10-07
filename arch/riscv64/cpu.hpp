#pragma once

#include "cpu_abi.h"
#include <base/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <array>
#include <time/time.hpp>
#include <type_traits>
#include <stddef.h>

struct CpuLocal;
struct CpuRuntime;
struct PanicSlot;

namespace arch {
// Callee-saved state. The selected port defines the layout consumed by cpu.S.
struct Ctx {
    usize ra{}, sp{};
    std::array<usize, 12> s{};
    Ctx() = default;
    Ctx(usize top, void (*entry)(void*) noexcept, void* arg) noexcept;
};
static_assert(std::is_standard_layout_v<Ctx> && sizeof(Ctx) == CPU_CTX_SIZE);
static_assert(offsetof(Ctx, ra) == CPU_CTX_RA_OFF);
static_assert(offsetof(Ctx, sp) == CPU_CTX_SP_OFF);
static_assert(offsetof(Ctx, s) == CPU_CTX_S_OFF);
// Returns when a later switch selects outgoing. IRQs remain masked.
extern "C" void switch_ctx(Ctx& outgoing, const Ctx& incoming) noexcept;
extern "C" [[noreturn]] void enter_ctx(const Ctx&) noexcept;
struct StackRegs { usize pc{}, sp{}, fp{}, ra{}; };
[[nodiscard, gnu::always_inline]] inline StackRegs stack_regs() noexcept {
    StackRegs r{};
    asm volatile("mv %0, sp" : "=r"(r.sp));
    asm volatile("mv %0, s0" : "=r"(r.fp));
    asm volatile("mv %0, ra" : "=r"(r.ra));
    r.pc = r.ra;
    return r;
}
// Firmware output; allocation-free and callable before per-CPU setup.
void putchar(char) noexcept;
inline void io_fence() noexcept { asm volatile("fence iorw, iorw" ::: "memory"); }
// Immutable secondary input. The kernel publishes ready last with release;
// the physical entry acquires it before reading any remaining field.
struct Start {
    u32 ready{}, padding{};
    usize hart{}, root{}, stack{};
    CpuRuntime* runtime{};
    void (*entry)(CpuRuntime*, usize) noexcept {};
};
// Only scratch is used before a trusted stack exists. Full registers belong
// to TrapFrame. IRQs must be masked when changing the current stack.
struct Scratch {
    usize t0{}, t1{}, t2{}, sp{};
};
struct Entry {
    Scratch scratch{};
    CpuLocal* owner{};
    usize stack{}, depth{}, emergency_stack{};
    PanicSlot* panic{};
    usize emergency_depth{}, stop{};
    u64 tick{};
};
static_assert(std::is_standard_layout_v<Start> && std::is_standard_layout_v<Entry>);
static_assert(sizeof(Start) == CPU_START_SIZE && alignof(Start) == alignof(usize));
static_assert(sizeof(Entry) == CPU_ENTRY_BLOCK_SIZE && alignof(Entry) == alignof(usize));
static_assert(sizeof(Scratch) == CPU_ENTRY_SIZE);
static_assert(offsetof(Start, ready) == CPU_START_READY_OFF);
static_assert(offsetof(Start, hart) == CPU_START_HART_OFF);
static_assert(offsetof(Start, root) == CPU_START_ROOT_OFF);
static_assert(offsetof(Start, stack) == CPU_START_STACK_OFF);
static_assert(offsetof(Start, runtime) == CPU_START_RUNTIME_OFF);
static_assert(offsetof(Start, entry) == CPU_START_ENTRY_OFF);
static_assert(offsetof(Entry, owner) == CPU_ENTRY_OWNER_OFF);
static_assert(offsetof(Entry, stack) == CPU_ENTRY_STACK_OFF);
static_assert(offsetof(Entry, depth) == CPU_ENTRY_DEPTH_OFF);
static_assert(offsetof(Entry, emergency_stack) == CPU_ENTRY_EMERGENCY_STACK_OFF);
static_assert(offsetof(Entry, panic) == CPU_ENTRY_PANIC_OFF);
static_assert(offsetof(Entry, emergency_depth) == CPU_ENTRY_EMERGENCY_DEPTH_OFF);
static_assert(offsetof(Entry, stop) == CPU_ENTRY_STOP_OFF);
static_assert(offsetof(Entry, tick) == CPU_ENTRY_TICK_OFF);
static_assert(offsetof(Scratch, t0) == CPU_ENTRY_T0_OFF);
static_assert(offsetof(Scratch, t1) == CPU_ENTRY_T1_OFF);
static_assert(offsetof(Scratch, t2) == CPU_ENTRY_T2_OFF);
static_assert(offsetof(Scratch, sp) == CPU_ENTRY_SP_OFF);
static_assert(offsetof(Entry, scratch) == 0);
Entry* local() noexcept;
void install(Entry&) noexcept;
bool interrupts_enabled() noexcept;
bool disable_interrupts() noexcept;
void enable_interrupts() noexcept;
void restore_interrupts(bool enabled) noexcept;
void wait_for_interrupt() noexcept;
bool secondary_start_available() noexcept;
auto start_secondary(CpuHwId, usize entry, usize record) noexcept -> std::expected<void, isize>;
bool ipi_available() noexcept;
auto send_ipi(CpuHwId) noexcept -> std::expected<void, isize>;
void enable_ipi() noexcept;
void acknowledge_ipi() noexcept;
auto read_clock() noexcept -> time::Instant;
bool timer_available() noexcept;
auto program_timer(time::Instant) noexcept -> std::expected<void, isize>;
void mask_timer() noexcept;
// rv64gc instructions are 16 or 32 bits.
constexpr usize instruction_size(u16 first) noexcept { return (first & 3U) == 3U ? 4 : 2; }
void sync_instruction_stream() noexcept;
enum class HaltReason : u8 { Panic, PeerStop, Fatal };
enum class HaltAction : u8 { Shutdown, Reboot };
[[noreturn]] void halt_current_cpu(HaltReason) noexcept;
[[noreturn]] void halt_system(HaltAction, HaltReason) noexcept;
extern "C" [[noreturn]] void switch_stack(usize top, void* arg, void (*entry)(void*) noexcept) noexcept;
} // namespace arch
