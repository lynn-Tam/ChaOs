#include "cpu.hpp"
#include "csr.hpp"
#include "pte.hpp"
#include "sbi.hpp"
#include <libk/assert.hpp>

extern "C" [[noreturn]] void ctx_entry() noexcept;

namespace arch {
Ctx::Ctx(usize top, void (*entry)(void*) noexcept, void* arg) noexcept
    : ra(reinterpret_cast<usize>(&ctx_entry)), sp(top),
      s{reinterpret_cast<usize>(entry), reinterpret_cast<usize>(arg)} {
    libk_assert(top && !(top & 15U) && entry);
}

Entry* local() noexcept { return reinterpret_cast<Entry*>(csr::Sscratch::read()); }
void install(Entry& e) noexcept {
    libk_assert(e.owner);
    csr::Sscratch::write(reinterpret_cast<usize>(&e));
}
bool interrupts_enabled() noexcept { return csr::Sstatus::is_interrupts_enabled(); }
bool disable_interrupts() noexcept {
    return (csr::Sstatus::read_and_clear_bits(csr::Sstatus::SIE) & csr::Sstatus::SIE) != 0;
}
void enable_interrupts() noexcept { csr::Sstatus::enable_interrupts(); }
void restore_interrupts(bool enabled) noexcept {
    if (enabled)
        enable_interrupts();
    else
        csr::Sstatus::disable_interrupts();
}
void wait_for_interrupt() noexcept { asm volatile("wfi" ::: "memory"); }
void sync_instruction_stream() noexcept { asm volatile("fence.i" ::: "memory"); }
bool secondary_start_available() noexcept { return sbi::probe(sbi::Ext::Hsm); }
auto start_secondary(CpuHwId hart, usize entry, usize record) noexcept -> std::expected<void, isize> {
    if (!record || (entry & 3U)) return std::unexpected(sbi::BadAddr);
    auto r = sbi::call(sbi::Ext::Hsm, 0, hart.raw, entry, record);
    if (!r) return std::unexpected(r.error());
    return {};
}
[[noreturn]] void halt_current_cpu([[maybe_unused]] HaltReason reason) noexcept {
    static_cast<void>(disable_interrupts());
    for (;;) wait_for_interrupt();
}
auto read_clock() noexcept -> time::Instant {
    u64 ticks;
    asm volatile("rdtime %0" : "=r"(ticks));
    return time::Instant::from_ticks(ticks);
}
bool timer_available() noexcept { return sbi::probe(sbi::Ext::Timer); }
auto program_timer(time::Instant deadline) noexcept -> std::expected<void, isize> {
    auto r = sbi::call(sbi::Ext::Timer, 0, deadline.ticks());
    if (!r) return std::unexpected(r.error());
    csr::Sie::enable_timer();
    return {};
}
void mask_timer() noexcept { csr::Sie::disable_timer(); }

auto ipi_available() noexcept -> bool { return sbi::probe(sbi::Ext::Ipi); }

auto send_ipi(CpuHwId target) noexcept -> std::expected<void, isize> {
    constexpr usize width = sizeof(usize) * 8;
    const usize base = target.raw & ~(width - 1);
    const auto result = sbi::call(sbi::Ext::Ipi, 0, usize{1} << (target.raw - base), base);
    if (result) return {};
    return std::unexpected(result.error());
}

void enable_ipi() noexcept { csr::Sie::enable_software(); }

void acknowledge_ipi() noexcept { csr::Sip::clear_software_pending(); }

auto pt_token(mm::Page root) noexcept -> usize {
    const auto satp = csr::Satp::try_make_sv39(root.raw());
    libk_assert(satp);
    return *satp;
}
void activate_root(usize satp) noexcept {
    libk_assert(!csr::Sstatus::is_interrupts_enabled());
    libk_assert(csr::Satp::mode(satp) == csr::Satp::MODE_SV39);
    csr::Satp::write(satp);
    flush_tlb_all();
    libk_assert(csr::Satp::read() == satp);
}
bool root_active(usize satp) noexcept {
    return csr::Satp::read() == satp;
}
void flush_tlb_all() noexcept {
    asm volatile("sfence.vma x0, x0" ::: "memory");
}
} // namespace arch
