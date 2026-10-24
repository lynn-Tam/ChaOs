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
void enable_ext_irq() noexcept { csr::Sie::enable_external(); }
void enable_interrupts() noexcept { csr::Sstatus::enable_interrupts(); }
void restore_interrupts(bool enabled) noexcept {
    if (enabled)
        enable_interrupts();
    else
        csr::Sstatus::disable_interrupts();
}
void wait_for_interrupt() noexcept { asm volatile("wfi" ::: "memory"); }
void sync_instruction_stream() noexcept { asm volatile("fence.i" ::: "memory"); }
auto start_secondary(usize hart, usize entry, usize record) noexcept -> std::expected<void, isize> {
    if (!record || (entry & 3U)) return std::unexpected(sbi::BadAddr);
    auto r = sbi::call(sbi::Ext::Hsm, 0, hart, entry, record);
    if (!r) return std::unexpected(r.error());
    return {};
}
[[noreturn]] void halt_current_cpu([[maybe_unused]] HaltReason reason) noexcept {
    static_cast<void>(disable_interrupts());
    for (;;) wait_for_interrupt();
}
bool timer_available() noexcept { return sbi::probe(sbi::Ext::Timer); }
auto program_timer(u64 deadline) noexcept -> std::expected<void, isize> {
    auto r = sbi::call(sbi::Ext::Timer, 0, deadline);
    if (!r) return std::unexpected(r.error());
    csr::Sie::enable_timer();
    return {};
}
void mask_timer() noexcept { csr::Sie::disable_timer(); }

auto ipi_available() noexcept -> bool { return sbi::probe(sbi::Ext::Ipi); }

auto send_ipi(usize target) noexcept -> std::expected<void, isize> {
    constexpr usize width = sizeof(usize) * 8;
    const usize base = target & ~(width - 1);
    const auto result = sbi::call(sbi::Ext::Ipi, 0, usize{1} << (target - base), base);
    if (result) return {};
    return std::unexpected(result.error());
}

void enable_ipi() noexcept { csr::Sie::enable_software(); }

void acknowledge_ipi() noexcept { csr::Sip::clear_software_pending(); }

auto pt_token(usize ppn) noexcept -> usize {
    const auto satp = csr::Satp::try_make_sv39(ppn);
    libk_assert(satp);
    return *satp;
}
void load_root(usize satp) noexcept {
    libk_assert(!interrupts_enabled());
    libk_assert(csr::Satp::mode(satp) == csr::Satp::MODE_SV39);
    csr::Satp::write(satp);
    libk_assert(csr::Satp::read() == satp);
}
auto tagged_root(usize satp, usize asid) noexcept -> usize { return satp | (asid << 44); }
auto asid_mask() noexcept -> usize {
    const auto root = csr::Satp::read();
    csr::Satp::write(tagged_root(root, 0xffff));
    const auto mask = (csr::Satp::read() >> 44) & 0xffff;
    csr::Satp::write(root);
    flush_tlb_all();
    return mask;
}
void flush_tlb(usize asid) noexcept {
    asm volatile("sfence.vma x0, %0" :: "r"(asid) : "memory");
}
void flush_tlb_page(usize address, usize asid) noexcept {
    asm volatile("sfence.vma %0, %1" :: "r"(address), "r"(asid) : "memory");
}
bool flush_remote(usize hart) noexcept {
    constexpr usize width = sizeof(usize) * 8;
    const usize base = hart & ~(width - 1);
    return bool(sbi::call(sbi::Ext::Rfence, 1, usize{1} << (hart - base), base, 0, 0));
}
bool root_active(usize satp) noexcept {
    return csr::Satp::read() == satp;
}
void flush_tlb_all() noexcept {
    asm volatile("sfence.vma x0, x0" ::: "memory");
}
} // namespace arch
