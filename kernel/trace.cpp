#include <trace.hpp>

#include <cpu.hpp>
#include <cpu/cpu.hpp>

namespace trace {

auto enabled() noexcept -> bool { return TRACE_ENABLED; }

void emit(Event kind, u64 actor, u64 object, u64 a, u64 b) noexcept {
    if (!enabled()) return;
    const auto irq = arch::disable_interrupts();
    auto* cpu = (arch::local() ? arch::local()->owner : nullptr);
    auto* log = cpu ? cpu->log : nullptr;
    if (log) {
        const u64 head = log->head.load<libk::MemoryOrder::Relaxed>();
        auto& slot = log->cells[head % Ring::capacity];
        slot.seq.store<libk::MemoryOrder::Release>(head * 2 + 1);
        libk::atomic_thread_fence<libk::MemoryOrder::Release>();
        slot.tick.store<libk::MemoryOrder::Relaxed>(arch::read_clock().ticks());
        slot.actor.store<libk::MemoryOrder::Relaxed>(actor);
        slot.object.store<libk::MemoryOrder::Relaxed>(object);
        slot.a.store<libk::MemoryOrder::Relaxed>(a);
        slot.b.store<libk::MemoryOrder::Relaxed>(b);
        slot.kind.store<libk::MemoryOrder::Relaxed>(kind);
        slot.seq.store<libk::MemoryOrder::Release>(head * 2 + 2);
        log->head.store<libk::MemoryOrder::Release>(head + 1);
    }
    arch::restore_interrupts(irq);
}

auto snapshot(const Cpu& cpu) noexcept -> View {
    const auto* log = cpu.log;
    const u64 last = log ? log->head.load<libk::MemoryOrder::Acquire>() : 0;
    return {log, last > Ring::capacity ? last - Ring::capacity : 0, last};
}

auto View::read(u64 seq, Sample& out) const noexcept -> bool {
    if (!log || seq < first || seq >= last) return false;
    auto& slot = log->cells[seq % Ring::capacity];
    const u64 stamp = slot.seq.load<libk::MemoryOrder::Acquire>();
    if (stamp != seq * 2 + 2) return false;
    Sample value{seq,
        slot.tick.load<libk::MemoryOrder::Relaxed>(),
        slot.actor.load<libk::MemoryOrder::Relaxed>(),
        slot.object.load<libk::MemoryOrder::Relaxed>(),
        slot.a.load<libk::MemoryOrder::Relaxed>(),
        slot.b.load<libk::MemoryOrder::Relaxed>(),
        slot.kind.load<libk::MemoryOrder::Relaxed>()};
    libk::atomic_thread_fence<libk::MemoryOrder::Acquire>();
    if (slot.seq.load<libk::MemoryOrder::Relaxed>() != stamp) return false;
    out = value;
    return true;
}

} // namespace trace
