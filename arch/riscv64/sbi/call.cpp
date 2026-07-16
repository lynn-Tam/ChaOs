#include "arch/riscv64/sbi/call.hpp"

namespace arch::riscv64::sbi {

auto call(
    usize extension,
    usize function,
    usize arg0,
    usize arg1,
    usize arg2) noexcept -> Ret {
    register isize a0 asm("a0") = static_cast<isize>(arg0);
    register usize a1 asm("a1") = arg1;
    register usize a2 asm("a2") = arg2;
    register usize a6 asm("a6") = function;
    register usize a7 asm("a7") = extension;

    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a2), "r"(a6), "r"(a7)
        : "memory");
    return Ret{a0, a1};
}

} // namespace arch::riscv64::sbi
