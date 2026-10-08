#include <cpu/ipi.hpp>
#include <panic.hpp>

extern const char build_id[];
#include <console.hpp>
#include <trace.hpp>

#include <cpu.hpp>
#include <cpu/cpu.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <libk/fmt.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>


extern "C" char kernel_text_start[];
extern "C" char kernel_text_end[];

static constinit libk::Atomic<bool> claimed{};

static void raw_text(const char* text) noexcept {
    if (text == nullptr) {
        raw_text("<none>");
        return;
    }
    while (*text != '\0') {
        arch::putchar(*text++);
    }
}

static void raw_decimal(u64 value) noexcept {
    char digits[20]{};
    usize count{};
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        arch::putchar(digits[--count]);
    }
}

static void raw_hex(usize value) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    raw_text("0x");
    for (usize shift = sizeof(usize) * 8; shift != 0; shift -= 4) {
        arch::putchar(digits[(value >> (shift - 4)) & 0xfU]);
    }
}



static void raw_source(const libk::AssertInfo& source) noexcept {
    if (source.file == nullptr) {
        return;
    }
    raw_text("site: ");
    raw_text(source.file);
    arch::putchar(':');
    raw_decimal(source.line);
    arch::putchar('\n');
}

[[noreturn]] static void double_panic(
    usize cpu,
    const arch::TrapRegs& snapshot) noexcept {
    auto& local = current_cpu();
    const auto* const dispatcher = local.dispatcher_storage ? &*local.dispatcher_storage : nullptr;
    Thread* target = dispatcher != nullptr
        ? dispatcher->current() : nullptr;
    const usize entry_top = local.entry.stack;
    raw_text("\nDOUBLE PANIC cpu=");
    raw_decimal(cpu);
    raw_text(" pc=");
    raw_hex(snapshot.pc);
    raw_text(" cause=");
    raw_hex(snapshot.cause);
    raw_text(" addr=");
    raw_hex(snapshot.fault_address);
    raw_text(" entry_top=");
    raw_hex(entry_top);
    raw_text(" target_kind=");
    if (!target) {
        raw_text("none");

    } else {
        raw_text("thread");
    }
    raw_text(" target_id=");
    raw_hex(target ? target->identity() : 0);
    raw_text(" stack_base=");
    raw_hex(target ? target->home_stack_base() : 0);
    raw_text(" stack_top=");
    raw_hex(target ? target->current_stack_top() : 0);
    arch::putchar('\n');
    arch::halt_current_cpu(arch::HaltReason::Fatal);
}

static void capture_stack_bounds(
    PanicSlot& slot,
    Cpu& cpu,
    usize sp) noexcept {
    Thread* const thread = cpu.current;
    if (thread != nullptr && sp >= thread->home_stack_base()
        && sp < thread->home_stack_top()) {
        slot.stack_base = thread->home_stack_base();
        slot.stack_top = thread->home_stack_top();
        return;
    }
    Cpu& runtime = cpu;
    const mm::Stack* stacks[] = {
        runtime.init_stack ? &*runtime.init_stack : nullptr,
        runtime.emergency_stack ? &*runtime.emergency_stack : nullptr,
    };
    for (const mm::Stack* stack : stacks) {
        if (stack != nullptr && stack->contains(sp)) {
            slot.stack_base = stack->base();
            slot.stack_top = stack->top();
            return;
        }
    }
}

static void capture(
    PanicSlot& slot,
    const char* reason, libk::AssertInfo site, const arch::TrapCtx* trap,
    arch::StackRegs call_site,
    bool interrupts_were_enabled) noexcept {
    slot.reason = reason;
    slot.site = site;
    slot.has_full_trap = trap != nullptr;
    if (trap != nullptr) {
        slot.trap = trap->snapshot();
    }
    slot.stack = trap ? slot.trap.stack() : call_site;
    slot.interrupts_enabled = interrupts_were_enabled;

    auto* const owner = arch::local() ? arch::local()->owner : nullptr;
    libk_assert(owner != nullptr);
    auto& cpu = *owner;
    slot.registry = cpu.cpus;
    slot.current_thread = reinterpret_cast<usize>(cpu.current);
    slot.active_root = cpu.root ? 1 : 0;
    slot.trap_depth = arch::local()->depth;
    capture_stack_bounds(slot, cpu, slot.stack.sp);
}

static void print_source(const libk::AssertInfo& source) noexcept {
    if (source.file == nullptr) {
        return;
    }
    console::raw<"site: {}:{}\nfunction: {}\n">(
        source.file, source.line, source.function);
    if (source.expression != nullptr) {
        console::raw<"expression: {}\n">(source.expression);
    }
}

static void print_snapshot(const PanicSlot& slot) noexcept {
    console::raw<
        "cpu: logical={} hart={} trap-depth={} irq-before={}\n"
        "thread={:#x} active-root={}\n">(
        slot.cpu.raw,
        slot.hardware.raw,
        slot.trap_depth,
        slot.interrupts_enabled,
        slot.current_thread,
        slot.active_root);
    if (slot.has_full_trap) {
        const auto& gpr = slot.trap.gpr;
        console::raw<
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
        console::raw<
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
        console::raw<
            "context: call-site\n"
            "pc={:#018x} sp={:#018x} fp={:#018x} ra={:#018x}\n">(
            slot.stack.pc,
            slot.stack.sp,
            slot.stack.fp,
            slot.stack.ra);
    }
}

[[nodiscard]] static auto in_stack(
    const PanicSlot& slot,
    usize address,
    usize bytes) noexcept -> bool {
    return slot.stack_base != 0
        && address >= slot.stack_base
        && address <= slot.stack_top
        && bytes <= slot.stack_top - address;
}

[[nodiscard]] static auto in_kernel_text(usize address) noexcept -> bool {
    return address >= reinterpret_cast<usize>(kernel_text_start)
        && address < reinterpret_cast<usize>(kernel_text_end);
}

static void print_backtrace(const PanicSlot& slot) noexcept {
    console::raw<"backtrace:\n">();
    const auto& seed = slot.stack;
    if (seed.pc != 0 && in_kernel_text(seed.pc)) {
        console::raw<"  #0 {:#018x}\n">(seed.pc);
    }
    usize frame = seed.fp;
    usize printed = 1;
    bool first_record = true;
    for (usize walked = 1; walked < 32; ++walked) {
        if ((frame & (alignof(usize) - 1)) != 0
            || frame < 2 * sizeof(usize)
            || !in_stack(slot, frame - 2 * sizeof(usize),
                2 * sizeof(usize))) {
            console::raw<"  stopped: invalid frame pointer {:#018x}\n">(
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
            console::raw<
                "  stopped: return address outside kernel text {:#018x}\n">(
                address);
            return;
        }
        // A call-site seed names the return PC stored in panic()'s own frame.
        // Walk through that record, but do not report the same PC twice.
        if (!first_record || address != seed.pc) {
            console::raw<"  #{} {:#018x}\n">(printed, address);
            ++printed;
        }
        first_record = false;
        if (previous <= frame || previous > slot.stack_top) {
            console::raw<"  stopped: invalid previous frame {:#018x}\n">(
                previous);
            return;
        }
        frame = previous;
    }
}

static void request_peer_stops(PanicSlot& owner) noexcept {
    Cpus* const registry = owner.registry;
    if (registry == nullptr) {
        return;
    }
    for (usize index = 0; index < registry->count(); ++index) {
        const CpuId id{index};
        if (id == owner.cpu) {
            continue;
        }
        Cpu* const runtime = registry->get(id);
        if (runtime) runtime->request_stop();
    }
}

static void wait_for_peers(const PanicSlot& owner) noexcept {
    Cpus* const registry = owner.registry;
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
            const Cpu* const runtime = registry->get(id);
            if (runtime == nullptr || !runtime->online()) {
                continue;
            }
            stopped = stopped && runtime->panic != nullptr
                && runtime->panic->stopped.load<libk::MemoryOrder::Acquire>();
        }
        if (stopped || arch::read_clock().ticks() - start >= timeout_ticks) {
            return;
        }
    }
}

static void print_peers(const PanicSlot& owner) noexcept {
    Cpus* const registry = owner.registry;
    if (registry == nullptr) {
        return;
    }
    console::raw<"peer cpus:\n">();
    for (usize index = 0; index < registry->count(); ++index) {
        const CpuId id{index};
        const Cpu* const runtime = registry->get(id);
        if (runtime == nullptr || runtime->panic == nullptr) {
            continue;
        }
        const PanicSlot& slot = *runtime->panic;
        if (id == owner.cpu) {
            console::raw<"  cpu {}: owner\n">(id.raw);
        } else if (slot.stopped.load<libk::MemoryOrder::Acquire>()) {
            const auto& seed = slot.stack;
            console::raw<
                "  cpu {}: stopped pc={:#018x} sp={:#018x} "
                "fp={:#018x} ra={:#018x}\n">(
                id.raw,
                seed.pc,
                seed.sp,
                seed.fp,
                seed.ra);
            print_snapshot(slot);
            print_backtrace(slot);
        } else {
            console::raw<"  cpu {}: no acknowledgement\n">(id.raw);
        }
        trace::Sample e{};
        const auto log = trace::snapshot(*runtime);
        for (u64 n = log.first; n < log.last; ++n) {
            if (log.read(n, e)) {
                console::raw<"    trace {} t={} kind={} actor={:#x} object={:#x} a={:#x} b={:#x}\n">(
                    e.seq, e.tick, static_cast<u32>(e.kind), e.actor, e.object, e.a, e.b);
            }
        }
    }
}

[[noreturn]] static void panic_on_emergency_stack(void* argument) noexcept {
    auto& slot = *static_cast<PanicSlot*>(argument);
    bool expected{};
    if (!claimed.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, true)) {
        slot.stopped.store<libk::MemoryOrder::Release>(true);
        arch::halt_current_cpu(arch::HaltReason::PeerStop);
    }
    request_peer_stops(slot);
    wait_for_peers(slot);

    console::raw<
        "\n================ MYOS KERNEL PANIC ================\n"
        "build: {}\nreason: {}\n">(
        libk::StrView::from_cstr(build_id), slot.reason);
    print_source(slot.site);
    print_snapshot(slot);
    print_backtrace(slot);
    print_peers(slot);
    console::raw<"====================================================\n">();
    arch::halt_system(
        arch::HaltAction::Shutdown,
        arch::HaltReason::Panic);
}

static void halt_peer(void* ptr) noexcept {
    auto& slot = *static_cast<PanicSlot*>(ptr);
    // Capture is complete before publishing the acknowledgement. This CPU
    // cannot re-enter normal execution after the release store.
    slot.stopped.store<libk::MemoryOrder::Release>(true);
    arch::halt_current_cpu(arch::HaltReason::PeerStop);
}

[[noreturn]] static void enter_panic(
    const char* reason, libk::AssertInfo site, const arch::TrapCtx* trap,
    void (*entry)(void*) noexcept) noexcept {
    const arch::StackRegs call_site = arch::stack_regs();
    const bool interrupts = arch::disable_interrupts();
    auto* const local = arch::local();
    if (!local || !local->panic || !local->emergency_stack || (local->emergency_stack & 15U)) {
        raw_text("\nEARLY KERNEL PANIC\n");
        raw_text(reason);
        arch::putchar('\n');
        raw_source(site);
        arch::halt_system(arch::HaltAction::Shutdown, arch::HaltReason::Fatal);
    }
    auto& slot = *local->panic;
    if (local->emergency_depth) {
        double_panic(slot.cpu.raw, trap ? trap->snapshot() : arch::TrapRegs{});
    }
    local->emergency_depth = 1;
    capture(slot, reason, site, trap, call_site, interrupts);
    arch::switch_stack(local->emergency_stack, &slot, entry);
}

void panic(const char* reason, const arch::TrapCtx* trap,
           std::source_location site) noexcept {
    enter_panic(reason, {nullptr, site.file_name(), site.function_name(), site.line()},
                trap, panic_on_emergency_stack);
}

void panic_stop(const arch::TrapCtx& trap) noexcept {
    enter_panic("peer stop", {}, &trap, halt_peer);
}

void libk::assert_fail(const AssertInfo& info) noexcept {
    enter_panic("assertion failed", info, nullptr, panic_on_emergency_stack);
}

extern "C" [[noreturn]] void abort() noexcept { panic("aborted"); }
