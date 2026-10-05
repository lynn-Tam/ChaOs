#include "pte.hpp"
#include <arch/riscv64/cpu/csr.hpp>
#include <libk/assert.hpp>

namespace arch {
auto pt_token(mm::Page root) noexcept -> usize {
    const auto satp = riscv64::Satp::try_make_sv39(root.raw());
    libk_assert(satp);
    return *satp;
}
void activate_root(usize satp) noexcept {
    libk_assert(!riscv64::Sstatus::is_interrupts_enabled());
    libk_assert(riscv64::Satp::mode(satp) == riscv64::Satp::MODE_SV39);
    riscv64::Satp::write(satp);
    flush_tlb_all();
    libk_assert(riscv64::Satp::read() == satp);
}
bool root_active(usize satp) noexcept {
    return riscv64::Satp::read() == satp;
}
void flush_tlb_all() noexcept {
    asm volatile("sfence.vma x0, x0" ::: "memory");
}
} // namespace arch
