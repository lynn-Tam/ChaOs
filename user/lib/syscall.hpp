#pragma once

#include <uapi/capability.h>
#include <uapi/resource.h>
#include <uapi/status.h>
#include <uapi/syscall.h>
#include <uapi/types.h>
#include <uapi/thread.h>
#include <uapi/tunnel.h>
#include <uapi/vproc.h>
#include <uapi/vm.h>

namespace myos {

struct SysResult final {
    myos_status_t status{};
    myos_word_t value{};
};

[[nodiscard]] inline auto syscall(
    myos_word_t operation,
    myos_word_t arg0 = 0,
    myos_word_t arg1 = 0,
    myos_word_t arg2 = 0,
    myos_word_t arg3 = 0,
    myos_word_t arg4 = 0,
    myos_word_t arg5 = 0) noexcept -> SysResult {
    register myos_word_t a0 asm("a0") = arg0;
    register myos_word_t a1 asm("a1") = arg1;
    register myos_word_t a2 asm("a2") = arg2;
    register myos_word_t a3 asm("a3") = arg3;
    register myos_word_t a4 asm("a4") = arg4;
    register myos_word_t a5 asm("a5") = arg5;
    register myos_word_t a7 asm("a7") = operation;
    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
        : "memory");
    return SysResult{
        .status = static_cast<myos_status_t>(a0),
        .value = a1,
    };
}

inline void yield() noexcept {
    (void)syscall(MYOS_SYS_YIELD);
}

[[nodiscard]] inline auto sc_bind(
    myos_cap_t context,
    myos_cap_t thread) noexcept -> SysResult {
    return syscall(MYOS_SYS_SC_BIND, context, thread);
}

[[nodiscard]] inline auto execution_start(myos_cap_t target) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_EXECUTION_START, target);
}

[[nodiscard]] inline auto cap_close(myos_cap_t capability) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_CAP_CLOSE, capability);
}

[[nodiscard]] inline auto cap_duplicate(
    myos_cap_t source,
    myos_cap_t destination_cspace,
    myos_word_t rights) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_CAP_DUPLICATE, source, destination_cspace, rights);
}

[[nodiscard]] inline auto cap_delegate(
    myos_cap_t source,
    myos_cap_t destination_cspace,
    myos_word_t rights) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_CAP_DELEGATE, source, destination_cspace, rights);
}

[[nodiscard]] inline auto cap_move(
    myos_cap_t source,
    myos_cap_t destination_cspace) noexcept -> SysResult {
    return syscall(MYOS_SYS_CAP_MOVE, source, destination_cspace);
}

[[nodiscard]] inline auto object_destroy(myos_cap_t capability) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_OBJECT_DESTROY, capability);
}

[[nodiscard]] inline auto memory_create(
    myos_cap_t pool,
    myos_word_t size,
    myos_word_t access) noexcept -> SysResult {
    return syscall(MYOS_SYS_MEMORY_CREATE, pool, size, access);
}

[[nodiscard]] inline auto memory_seal(myos_cap_t memory) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_MEMORY_SEAL, memory);
}

[[nodiscard]] inline auto resource_create_child(
    myos_cap_t pool,
    myos_word_t memory,
    myos_word_t caps,
    myos_word_t kinds) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_RESOURCE_CREATE_CHILD, pool, memory, caps, kinds);
}

[[nodiscard]] inline auto resource_close(myos_cap_t pool) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_RESOURCE_CLOSE, pool);
}

[[nodiscard]] inline auto vspace_create(myos_cap_t pool) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_VSPACE_CREATE, pool);
}

[[nodiscard]] inline auto cspace_create(
    myos_cap_t pool,
    myos_word_t slots,
    myos_word_t pages) noexcept -> SysResult {
    return syscall(MYOS_SYS_CSPACE_CREATE, pool, slots, pages);
}

[[nodiscard]] inline auto thread_create(
    myos_cap_t pool,
    myos_cap_t vspace,
    myos_cap_t cspace,
    myos_cap_t start_memory,
    myos_word_t start_offset = 0) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_THREAD_CREATE,
        pool, vspace, cspace, start_memory, start_offset);
}

[[nodiscard]] inline auto sc_create(
    myos_cap_t pool,
    myos_cap_t domain,
    myos_word_t budget_ns,
    myos_word_t period_ns,
    myos_word_t urgency,
    myos_word_t home_cpu) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_SC_CREATE,
        pool, domain, budget_ns, period_ns, urgency, home_cpu);
}

[[nodiscard]] inline auto notification_create(
    myos_cap_t pool,
    myos_word_t badge) noexcept -> SysResult {
    return syscall(MYOS_SYS_NOTIFICATION_CREATE, pool, badge);
}

[[nodiscard]] inline auto notification_signal(
    myos_cap_t notification) noexcept -> SysResult {
    return syscall(MYOS_SYS_NOTIFICATION_SIGNAL, notification);
}

[[nodiscard]] inline auto notification_take(
    myos_cap_t notification) noexcept -> SysResult {
    return syscall(MYOS_SYS_NOTIFICATION_TAKE, notification);
}

[[nodiscard]] inline auto notification_wait(
    myos_cap_t notification,
    myos_word_t cookie = 0) noexcept -> SysResult {
    return syscall(MYOS_SYS_NOTIFICATION_WAIT, notification, cookie);
}

[[nodiscard]] inline auto vproc_create(
    myos_cap_t pool,
    myos_cap_t vspace,
    myos_cap_t cspace,
    myos_cap_t start_memory,
    myos_word_t start_offset = 0) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_VPROC_CREATE,
        pool, vspace, cspace, start_memory, start_offset);
}

[[nodiscard]] inline auto vproc_return(
    myos_word_t generation) noexcept -> SysResult {
    return syscall(MYOS_SYS_VPROC_RETURN, generation);
}

[[nodiscard]] inline auto vproc_poll() noexcept -> SysResult {
    return syscall(MYOS_SYS_VPROC_POLL);
}

[[nodiscard]] inline auto tunnel_create(
    myos_cap_t pool,
    myos_cap_t source,
    myos_cap_t target,
    myos_word_t slot,
    myos_word_t tag) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_TUNNEL_CREATE,
        pool,
        source,
        target,
        slot,
        tag,
        MYOS_TUNNEL_FLAGS_NONE);
}

[[nodiscard]] inline auto tunnel_invoke(myos_cap_t tunnel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_TUNNEL_INVOKE, tunnel);
}

[[nodiscard]] inline auto tunnel_take(myos_cap_t tunnel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_TUNNEL_TAKE, tunnel);
}

[[nodiscard]] inline auto tunnel_close(myos_cap_t tunnel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_TUNNEL_CLOSE, tunnel);
}

[[nodiscard]] inline auto operation_take(
    myos_operation_key_t key) noexcept -> SysResult {
    return syscall(MYOS_SYS_OPERATION_TAKE, key);
}

[[nodiscard]] inline auto cap_revoke(
    myos_cap_t capability,
    bool include_source) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_CAP_REVOKE, capability, include_source ? 1 : 0);
}

[[nodiscard]] inline auto vm_create_region(
    myos_cap_t vspace,
    myos_word_t address,
    myos_word_t size,
    myos_word_t access,
    myos_word_t memory_types,
    myos_word_t rights) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_VM_CREATE_REGION,
        vspace, address, size, access, memory_types, rights);
}

[[nodiscard]] inline auto vm_map(
    myos_cap_t vspace,
    myos_cap_t memory,
    myos_word_t address,
    myos_word_t size,
    myos_word_t object_page,
    myos_word_t access) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_VM_MAP,
        vspace, memory, address, size, object_page, access);
}

[[nodiscard]] inline auto vm_protect(
    myos_cap_t vspace,
    myos_word_t address,
    myos_word_t size,
    myos_word_t access) noexcept -> SysResult {
    return syscall(MYOS_SYS_VM_PROTECT, vspace, address, size, access);
}

[[nodiscard]] inline auto vm_unmap(
    myos_cap_t vspace,
    myos_word_t address,
    myos_word_t size) noexcept -> SysResult {
    return syscall(MYOS_SYS_VM_UNMAP, vspace, address, size);
}

[[noreturn]] inline void exit() noexcept {
    (void)syscall(MYOS_SYS_EXIT);
    for (;;) {
        asm volatile("wfi");
    }
}

} // namespace myos
