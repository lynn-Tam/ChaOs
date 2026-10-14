#include <test/test.hpp>

#include <boot/info.hpp>
#include <mm/table.hpp>
#include <cpu.hpp>
#include <trap.hpp>

namespace {

bool test_riscv_instruction_size(const TestContext&) noexcept {
    return arch::instruction_size(0x9002) == 2 // c.ebreak
        && arch::instruction_size(0x0073) == 4; // ebreak/ecall parcel
}

bool test_user_start_validates_privilege_inputs(const TestContext&) noexcept {
    const arch::UserStart valid{
        .entry = mm::UserBegin,
        .stack = mm::UserEnd,
    };
    arch::UserStart low = valid;
    low.entry = mm::UserBegin - 2;
    arch::UserStart odd = valid;
    odd.entry = mm::UserBegin + 1;
    arch::UserStart kernel = valid;
    kernel.entry = boot_layout.va;
    arch::UserStart unaligned_stack = valid;
    unaligned_stack.stack = mm::UserEnd - 1;
    return arch::valid_user_start(valid)
        && !arch::valid_user_start(low)
        && !arch::valid_user_start(odd)
        && !arch::valid_user_start(kernel)
        && !arch::valid_user_start(unaligned_stack);
}

bool test_synthetic_user_frame_consumes_home_stack_only(
    const TestContext&) noexcept {
    alignas(16) byte home[1024]{};
    const usize top = reinterpret_cast<usize>(home) + sizeof(home);
    const arch::UserStart valid{
        .entry = mm::UserBegin,
        .stack = mm::UserBegin + mm::page_size,
        .arguments = {1, 2, 3, 4, 5, 6},
    };
    auto prepared = arch::prepare_user_stack(top, valid);
    auto rejected = arch::prepare_user_stack(
        top,
        arch::UserStart{
            .entry = boot_layout.va,
            .stack = valid.stack,
        });
    if (!prepared || *prepared < reinterpret_cast<usize>(home)
        || *prepared >= top || (*prepared & 15) != 0 || rejected) return false;
    auto& frame = *reinterpret_cast<arch::TrapFrame*>(*prepared);
    arch::TrapCtx ctx{frame};
    const auto saved = ctx.snapshot();
    for (usize i = 0; i < valid.arguments.size(); ++i)
        if (ctx.arg(i) != valid.arguments[i]) return false;
    ctx.set_result(0, 17);
    return ctx.pc() == valid.entry && frame.gpr[1] == valid.stack
        && frame.gpr[0] == 0 && frame.gpr[30] == 0 && frame.sstatus == 0x20
        && ctx.arg(0) == 17 && saved.gpr[9] == 1;

}

} // namespace

void register_user_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "user",
        "RISC-V instruction parcels distinguish compressed completion",
        test_riscv_instruction_size);
    (void)registry.add(
        "user",
        "UserStart rejects privilege and canonical-address forgery",
        test_user_start_validates_privilege_inputs);
    (void)registry.add(
        "user",
        "synthetic first frame lives only on the Thread home stack",
        test_synthetic_user_frame_consumes_home_stack_only);
}
