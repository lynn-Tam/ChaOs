#include <cpu/ipi.hpp>
#include <cpu.hpp>
#include <cpu/cpu.hpp>
#include <mm/tlb.hpp>
#include <pte.hpp>
#include <trace.hpp>

namespace mm {
// Requests themselves provide storage; no per-CPU bounded queue or reserve
// protocol. The lock serializes publication with each consumer's fence.
static constinit sync::Spin flush_lock;
static constinit Flush *flushes{};

Flush::~Flush() noexcept {
    libk_assert(complete() && !link_);
    pages_.reset();
    fee_.reset();
    if (owner_) libk_assert(owner_->retained_.fetch_sub<libk::MemoryOrder::Release>(1) != 0);
}

bool Flush::release(resource::Charge &refund) noexcept {
    if (!complete()) return false;
    libk_assert(!refund);
    pages_.reset();
    refund = std::move(fee_);
    if (owner_) {
        libk_assert(owner_->retained_.fetch_sub<libk::MemoryOrder::Release>(1) != 0);
        owner_ = nullptr;
    }
    return true;
}

bool Flush::acknowledged(CpuId cpu) const noexcept {
    sync::Lock lock{flush_lock};
    return targets_.contains(cpu) && !pending_.contains(cpu);
}

// Runs under flush_lock. Unlink before publishing completion: another CPU
// may destroy the request immediately after its completion becomes visible.
void Flush::ack(CpuId cpu) noexcept {
    libk_assert(pending_.erase(cpu));
    static_cast<void>(done_.acknowledge([&]() noexcept {
        *link_ = next_;
        if (next_) next_->link_ = link_;
        link_ = nullptr;
        next_ = nullptr;
    }));
}

bool Flush::kick(Cpus &cpus) const noexcept {
    CpuSet pending;
    {
        sync::Lock lock{flush_lock};
        pending = pending_;
    }
    bool sent = true;
    pending.for_each([&](CpuId cpu) noexcept {
        const auto *target = cpus.get(cpu);
        libk_assert(target);
        if (!send_ipi(target->hw)) {
            sent = false;
            trace::emit(trace::Event::KickFail, cpu.raw);
        }
    });
    return sent;
}

bool Tlb::Edit::commit(Flush &flush, Cpus *cpus, CpuId local, bool executable) noexcept {
    libk_assert(owner_ && !flush.submitted());
    flush.owner_ = owner_;
    flush.executable_ = executable;
    static_cast<void>(owner_->retained_.fetch_add<libk::MemoryOrder::Relaxed>(1));
    {
        sync::Lock lock{flush_lock};
        flush.targets_ = flush.pending_ = owner_->active_;
        flush.done_.initialize(flush.pending_.size());
        if (!flush.pending_.empty()) {
            flush.next_ = flushes;
            flush.link_ = &flushes;
            if (flushes) flushes->link_ = &flush.next_;
            flushes = &flush;
            if (flush.pending_.contains(local)) {
                arch::flush_tlb_all();
                if (executable) arch::sync_instruction_stream();
                flush.ack(local);
            }
        }
    }
    trace::emit(trace::Event::Post, reinterpret_cast<u64>(&flush), flush.targets_.size());
    // Keep IRQs disabled until all sends and notifier arming finish, while
    // admitting late entrants only after the new PTEs are published.
    lock_.unlock();
    owner_ = nullptr;
    if (cpus) static_cast<void>(flush.kick(*cpus));
    else {
        libk_assert(flush.complete()); // Bootstrap/native paths are local.
    }
    const bool complete = flush.complete() || (flush.done_.notifiable() && !flush.done_.arm());
    lock_.restore();
    return complete;
}

void Tlb::Edit::publish_fresh() noexcept {
    // Only monotonically allocated addresses with permanent backing use this
    // path. It cannot publish a reused VA or a replacement physical page.
    libk_assert(owner_);
    arch::flush_tlb_all();
    abort();
}

Tlb::~Tlb() noexcept { libk_assert(active_.empty() && retained_.load<libk::MemoryOrder::Acquire>() == 0); }
void Tlb::enter(CpuId cpu) noexcept {
    sync::Lock lock{lock_};
    libk_assert(active_.insert(cpu));
}
void Tlb::leave(CpuId cpu) noexcept {
    sync::Lock lock{lock_};
    libk_assert(active_.erase(cpu));
}
CpuSet Tlb::active_cpus() const noexcept {
    sync::Lock lock{lock_};
    return active_;
}

void Root::activate(Cpu &cpu) const noexcept {
    libk_assert(!arch::interrupts_enabled());
    const auto id = cpu.id;
    auto *outgoing = cpu.tlb;
    if (outgoing != state_) state_->enter(id);
    if (outgoing != state_) arch::activate_root(root_);
    else libk_assert(cpu.root == root_);
    // Every entry fences, including a same-root resumption. This replaces
    // version caches and also covers edits committed while this CPU was absent.
    arch::flush_tlb_all();
    arch::sync_instruction_stream();
    cpu.tlb = state_;
    cpu.root = root_;
    if (outgoing && outgoing != state_) outgoing->leave(id);
}

void Root::adopt(Cpu &cpu) const noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(!cpu.tlb && !cpu.root && arch::root_active(root_));
    state_->enter(cpu.id);
    arch::flush_tlb_all();
    arch::sync_instruction_stream();
    cpu.tlb = state_;
    cpu.root = root_;
}

void drain_tlb(CpuId cpu) noexcept {
    libk_assert(!arch::interrupts_enabled());
    sync::Lock lock{flush_lock};
    bool fenced{}, executable{};
    for (auto *p = flushes; p;) {
        auto &flush = *p;
        p = p->next_;
        if (!flush.pending_.contains(cpu)) continue;
        if (!fenced) {
            arch::flush_tlb_all();
            fenced = true;
        }
        if (flush.executable_ && !executable) {
            arch::sync_instruction_stream();
            executable = true;
        }
        // No dereference follows ack: its owner may now release the request.
        trace::emit(trace::Event::Complete, cpu.raw, reinterpret_cast<u64>(&flush));
        flush.ack(cpu);
    }
}
} // namespace mm
