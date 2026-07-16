#include <diag/panic.hpp>

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <arch/ipi.hpp>
#include <arch/time.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <libk/assert.hpp>
#include <libk/fmt.hpp>
#include <platform/console.hpp>
#include <platform/system.hpp>
#include <thread/thread.hpp>

namespace kernel::diag {
namespace {

extern "C" char kernel_text_start[];
extern "C" char kernel_text_end[];

enum class PanicPhase : u32 {
    Idle,
    Claimed,
    StoppingPeers,
    Dumping,
    Halted,
};

struct PanicCoordinator final {
    libk::Atomic<PanicPhase> phase{PanicPhase::Idle};
    libk::Atomic<u32> owner{0xffffffffU};
    libk::Atomic<u64> sequence{};
};

constinit PanicCoordinator coordinator{};

void raw_char(char character) noexcept {
    platform::console::write(character);
}

void raw_text(const char* text) noexcept {
    if (text == nullptr) {
        raw_text("<none>");
        return;
    }
    while (*text != '\0') {
        raw_char(*text++);
    }
}

void raw_decimal(u64 value) noexcept {
    char digits[20]{};
    usize count{};
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        raw_char(digits[--count]);
    }
}

void raw_hex(usize value) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    raw_text("0x");
    for (usize shift = sizeof(usize) * 8; shift != 0; shift -= 4) {
        raw_char(digits[(value >> (shift - 4)) & 0xfU]);
    }
}

class PanicSink final {
public:
    auto write(char character) noexcept -> bool {
        platform::console::write(character);
        return true;
    }

    auto write(const char* text, usize size) noexcept -> bool {
        platform::console::write(libk::StrView{text, size});
        return true;
    }
};

template<libk::fmt::fixed_string Format, typename... Args>
void panic_print(const Args&... arguments) noexcept {
    PanicSink sink{};
    const auto result = libk::fmt::format_to<Format>(sink, arguments...);
    if (!result) {
        raw_text("<panic format failure>");
    }
}

void raw_source(const SourceLocation& source) noexcept {
    if (source.file == nullptr) {
        return;
    }
    raw_text("site: ");
    raw_text(source.file);
    raw_char(':');
    raw_decimal(source.line);
    raw_char('\n');
}

[[noreturn]] void double_panic(usize cpu, usize pc, usize cause) noexcept {
    raw_text("\nDOUBLE PANIC cpu=");
    raw_decimal(cpu);
    raw_text(" pc=");
    raw_hex(pc);
    raw_text(" cause=");
    raw_hex(cause);
    raw_char('\n');
    platform::halt_current_cpu(platform::HaltReason::Fatal);
}

void capture_stack_bounds(
    PanicSlot& slot,
    CpuLocal& cpu,
    arch::UnwindSeed seed) noexcept {
    const usize sp = seed.sp;
    Thread* const thread = cpu.current_thread();
    if (thread != nullptr && sp >= thread->home_stack_base()
        && sp < thread->home_stack_top()) {
        slot.stack_base = thread->home_stack_base();
        slot.stack_top = thread->home_stack_top();
        return;
    }
    CpuRuntime& runtime = cpu.runtime();
    const KernelStack* stacks[] = {
        runtime.stacks.init ? &*runtime.stacks.init : nullptr,
        runtime.stacks.irq ? &*runtime.stacks.irq : nullptr,
        runtime.stacks.emergency ? &*runtime.stacks.emergency : nullptr,
    };
    for (const KernelStack* stack : stacks) {
        if (stack != nullptr && stack->contains(sp)) {
            slot.stack_base = stack->base();
            slot.stack_top = stack->top();
            return;
        }
    }
}

void capture(
    PanicSlot& slot,
    PanicRequest request,
    arch::CallSiteSnapshot call_site,
    bool interrupts_were_enabled) noexcept {
    PanicSlotState expected = PanicSlotState::Empty;
    if (!slot.state.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(
            expected, PanicSlotState::Capturing)) {
        const usize pc = request.trap != nullptr ? request.trap->pc() : 0;
        const usize cause = request.trap != nullptr
            ? request.trap->snapshot().cause
            : 0;
        double_panic(slot.cpu.raw, pc, cause);
    }

    slot.request = request;
    slot.request.trap = nullptr;
    slot.has_full_trap = request.trap != nullptr;
    if (request.trap != nullptr) {
        slot.trap = request.trap->snapshot();
    }
    slot.call_site = call_site;
    slot.interrupts_enabled = interrupts_were_enabled;

    void* const owner = arch::current_cpu_owner();
    KASSERT(owner != nullptr);
    auto& cpu = *static_cast<CpuLocal*>(owner);
    slot.registry = cpu.runtime().owner_registry;
    slot.current_thread = reinterpret_cast<usize>(cpu.current_thread());
    slot.active_root = cpu.active_root_ ? 1 : 0;
    slot.observed_epoch = cpu.observed_epoch_.raw;
    slot.trap_depth = arch::trap_depth();
    const arch::UnwindSeed seed = slot.has_full_trap
        ? arch::unwind_seed(slot.trap)
        : slot.call_site;
    capture_stack_bounds(slot, cpu, seed);
    slot.state.store<libk::MemoryOrder::Release>(PanicSlotState::Captured);
}

void print_source(const SourceLocation& source) noexcept {
    if (source.file == nullptr) {
        return;
    }
    panic_print<"site: {}:{}\nfunction: {}\n">(
        source.file, source.line, source.function);
    if (source.expression != nullptr) {
        panic_print<"expression: {}\n">(source.expression);
    }
}

void print_snapshot(const PanicSlot& slot) noexcept {
    panic_print<
        "cpu: logical={} hart={} trap-depth={} irq-before={}\n"
        "thread={:#x} active-root={} observed-epoch={}\n">(
        slot.cpu.raw,
        slot.hardware.raw,
        slot.trap_depth,
        slot.interrupts_enabled,
        slot.current_thread,
        slot.active_root,
        slot.observed_epoch);
    if (slot.has_full_trap) {
        const auto& gpr = slot.trap.gpr;
        panic_print<
            "context: full trap frame\n"
            "pc={:#018x} status={:#018x} cause={:#018x} fault={:#018x}\n"
            "ra={:#018x} sp={:#018x} gp={:#018x} tp={:#018x}\n"
            "t0={:#018x} t1={:#018x} t2={:#018x} t3={:#018x} "
                "t4={:#018x} t5={:#018x} t6={:#018x}\n">(
            slot.trap.pc,
            slot.trap.status,
            slot.trap.cause,
            slot.trap.fault_address,
            gpr[0], gpr[1], gpr[2], gpr[3],
            gpr[4], gpr[5], gpr[6], gpr[27], gpr[28], gpr[29], gpr[30]);
        panic_print<
            "a0={:#018x} a1={:#018x} a2={:#018x} a3={:#018x} "
                "a4={:#018x} a5={:#018x} a6={:#018x} a7={:#018x}\n"
            "s0={:#018x} s1={:#018x} s2={:#018x} s3={:#018x} "
                "s4={:#018x} s5={:#018x}\n"
            "s6={:#018x} s7={:#018x} s8={:#018x} s9={:#018x} "
                "s10={:#018x} s11={:#018x}\n">(
            gpr[9], gpr[10], gpr[11], gpr[12],
            gpr[13], gpr[14], gpr[15], gpr[16],
            gpr[7], gpr[8], gpr[17], gpr[18], gpr[19], gpr[20],
            gpr[21], gpr[22], gpr[23], gpr[24], gpr[25], gpr[26]);
    } else {
        panic_print<
            "context: call-site\n"
            "pc={:#018x} sp={:#018x} fp={:#018x} ra={:#018x}\n">(
            slot.call_site.pc,
            slot.call_site.sp,
            slot.call_site.frame_pointer,
            slot.call_site.return_address);
    }
}

[[nodiscard]] auto in_stack(
    const PanicSlot& slot,
    usize address,
    usize bytes) noexcept -> bool {
    return slot.stack_base != 0
        && address >= slot.stack_base
        && address <= slot.stack_top
        && bytes <= slot.stack_top - address;
}

[[nodiscard]] auto in_kernel_text(usize address) noexcept -> bool {
    return address >= reinterpret_cast<usize>(kernel_text_start)
        && address < reinterpret_cast<usize>(kernel_text_end);
}

void print_backtrace(const PanicSlot& slot) noexcept {
    panic_print<"backtrace:\n">();
    const arch::UnwindSeed seed = slot.has_full_trap
        ? arch::unwind_seed(slot.trap)
        : slot.call_site;
    if (seed.pc != 0 && in_kernel_text(seed.pc)) {
        panic_print<"  #0 {:#018x}\n">(seed.pc);
    }
    usize frame = seed.frame_pointer;
    usize printed = 1;
    bool first_record = true;
    for (usize walked = 1; walked < 32; ++walked) {
        if ((frame & (alignof(usize) - 1)) != 0
            || frame < 2 * sizeof(usize)
            || !in_stack(slot, frame - 2 * sizeof(usize),
                2 * sizeof(usize))) {
            panic_print<"  stopped: invalid frame pointer {:#018x}\n">(
                frame);
            return;
        }
        const auto* const record =
            reinterpret_cast<const usize*>(frame - 2 * sizeof(usize));
        const usize previous = record[0];
        const usize address = record[1];
        if (address == 0) {
            return;
        }
        if (!in_kernel_text(address)) {
            panic_print<
                "  stopped: return address outside kernel text {:#018x}\n">(
                address);
            return;
        }
        // A call-site seed names the return PC stored in panic()'s own frame.
        // Walk through that record, but do not report the same PC twice.
        if (!first_record || address != seed.pc) {
            panic_print<"  #{} {:#018x}\n">(printed, address);
            ++printed;
        }
        first_record = false;
        if (previous <= frame || previous > slot.stack_top) {
            panic_print<"  stopped: invalid previous frame {:#018x}\n">(
                previous);
            return;
        }
        frame = previous;
    }
}

void request_peer_stops(PanicSlot& owner) noexcept {
    CpuRegistry* const registry = owner.registry;
    if (registry == nullptr) {
        return;
    }
    for (usize index = 0; index < registry->count(); ++index) {
        const CpuId id{index};
        if (id == owner.cpu) {
            continue;
        }
        const CpuDescriptor* const descriptor = registry->descriptor(id);
        CpuRuntime* const runtime = registry->runtime(id);
        if (descriptor == nullptr || runtime == nullptr
            || descriptor->state() != CpuState::Online) {
            continue;
        }
        arch::request_panic_stop(runtime->local.arch_state);
        static_cast<void>(arch::send_ipi(descriptor->hardware_id()));
    }
}

void wait_for_peers(const PanicSlot& owner) noexcept {
    CpuRegistry* const registry = owner.registry;
    if (registry == nullptr) {
        return;
    }
    const u64 start = arch::read_clock().ticks();
    constexpr u64 timeout_ticks = 1'000'000;
    for (;;) {
        bool stopped = true;
        for (usize index = 0; index < registry->count(); ++index) {
            const CpuId id{index};
            if (id == owner.cpu) {
                continue;
            }
            const CpuDescriptor* const descriptor = registry->descriptor(id);
            const CpuRuntime* const runtime = registry->runtime(id);
            if (descriptor == nullptr || runtime == nullptr
                || descriptor->state() != CpuState::Online) {
                continue;
            }
            const PanicSlotState state = runtime->diagnostics->panic.state.load<
                libk::MemoryOrder::Acquire>();
            stopped = stopped && state == PanicSlotState::Stopped;
        }
        if (stopped || arch::read_clock().ticks() - start >= timeout_ticks) {
            return;
        }
    }
}

void print_peers(const PanicSlot& owner) noexcept {
    CpuRegistry* const registry = owner.registry;
    if (registry == nullptr) {
        return;
    }
    panic_print<"peer cpus:\n">();
    for (usize index = 0; index < registry->count(); ++index) {
        const CpuId id{index};
        const CpuRuntime* const runtime = registry->runtime(id);
        if (runtime == nullptr) {
            continue;
        }
        const PanicSlot& slot = runtime->diagnostics->panic;
        const PanicSlotState state = slot.state.load<libk::MemoryOrder::Acquire>();
        if (id == owner.cpu) {
            panic_print<"  cpu {}: owner\n">(id.raw);
        } else if (state == PanicSlotState::Stopped) {
            panic_print<"  cpu {}: stopped pc={:#018x}\n">(
                id.raw,
                slot.has_full_trap ? slot.trap.pc : slot.call_site.pc);
        } else {
            panic_print<"  cpu {}: no acknowledgement\n">(id.raw);
        }
    }
}

[[noreturn]] void panic_on_emergency_stack(void* argument) noexcept {
    auto& slot = *static_cast<PanicSlot*>(argument);
    PanicPhase expected = PanicPhase::Idle;
    if (!coordinator.phase.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, PanicPhase::Claimed)) {
        slot.state.store<libk::MemoryOrder::Release>(PanicSlotState::Stopped);
        platform::halt_current_cpu(platform::HaltReason::PeerStop);
    }
    coordinator.owner.store<libk::MemoryOrder::Release>(
        static_cast<u32>(slot.cpu.raw));
    const u64 sequence = coordinator.sequence.fetch_add<
        libk::MemoryOrder::Relaxed>(1) + 1;
    coordinator.phase.store<libk::MemoryOrder::Release>(
        PanicPhase::StoppingPeers);
    request_peer_stops(slot);
    wait_for_peers(slot);
    coordinator.phase.store<libk::MemoryOrder::Release>(PanicPhase::Dumping);

    panic_print<
        "\n================ MYOS KERNEL PANIC ================\n"
        "build: {}\nsequence: {}\n"
        "event: {:#010x} kind={} facility={}\n">(
        MYOS_BUILD_ID,
        sequence,
        slot.request.event.id.raw,
        static_cast<u8>(slot.request.kind),
        static_cast<u8>(slot.request.event.facility));
    print_source(slot.request.source);
    for (usize index = 0; index < slot.request.event.argument_count; ++index) {
        panic_print<"arg{}={:#018x}\n">(
            index, slot.request.event.arguments[index]);
    }
    print_snapshot(slot);
    print_backtrace(slot);
    print_peers(slot);
    panic_print<"====================================================\n">();
    coordinator.phase.store<libk::MemoryOrder::Release>(PanicPhase::Halted);
    platform::halt_system(
        platform::HaltAction::Shutdown,
        platform::HaltReason::Panic);
}

} // namespace

auto stop_requested() noexcept -> bool {
    return arch::panic_stop_requested();
}

[[noreturn]] void panic(PanicRequest request) noexcept {
    const arch::CallSiteSnapshot call_site = arch::capture_call_site();
    const arch::InterruptState interrupts = arch::disable_interrupts();
    void* const slot_pointer = arch::panic_slot();
    const usize stack_top = arch::emergency_stack();
    if (slot_pointer == nullptr || stack_top == 0) {
        raw_text("\nEARLY KERNEL PANIC\n");
        raw_source(request.source);
        platform::halt_system(
            platform::HaltAction::Shutdown,
            platform::HaltReason::Fatal);
    }
    auto& slot = *static_cast<PanicSlot*>(slot_pointer);
    if (!arch::enter_emergency()) {
        const usize pc = request.trap != nullptr ? request.trap->pc() : 0;
        double_panic(slot.cpu.raw, pc, 0);
    }
    capture(slot, request, call_site, interrupts.enabled());
    arch::switch_to_panic_stack(
        stack_top,
        &slot,
        panic_on_emergency_stack);
}

void assert_fail(
    const char* expression,
    const char* file,
    const char* function,
    u32 line) noexcept {
    panic(PanicRequest{
        .kind = PanicKind::Assertion,
        .event = FatalEvent{Facility::Core, EventId{0x10000001}},
        .source = SourceLocation{file, function, expression, line},
    });
}

void fatal(FatalEvent event, SourceLocation source) noexcept {
    panic(PanicRequest{
        .kind = PanicKind::ExplicitFatal,
        .event = event,
        .source = source,
    });
}

void panic_unhandled(
    FatalEvent event,
    const arch::TrapContext& trap) noexcept {
    panic(PanicRequest{
        .kind = PanicKind::UnhandledTrap,
        .event = event,
        .trap = &trap,
    });
}

void stop_peer(const arch::TrapContext& trap) noexcept {
    panic(PanicRequest{
        .kind = PanicKind::PeerStop,
        .event = FatalEvent{Facility::Core, EventId{0x10000002}},
        .trap = &trap,
    });
}

} // namespace kernel::diag

namespace libk {

void assert_fail(const AssertInfo& info) noexcept {
    kernel::diag::assert_fail(
        info.expression,
        info.file,
        info.function,
        static_cast<u32>(info.line));
}

} // namespace libk
