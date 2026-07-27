#pragma once

#include <uapi/capability.h>
#include <uapi/channel.h>
#include <uapi/endpoint.h>
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
    myos_word_t value2{};
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
        : "+r"(a0), "+r"(a1), "+r"(a2)
        : "r"(a3), "r"(a4), "r"(a5), "r"(a7)
        : "memory");
    return SysResult{
        .status = static_cast<myos_status_t>(a0),
        .value = a1,
        .value2 = a2,
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

[[nodiscard]] inline auto memory_create_pager(
    myos_cap_t pool,
    myos_word_t size,
    myos_word_t access,
    myos_cap_t pager) noexcept -> SysResult {
    return syscall(MYOS_SYS_MEMORY_CREATE_PAGER, pool, size, access, pager);
}

[[nodiscard]] inline auto pager_create(
    myos_cap_t pool,
    myos_word_t backing_key,
    myos_word_t max_pages) noexcept -> SysResult {
    return syscall(MYOS_SYS_PAGER_CREATE, pool, backing_key, max_pages);
}

[[nodiscard]] inline auto irq_create(
    myos_cap_t pool,
    myos_word_t source,
    myos_word_t level = 1) noexcept -> SysResult {
    return syscall(MYOS_SYS_IRQ_CREATE, pool, source, level);
}

[[nodiscard]] inline auto pager_request(
    myos_cap_t pager,
    myos_word_t page_generation,
    myos_word_t first,
    myos_word_t count,
    myos_word_t backing_epoch,
    myos_word_t urgency = 0) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_PAGER_REQUEST,
        pager,
        page_generation,
        first,
        count,
        backing_epoch,
        urgency);
}

[[nodiscard]] inline auto pager_claim(myos_cap_t pager) noexcept -> SysResult {
    return syscall(MYOS_SYS_PAGER_CLAIM, pager);
}

[[nodiscard]] inline auto pager_complete(
    myos_cap_t pager,
    myos_word_t slot,
    myos_word_t generation) noexcept -> SysResult {
    return syscall(MYOS_SYS_PAGER_COMPLETE, pager, slot, generation);
}

[[nodiscard]] inline auto pager_fail(
    myos_cap_t pager,
    myos_word_t slot,
    myos_word_t generation) noexcept -> SysResult {
    return syscall(MYOS_SYS_PAGER_FAIL, pager, slot, generation);
}

[[nodiscard]] inline auto pager_requeue(
    myos_cap_t pager,
    myos_word_t slot,
    myos_word_t generation) noexcept -> SysResult {
    return syscall(MYOS_SYS_PAGER_REQUEUE, pager, slot, generation);
}

[[nodiscard]] inline auto pager_supply(
    myos_cap_t pager,
    myos_cap_t target_memory,
    myos_cap_t staging_memory,
    myos_word_t page,
    myos_word_t request_generation,
    myos_word_t claim_generation) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_PAGER_SUPPLY,
        pager,
        target_memory,
        staging_memory,
        page,
        request_generation,
        claim_generation);
}

[[nodiscard]] inline auto irq_bind(
    myos_cap_t irq,
    myos_cap_t notification,
    myos_word_t badge) noexcept -> SysResult {
    return syscall(MYOS_SYS_IRQ_BIND, irq, notification, badge);
}

[[nodiscard]] inline auto irq_observe(myos_cap_t irq) noexcept -> SysResult {
    return syscall(MYOS_SYS_IRQ_OBSERVE, irq);
}

[[nodiscard]] inline auto irq_ack(
    myos_cap_t irq,
    myos_word_t sequence) noexcept -> SysResult {
    return syscall(MYOS_SYS_IRQ_ACK, irq, sequence);
}

[[nodiscard]] inline auto terminal_query(myos_cap_t target) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_TERMINAL_QUERY, target);
}

[[nodiscard]] inline auto terminal_observe_bind(
    myos_cap_t target,
    myos_cap_t notification,
    myos_word_t badge) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_TERMINAL_OBSERVE_BIND, target, notification, badge);
}

[[nodiscard]] inline auto pager_bind(
    myos_cap_t pager,
    myos_cap_t notification,
    myos_word_t badge) noexcept -> SysResult {
    return syscall(MYOS_SYS_PAGER_BIND, pager, notification, badge);
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

[[nodiscard]] inline auto notification_bind_vproc(
    myos_cap_t notification,
    myos_word_t slot,
    myos_word_t tag) noexcept -> SysResult {
    return syscall(MYOS_SYS_NOTIFICATION_BIND_VPROC, notification, slot, tag);
}

[[nodiscard]] inline auto notification_unbind_vproc(
    myos_cap_t notification) noexcept -> SysResult {
    return syscall(MYOS_SYS_NOTIFICATION_UNBIND_VPROC, notification);
}

[[nodiscard]] inline auto endpoint_create(
    myos_cap_t pool,
    myos_cap_t vspace,
    myos_cap_t cspace,
    myos_cap_t descriptor_memory,
    myos_word_t descriptor_offset = 0) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_ENDPOINT_CREATE,
        pool, vspace, cspace, descriptor_memory, descriptor_offset);
}

[[nodiscard]] inline auto endpoint_mint(
    myos_cap_t endpoint,
    myos_cap_t destination_cspace,
    myos_word_t badge,
    myos_word_t cap_limit,
    myos_word_t rights) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_ENDPOINT_MINT,
        endpoint, destination_cspace, badge, cap_limit, rights);
}

[[nodiscard]] inline auto endpoint_call(
    myos_cap_t endpoint,
    myos_word_t first = 0,
    myos_word_t second = 0,
    myos_word_t third = 0,
    myos_word_t timeout_ns = 0) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_ENDPOINT_CALL,
        endpoint, first, second, third, timeout_ns);
}

[[nodiscard]] inline auto endpoint_reply(
    myos_status_t status,
    myos_word_t value = 0) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_ENDPOINT_REPLY,
        static_cast<myos_word_t>(status), value);
}

[[nodiscard]] inline auto endpoint_abort(
    myos_word_t detail = 0) noexcept -> SysResult {
    return syscall(MYOS_SYS_ENDPOINT_ABORT, detail);
}

[[nodiscard]] inline auto endpoint_close(myos_cap_t endpoint) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_ENDPOINT_CLOSE, endpoint);
}

[[nodiscard]] inline auto channel_create(
    myos_cap_t pool,
    myos_word_t queue_capacity,
    myos_word_t max_words,
    myos_word_t max_caps,
    myos_word_t relation_capacity) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_CHANNEL_CREATE,
        pool,
        queue_capacity,
        max_words,
        max_caps,
        relation_capacity);
}

[[nodiscard]] inline auto channel_try_send(myos_cap_t channel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_CHANNEL_TRY_SEND, channel);
}

[[nodiscard]] inline auto channel_try_recv(myos_cap_t channel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_CHANNEL_TRY_RECV, channel);
}

[[nodiscard]] inline auto channel_send(myos_cap_t channel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_CHANNEL_SEND, channel);
}

[[nodiscard]] inline auto channel_recv(myos_cap_t channel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_CHANNEL_RECV, channel);
}

[[nodiscard]] inline auto channel_close(myos_cap_t channel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_CHANNEL_CLOSE, channel);
}

[[nodiscard]] inline auto channel_bind(
    myos_cap_t channel,
    myos_cap_t notification,
    myos_word_t condition) noexcept -> SysResult {
    return syscall(MYOS_SYS_CHANNEL_BIND, channel, notification, condition);
}

[[nodiscard]] inline auto channel_arm(
    myos_cap_t channel,
    myos_word_t relation,
    myos_word_t observed) noexcept -> SysResult {
    return syscall(MYOS_SYS_CHANNEL_ARM, channel, relation, observed);
}

[[nodiscard]] inline auto channel_mint(
    myos_cap_t root,
    myos_cap_t destination_cspace,
    myos_word_t badge,
    myos_word_t rights) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_CHANNEL_MINT,
        root,
        destination_cspace,
        badge,
        rights);
}

[[nodiscard]] inline auto clock_now() noexcept -> SysResult {
    return syscall(MYOS_SYS_CLOCK_NOW);
}

[[nodiscard]] inline auto clock_frequency() noexcept -> SysResult {
    return syscall(MYOS_SYS_CLOCK_FREQUENCY);
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

[[nodiscard]] inline auto vproc_arm(
    myos_cap_t descriptor_memory,
    myos_word_t descriptor_offset = 0) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_VPROC_ARM, descriptor_memory, descriptor_offset);
}

[[nodiscard]] inline auto vproc_checkpoint() noexcept -> SysResult {
    return syscall(MYOS_SYS_VPROC_CHECKPOINT);
}

[[nodiscard]] inline auto vproc_park(
    myos_word_t observed_sequence) noexcept -> SysResult {
    return syscall(MYOS_SYS_VPROC_PARK, observed_sequence);
}

[[nodiscard]] inline auto tunnel_open(
    myos_cap_t pool,
    myos_word_t slot,
    myos_word_t tag) noexcept -> SysResult {
    return syscall(
        MYOS_SYS_TUNNEL_OPEN,
        pool,
        slot,
        tag,
        MYOS_TUNNEL_FLAGS_NONE);
}

[[nodiscard]] inline auto tunnel_connect(myos_cap_t connect) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_TUNNEL_CONNECT, connect);
}

[[nodiscard]] inline auto tunnel_invoke(myos_cap_t tunnel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_TUNNEL_INVOKE, tunnel);
}

[[nodiscard]] inline auto tunnel_ack(
    myos_cap_t tunnel,
    myos_word_t observed_sequence) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_TUNNEL_ACK, tunnel, observed_sequence);
}

[[nodiscard]] inline auto tunnel_close(myos_cap_t tunnel) noexcept
    -> SysResult {
    return syscall(MYOS_SYS_TUNNEL_CLOSE, tunnel);
}

[[nodiscard]] inline auto operation_poll(
    myos_operation_key_t key) noexcept -> SysResult {
    return syscall(MYOS_SYS_OPERATION_POLL, key);
}

[[nodiscard]] inline auto operation_cancel(
    myos_operation_key_t key) noexcept -> SysResult {
    return syscall(MYOS_SYS_OPERATION_CANCEL, key);
}

[[nodiscard]] inline auto operation_finish(
    myos_operation_key_t key) noexcept -> SysResult {
    return syscall(MYOS_SYS_OPERATION_FINISH, key);
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
