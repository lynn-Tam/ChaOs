#include <test/test.hpp>

#include <core/kernel_image.hpp>
#include <mm/virtual_layout.hpp>
#include <arch/user.hpp>
#include <cap/handle.hpp>
#include <uapi/capability.h>
#include <uapi/status.h>
#include <uapi/syscall.h>
#include <uapi/vm.h>

namespace {

bool test_user_start_validates_privilege_inputs(const TestContext&) noexcept {
    const arch::UserStart valid{
        .entry = kernel::mm::VirtAddr{kernel::mm::layout::LowGuardEnd},
        .stack = kernel::mm::VirtAddr{kernel::mm::layout::UserEnd},
    };
    arch::UserStart low = valid;
    low.entry = kernel::mm::VirtAddr{kernel::mm::layout::LowGuardEnd - 2};
    arch::UserStart odd = valid;
    odd.entry = kernel::mm::VirtAddr{kernel::mm::layout::LowGuardEnd + 1};
    arch::UserStart kernel = valid;
    kernel.entry = kernel::image::virtual_begin();
    arch::UserStart unaligned_stack = valid;
    unaligned_stack.stack = kernel::mm::VirtAddr{kernel::mm::layout::UserEnd - 1};
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
        .entry = kernel::mm::VirtAddr{kernel::mm::layout::LowGuardEnd},
        .stack = kernel::mm::VirtAddr{kernel::mm::layout::LowGuardEnd + kernel::mm::page_size},
        .arguments = {1, 2, 3, 4, 5, 6},
    };
    auto prepared = arch::prepare_user_stack(top, valid);
    auto rejected = arch::prepare_user_stack(
        top,
        arch::UserStart{
            .entry = kernel::image::virtual_begin(),
            .stack = valid.stack,
        });
    return prepared && *prepared >= reinterpret_cast<usize>(home)
        && *prepared < top && (*prepared & 0xfU) == 0 && !rejected;
}

bool test_uapi_values_are_stable_and_not_internal_pointers(
    const TestContext&) noexcept {
    static_assert(sizeof(kernel::cap::CapHandle) == sizeof(myos_cap_t));
    static_assert(MYOS_SYS_YIELD != MYOS_SYS_EXIT);
    static_assert(MYOS_SYS_VM_MAP != MYOS_SYS_VM_PROTECT);
    static_assert(MYOS_RIGHT_REVOKE == (UINT64_C(1) << 11));
    static_assert((MYOS_VM_WRITE & MYOS_VM_READ) == 0);
    static_assert(MYOS_STATUS_OK == 0 && MYOS_STATUS_INVALID_CAP == -1);
    static_assert(MYOS_STATUS_BUSY == -7 && MYOS_STATUS_PENDING == -9);
    return !kernel::cap::CapHandle::from_raw(0)
        && !kernel::cap::CapHandle::from_raw(1);
}

} // namespace

void register_user_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "user",
        "UserStart rejects privilege and canonical-address forgery",
        test_user_start_validates_privilege_inputs);
    (void)registry.add(
        "user",
        "synthetic first frame lives only on the Thread home stack",
        test_synthetic_user_frame_consumes_home_stack_only);
    (void)registry.add(
        "user",
        "register ABI values remain explicit UAPI data",
        test_uapi_values_are_stable_and_not_internal_pointers);
}
