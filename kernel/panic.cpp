#include <panic.hpp>
#include <console.hpp>
#include <trace.hpp>

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <arch/ipi.hpp>
#include <arch/time.hpp>
#include <cpu/local.hpp>
#include <cpu/registry.hpp>
#include <cpu/runtime.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <libk/fmt.hpp>
#include <arch/console.hpp>
#include <arch/system.hpp>
#include <boot/link.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>


extern "C" char kernel_text_start[];
extern "C" char kernel_text_end[];

static constinit libk::Atomic<bool> claimed{};

static void raw_char(char character) noexcept {
    arch::console::write(character);
}

static void raw_text(const char* text) noexcept {
    if (text == nullptr) {
        raw_text("<none>");
        return;
    }
    while (*text != '\0') {
        raw_char(*text++);
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
        raw_char(digits[--count]);
    }
}

static void raw_hex(usize value) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    raw_text("0x");
    for (usize shift = sizeof(usize) * 8; shift != 0; shift -= 4) {
        raw_char(digits[(value >> (shift - 4)) & 0xfU]);
    }
}



static void raw_source(const libk::AssertInfo& source) noexcept {
    if (source.file == nullptr) {
        return;
    }
    raw_text("site: ");
    raw_text(source.file);
    raw_char(':');
    raw_decimal(source.line);
    raw_char('\n');
}

/*luna change: extend double-panic projection with entry and canonical target stack facts, reason: distinguish stale target stack state from active-stack publication corruption without changing panic control*/
[[noreturn]] static void double_panic(
    usize cpu,
    const arch::TrapSnapshot& snapshot) noexcept {
    auto& local = current_cpu();
    const auto* const dispatcher = local.dispatcher();
    Thread* target = dispatcher != nullptr
        ? dispatcher->current() : nullptr;
    const usize entry_top = arch::active_stack(local.arch_state);
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
    raw_char('\n');
    arch::halt_current_cpu(arch::HaltReason::Fatal);
}

static void capture_stack_bounds(
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
    const mm::Stack* stacks[] = {
        runtime.init_stack ? &*runtime.init_stack : nullptr,
        runtime.irq_stack ? &*runtime.irq_stack : nullptr,
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
    const char* reason, libk::AssertInfo site, const arch::TrapContext* trap,
    arch::CallSiteSnapshot call_site,
    bool interrupts_were_enabled) noexcept {
    slot.reason = reason;
    slot.site = site;
    slot.has_full_trap = trap != nullptr;
    if (trap != nullptr) {
        slot.trap = trap->snapshot();
    }
    slot.call_site = call_site;
    slot.interrupts_enabled = interrupts_were_enabled;

    void* const owner = arch::current_cpu_owner();
    libk_assert(owner != nullptr);
    auto& cpu = *static_cast<CpuLocal*>(owner);
    slot.registry = cpu.runtime().owner_registry;
    slot.current_thread = reinterpret_cast<usize>(cpu.current_thread());
    slot.active_root = cpu.active_root_ ? 1 : 0;
    slot.trap_depth = arch::trap_depth();
    const arch::UnwindSeed seed = slot.has_full_trap
        ? arch::unwind_seed(slot.trap)
        : slot.call_site;
    capture_stack_bounds(slot, cpu, seed);
}

static void print_source(const libk::AssertInfo& source) noexcept {
    if (source.file == nullptr) {
        return;
    }
    console::print<"site: {}:{}\nfunction: {}\n">(
        source.file, source.line, source.function);
    if (source.expression != nullptr) {
        console::print<"expression: {}\n">(source.expression);
    }
}

static void print_snapshot(const PanicSlot& slot) noexcept {
    console::print<
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
        console::print<
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
        console::print<
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
        console::print<
            "context: call-site\n"
            "pc={:#018x} sp={:#018x} fp={:#018x} ra={:#018x}\n">(
            slot.call_site.pc,
            slot.call_site.sp,
            slot.call_site.frame_pointer,
            slot.call_site.return_address);
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
    console::print<"backtrace:\n">();
    const arch::UnwindSeed seed = slot.has_full_trap
        ? arch::unwind_seed(slot.trap)
        : slot.call_site;
    if (seed.pc != 0 && in_kernel_text(seed.pc)) {
        console::print<"  #0 {:#018x}\n">(seed.pc);
    }
    usize frame = seed.frame_pointer;
    usize printed = 1;
    bool first_record = true;
    for (usize walked = 1; walked < 32; ++walked) {
        if ((frame & (alignof(usize) - 1)) != 0
            || frame < 2 * sizeof(usize)
            || !in_stack(slot, frame - 2 * sizeof(usize),
                2 * sizeof(usize))) {
            console::print<"  stopped: invalid frame pointer {:#018x}\n">(
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
            console::print<
                "  stopped: return address outside kernel text {:#018x}\n">(
                address);
            return;
        }
        // A call-site seed names the return PC stored in panic()'s own frame.
        // Walk through that record, but do not report the same PC twice.
        if (!first_record || address != seed.pc) {
            console::print<"  #{} {:#018x}\n">(printed, address);
            ++printed;
        }
        first_record = false;
        if (previous <= frame || previous > slot.stack_top) {
            console::print<"  stopped: invalid previous frame {:#018x}\n">(
                previous);
            return;
        }
        frame = previous;
    }
}

static void request_peer_stops(PanicSlot& owner) noexcept {
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

static void wait_for_peers(const PanicSlot& owner) noexcept {
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
            stopped = stopped && runtime->panic != nullptr
                && runtime->panic->stopped.load<libk::MemoryOrder::Acquire>();
        }
        if (stopped || arch::read_clock().ticks() - start >= timeout_ticks) {
            return;
        }
    }
}

static void print_peers(const PanicSlot& owner) noexcept {
    CpuRegistry* const registry = owner.registry;
    if (registry == nullptr) {
        return;
    }
    console::print<"peer cpus:\n">();
    for (usize index = 0; index < registry->count(); ++index) {
        const CpuId id{index};
        const CpuRuntime* const runtime = registry->runtime(id);
        if (runtime == nullptr || runtime->panic == nullptr) {
            continue;
        }
        const PanicSlot& slot = *runtime->panic;
        if (id == owner.cpu) {
            console::print<"  cpu {}: owner\n">(id.raw);
        } else if (slot.stopped.load<libk::MemoryOrder::Acquire>()) {
            const arch::UnwindSeed seed = slot.has_full_trap
                ? arch::unwind_seed(slot.trap)
                : slot.call_site;
            console::print<
                "  cpu {}: stopped pc={:#018x} sp={:#018x} "
                "fp={:#018x} ra={:#018x}\n">(
                id.raw,
                seed.pc,
                seed.sp,
                seed.frame_pointer,
                seed.return_address);
            print_snapshot(slot);
            print_backtrace(slot);
        } else {
            console::print<"  cpu {}: no acknowledgement\n">(id.raw);
        }
        trace::Sample e{};
        const auto log = trace::snapshot(*runtime);
        for (u64 n = log.first; n < log.last; ++n) {
            if (log.read(n, e)) {
                console::print<"    trace {} t={} kind={} actor={:#x} object={:#x} a={:#x} b={:#x}\n">(
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

    console::print<
        "\n================ MYOS KERNEL PANIC ================\n"
        "build: {}\nreason: {}\n">(
        libk::StrView::from_cstr(build_id), slot.reason);
    print_source(slot.site);
    print_snapshot(slot);
    print_backtrace(slot);
    print_peers(slot);
    console::print<"====================================================\n">();
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
    const char* reason, libk::AssertInfo site, const arch::TrapContext* trap,
    arch::PanicContinuation entry) noexcept {
    const arch::CallSiteSnapshot call_site = arch::capture_call_site();
    const arch::InterruptState interrupts = arch::disable_interrupts();
    void* const ptr = arch::panic_slot();
    const usize top = arch::emergency_stack();
    if (ptr == nullptr || top == 0) {
        raw_text("\nEARLY KERNEL PANIC\n");
        raw_text(reason);
        raw_char('\n');
        raw_source(site);
        arch::halt_system(arch::HaltAction::Shutdown, arch::HaltReason::Fatal);
    }
    auto& slot = *static_cast<PanicSlot*>(ptr);
    if (!arch::enter_emergency()) {
        double_panic(slot.cpu.raw, trap ? trap->snapshot() : arch::TrapSnapshot{});
    }
    capture(slot, reason, site, trap, call_site, interrupts.enabled());
    arch::switch_to_panic_stack(top, &slot, entry);
}

void panic(const char* reason, const arch::TrapContext* trap,
           std::source_location site) noexcept {
    enter_panic(reason, {nullptr, site.file_name(), site.function_name(), site.line()},
                trap, panic_on_emergency_stack);
}

void panic_stop(const arch::TrapContext& trap) noexcept {
    enter_panic("peer stop", {}, &trap, halt_peer);
}

void libk::assert_fail(const AssertInfo& info) noexcept {
    enter_panic("assertion failed", info, nullptr, panic_on_emergency_stack);
}

extern "C" [[noreturn]] void abort() noexcept { panic("aborted"); }
