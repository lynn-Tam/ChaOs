#include <cpu/ipi.hpp>
#include <cpu.hpp>
#include <cpu/cpu.hpp>
#include <mm/tlb.hpp>
#include <pte.hpp>
#include <trace.hpp>
#include <bit>
#include <algorithm>

namespace mm {
// Requests themselves provide storage; no per-CPU bounded queue or reserve
// protocol. The lock serializes publication with each consumer's fence.
static constinit sync::Spin flush_lock;
static constinit Flush *flushes{};
static constinit std::atomic<u64> next_id{1};

Tlb::Tlb() noexcept : id_(next_id.fetch_add(1, std::memory_order_relaxed)) { libk_assert(id_); }

Flush::~Flush() noexcept {
    libk_assert(complete() && !link_);
    pages_.reset();
    fee_.reset();
    if (owner_) libk_assert(owner_->retained_.fetch_sub(1, std::memory_order_release) != 0);
}

bool Flush::release(resource::Usage &release) noexcept {
    if (!complete()) return false;
    libk_assert(!release);
    for (usize i = 0; i < 8 && pages_.page_count(); ++i) {
        auto page = pages_.take();
        if (!page) break;
        page->reset();
    }
    if (pages_.page_count()) return false;
    pages_.reset();
    release = std::move(fee_);
    if (owner_) {
        libk_assert(owner_->retained_.fetch_sub(1, std::memory_order_release) != 0);
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

bool Tlb::Edit::commit(Flush &flush, Cpus *cpus, CpuId local, bool executable, bool global) noexcept {
    libk_assert(owner_ && !flush.submitted());
    flush.version_ = owner_->version_.fetch_add(1, std::memory_order_release) + 1;
    libk_assert(flush.version_);
    if (executable) libk_assert(owner_->executable_.fetch_add(1, std::memory_order_release) != ~u64{});
    flush.owner_ = owner_;
    flush.executable_ = executable; flush.global_ = global;
    static_cast<void>(owner_->retained_.fetch_add(1, std::memory_order_relaxed));
    {
        sync::Lock lock{flush_lock};
        flush.targets_ = owner_->active_;
        if (global) {
            // Shared kernel branches are cached while user roots are active.
            // Late CPU entrants fence before using the shared mappings.
            libk_assert(cpus);
            flush.targets_ = {};
            for (usize i = 0; i < cpus->count(); ++i)
                if (cpus->get(CpuId{i})->online()) (void)flush.targets_.insert(CpuId{i});
        }
        flush.pending_ = flush.targets_;
        flush.done_.initialize(flush.pending_.size());
        if (!flush.pending_.empty()) {
            flush.next_ = flushes;
            flush.link_ = &flushes;
            if (flushes) flushes->link_ = &flush.next_;
            flushes = &flush;
            if (flush.pending_.contains(local)) {
                flush.fence(Cpu::local());
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

void Flush::invalidate(va_t address, bool nonleaf) noexcept {
    libk_assert(!submitted());
    begin_ = std::min(begin_, address.raw()); end_ = std::max(end_, address.raw() + page_size);
    // Bound the receipt's fence work. A detached table invalidates non-leaf
    // walk caches, so its lifetime always requires an ASID-wide fence.
    full_ |= nonleaf || end_ - begin_ > 16 * page_size;
}

void Flush::fence(Cpu* cpu) const noexcept {
    if (!cpu || global_) arch::flush_tlb_all();
    else {
        const auto slot = cpu->translations.count ? owner_->id_ & (cpu->translations.count - 1) : 0;
        auto& seen = cpu->translations.entries[slot];
        // A displaced tag cannot be used again without Root admission.
        if (seen.id == owner_->id_) {
            const auto asid = cpu->translations.count ? slot + 1 : 0;
            if (!full_ && begin_ < end_ && seen.version == version_ - 1)
                for (auto va = begin_; va < end_; va += page_size) arch::flush_tlb_page(va, asid);
            else arch::flush_tlb(asid);
            seen.version = version_;
        }
    }
    if (executable_) arch::sync_instruction_stream();
}
void Tlb::fence_local() const noexcept {
    auto* local = Cpu::local();
    if (!local) { arch::flush_tlb_all(); return; } // Native bootstrap/test edits precede CPU installation.
    auto& cpu = *local;
    const auto slot = cpu.translations.count ? id_ & (cpu.translations.count - 1) : 0;
    arch::flush_tlb(cpu.translations.count ? slot + 1 : 0);
}
void Tlb::Edit::publish_leaf(va_t address, bool nonleaf, bool executable) noexcept {
    libk_assert(owner_);
    const auto version = owner_->version_.fetch_add(1, std::memory_order_release) + 1;
    libk_assert(version);
    const auto epoch = executable ? owner_->executable_.fetch_add(1, std::memory_order_release) + 1
                                  : owner_->executable_.load(std::memory_order_acquire);
    auto* local = Cpu::local();
    if (!local) {
        arch::flush_tlb_all();
        if (executable) arch::sync_instruction_stream();
        abort(); return;
    }
    auto& cpu = *local;
    const auto slot = cpu.translations.count ? owner_->id_ & (cpu.translations.count - 1) : 0;
    const auto asid = cpu.translations.count ? slot + 1 : 0;
    auto& seen = cpu.translations.entries[slot];
    if (nonleaf || seen.id != owner_->id_ || seen.version != version - 1) arch::flush_tlb(asid);
    else arch::flush_tlb_page(address.raw(), asid);
    if (executable) arch::sync_instruction_stream();
    if (seen.id == owner_->id_) { seen.version = version; if (executable) seen.executable = epoch; }
    abort();
}

void Tlb::Edit::publish_local() noexcept {
    // Stacks are consumed after Root admission. Shared KMaps additionally
    // complete a remote architectural fence before exposing their address.
    libk_assert(owner_);
    libk_assert(owner_->version_.fetch_add(1, std::memory_order_release) != ~u64{});
    arch::flush_tlb_all();
    abort();
}

Tlb::~Tlb() noexcept { libk_assert(active_.empty() && retained_.load(std::memory_order_acquire) == 0); }
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
    auto* outgoing = cpu.tlb;
    sync::Lock guard{state_->lock_};
    if (outgoing != state_) libk_assert(state_->active_.insert(cpu.id));
    auto& cache = cpu.translations;
    const auto slot = cache.count ? state_->id_ & (cache.count - 1) : 0;
    const auto asid = cache.count ? slot + 1 : 0;
    auto& seen = cache.entries[slot];
    const auto version = state_->version_.load(std::memory_order_acquire);
    const auto executable = state_->executable_.load(std::memory_order_acquire);
    const auto shared = shared_->version_.load(std::memory_order_acquire);
    if (outgoing != state_) arch::load_root(arch::tagged_root(root_, asid));
    else libk_assert(cpu.root == root_);
    if (cache.kernel_version != shared || (!cache.count && outgoing != state_)) arch::flush_tlb_all();
    else if (seen.id != state_->id_ || seen.version != version) arch::flush_tlb(asid);
    if (seen.id != state_->id_ || seen.executable != executable) arch::sync_instruction_stream();
    seen = {state_->id_, version, executable};
    cache.kernel_version = shared;
    cpu.tlb = state_; cpu.root = root_;
    guard.restore();
    if (outgoing && outgoing != state_) outgoing->leave(cpu.id);
}

void Root::sync(Cpu& cpu) const noexcept {
    const auto slot = cpu.translations.count ? state_->id_ & (cpu.translations.count - 1) : 0;
    auto& seen = cpu.translations.entries[slot];
    const auto epoch = state_->executable_.load(std::memory_order_acquire);
    if (seen.id != state_->id_ || seen.executable != epoch) {
        arch::sync_instruction_stream();
        if (seen.id == state_->id_) seen.executable = epoch;
    }
}

void Root::adopt(Cpu &cpu) const noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(!cpu.tlb && !cpu.root && arch::root_active(root_));
    cpu.translations.count = std::bit_floor(std::min(arch::asid_mask(), cpu.translations.entries.size()));
    activate(cpu);
}

void drain_tlb(CpuId cpu) noexcept {
    libk_assert(!arch::interrupts_enabled());
    sync::Lock lock{flush_lock};
    for (auto *p = flushes; p;) {
        auto &flush = *p;
        p = p->next_;
        if (!flush.pending_.contains(cpu)) continue;
        flush.fence(Cpu::local());
        // No dereference follows ack: its owner may now release the request.
        trace::emit(trace::Event::Complete, cpu.raw, reinterpret_cast<u64>(&flush));
        flush.ack(cpu);
    }
}
} // namespace mm
