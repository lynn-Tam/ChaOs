#include <trace.hpp>

#include <cpu.hpp>
#include <cpu/cpu.hpp>

namespace trace {

#if TRACE_ENABLED
void emit(Event kind, u64 actor, u64 object, u64 a, u64 b) noexcept {
    const auto irq = arch::disable_interrupts();
    auto* cpu = Cpu::local();
    auto* log = cpu ? cpu->log : nullptr;
    if (log) {
        const u64 head = log->head.load(std::memory_order_relaxed);
        auto& slot = log->cells[head % Ring::capacity];
        slot.seq.store(head * 2 + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        slot.tick.store(arch::read_clock(), std::memory_order_relaxed);
        slot.actor.store(actor, std::memory_order_relaxed);
        slot.object.store(object, std::memory_order_relaxed);
        slot.a.store(a, std::memory_order_relaxed);
        slot.b.store(b, std::memory_order_relaxed);
        slot.kind.store(kind, std::memory_order_relaxed);
        slot.seq.store(head * 2 + 2, std::memory_order_release);
        log->head.store(head + 1, std::memory_order_release);
    }
    arch::restore_interrupts(irq);
}

#endif

auto snapshot(const Cpu& cpu) noexcept -> View {
    const auto* log = cpu.log;
    const u64 last = log ? log->head.load(std::memory_order_acquire) : 0;
    return {log, last > Ring::capacity ? last - Ring::capacity : 0, last};
}

auto View::read(u64 seq, Sample& out) const noexcept -> bool {
    if (!log || seq < first || seq >= last) return false;
    auto& slot = log->cells[seq % Ring::capacity];
    const u64 stamp = slot.seq.load(std::memory_order_acquire);
    if (stamp != seq * 2 + 2) return false;
    Sample value{seq,
        slot.tick.load(std::memory_order_relaxed),
        slot.actor.load(std::memory_order_relaxed),
        slot.object.load(std::memory_order_relaxed),
        slot.a.load(std::memory_order_relaxed),
        slot.b.load(std::memory_order_relaxed),
        slot.kind.load(std::memory_order_relaxed)};
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.seq.load(std::memory_order_relaxed) != stamp) return false;
    out = value;
    return true;
}

} // namespace trace
