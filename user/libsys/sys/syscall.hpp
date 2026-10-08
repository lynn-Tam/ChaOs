#pragma once

#include <uapi/abi.h>
#include <uapi/cap.h>
#include <uapi/ipc.h>
#include <uapi/io.h>
#include <uapi/start.h>
#include <uapi/mem.h>

namespace sys {

// The userspace syscall ABI returns one status and two word-sized values.
// Keep this representation below deployment so raw syscall consumers do not
// inherit policy, wire parsers or mapping ownership.
struct SysResult final {
    status_t status{};
    word_t value{};
    word_t value2{};
};

} // namespace sys

namespace sys {

[[nodiscard]] inline auto syscall(
    word_t operation,
    word_t arg0 = 0,
    word_t arg1 = 0,
    word_t arg2 = 0,
    word_t arg3 = 0,
    word_t arg4 = 0,
    word_t arg5 = 0) noexcept -> SysResult {
    register word_t a0 asm("a0") = arg0;
    register word_t a1 asm("a1") = arg1;
    register word_t a2 asm("a2") = arg2;
    register word_t a3 asm("a3") = arg3;
    register word_t a4 asm("a4") = arg4;
    register word_t a5 asm("a5") = arg5;
    register word_t a7 asm("a7") = operation;
    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1), "+r"(a2)
        : "r"(a3), "r"(a4), "r"(a5), "r"(a7)
        : "memory");
    return SysResult{
        .status = static_cast<status_t>(a0),
        .value = a1,
        .value2 = a2,
    };
}

[[nodiscard]] inline auto io_space_create(cap_t pool) noexcept -> SysResult {
    return syscall(SYS_IO_SPACE_CREATE, pool);
}

[[nodiscard]] inline auto io_space_bind(cap_t space, cap_t device,
    cap_t memory, word_t first_page, word_t page_count,
    word_t iova) noexcept -> SysResult {
    return syscall(SYS_IO_SPACE_BIND, space, device, memory, first_page, page_count, iova);
}

[[nodiscard]] inline auto io_space_watch(cap_t space, cap_t notification,
    word_t badge) noexcept -> SysResult {
    return syscall(SYS_IO_SPACE_WATCH, space, notification, badge);
}

[[nodiscard]] inline auto io_space_state(cap_t space) noexcept -> SysResult {
    return syscall(SYS_IO_SPACE_STATE, space);
}

[[nodiscard]] inline auto io_space_reg(cap_t space, word_t index) noexcept -> SysResult {
    return syscall(SYS_IO_SPACE_REG, space, index);
}

[[nodiscard]] inline auto io_space_irq(cap_t space) noexcept -> SysResult {
    return syscall(SYS_IO_SPACE_IRQ, space);
}

[[nodiscard]] inline auto io_space_close(cap_t space) noexcept -> SysResult {
    return syscall(SYS_IO_SPACE_CLOSE, space);
}

inline void yield() noexcept {
    (void)syscall(SYS_YIELD);
}

[[nodiscard]] inline auto sc_bind(
    cap_t context,
    cap_t thread) noexcept -> SysResult {
    return syscall(SYS_SC_BIND, context, thread);
}

[[nodiscard]] inline auto execution_start(cap_t target) noexcept
    -> SysResult {
    return syscall(SYS_EXECUTION_START, target);
}

[[nodiscard]] inline auto cap_close(cap_t capability) noexcept
    -> SysResult {
    return syscall(SYS_CAP_CLOSE, capability, 0);
}

[[nodiscard]] inline auto cap_close(
    cap_t capability,
    cap_t destination_cspace) noexcept -> SysResult {
    return syscall(SYS_CAP_CLOSE, capability, destination_cspace);
}

[[nodiscard]] inline auto cap_duplicate(
    cap_t source,
    cap_t destination_cspace,
    word_t rights) noexcept -> SysResult {
    return syscall(
        SYS_CAP_DUPLICATE, source, destination_cspace, rights);
}

[[nodiscard]] inline auto cap_delegate(
    cap_t source,
    cap_t destination_cspace,
    word_t rights) noexcept -> SysResult {
    return syscall(
        SYS_CAP_DELEGATE, source, destination_cspace, rights);
}

[[nodiscard]] inline auto cap_typed_delegate(
    cap_t source,
    cap_t destination_cspace,
    cap_t descriptor_memory,
    word_t descriptor_offset = 0) noexcept -> SysResult {
    return syscall(
        SYS_CAP_TYPED_DELEGATE,
        source,
        destination_cspace,
        descriptor_memory,
        descriptor_offset);
}

[[nodiscard]] inline auto cap_move(
    cap_t source,
    cap_t destination_cspace) noexcept -> SysResult {
    return syscall(SYS_CAP_MOVE, source, destination_cspace);
}

[[nodiscard]] inline auto object_destroy(cap_t capability) noexcept
    -> SysResult {
    return syscall(SYS_OBJECT_DESTROY, capability);
}

[[nodiscard]] inline auto memory_create(
    cap_t pool,
    word_t size,
    word_t access) noexcept -> SysResult {
    return syscall(SYS_MEMORY_CREATE, pool, size, access);
}

// Data is snapshotted from the registered IPC buffer. The destination must
// remain private, writable and anonymous; no mapping/TLB teardown is needed.
[[nodiscard]] inline auto mem_trim(cap_t mem, word_t first, word_t count) noexcept -> SysResult {
    return syscall(SYS_MEM_TRIM, mem, first, count);
}
[[nodiscard]] inline auto mem_writeback(cap_t mem, word_t page) noexcept -> SysResult {
    return syscall(SYS_MEM_WRITEBACK, mem, page);
}

[[nodiscard]] inline auto memory_populate(cap_t memory, word_t page) noexcept -> SysResult {
    return syscall(SYS_MEMORY_POPULATE, memory, page);
}
[[nodiscard]] inline auto memory_write(cap_t memory, word_t offset,
    word_t ipc_offset, word_t size) noexcept -> SysResult {
    return syscall(SYS_MEMORY_WRITE, memory, offset, ipc_offset, size);
}

[[nodiscard]] inline auto memory_create_pager(
    cap_t pool,
    word_t size,
    word_t access,
    cap_t pager,
    word_t flags = 0) noexcept -> SysResult {
    return syscall(SYS_MEMORY_CREATE_PAGER, pool, size, access, pager, flags);
}

[[nodiscard]] inline auto pager_create(cap_t pool) noexcept -> SysResult {
    return syscall(SYS_PAGER_CREATE, pool);
}

[[nodiscard]] inline auto pager_claim(cap_t pager) noexcept -> SysResult {
    return syscall(SYS_PAGER_CLAIM, pager);
}

[[nodiscard]] inline auto pager_complete(cap_t pager, cap_t memory, word_t id) noexcept -> SysResult {
    return syscall(SYS_PAGER_COMPLETE, pager, memory, id);
}

[[nodiscard]] inline auto pager_fail(cap_t pager, cap_t memory, word_t id) noexcept -> SysResult {
    return syscall(SYS_PAGER_FAIL, pager, memory, id);
}

[[nodiscard]] inline auto pager_requeue(cap_t pager, word_t id) noexcept -> SysResult {
    return syscall(SYS_PAGER_REQUEUE, pager, id);
}

[[nodiscard]] inline auto pager_supply(cap_t pager, cap_t memory, cap_t staging,
    word_t page, word_t id) noexcept -> SysResult {
    return syscall(SYS_PAGER_SUPPLY, pager, memory, staging, page, id);
}

[[nodiscard]] inline auto irq_bind(
    cap_t irq,
    cap_t notification,
    word_t badge) noexcept -> SysResult {
    return syscall(SYS_IRQ_BIND, irq, notification, badge);
}

[[nodiscard]] inline auto irq_observe(cap_t irq) noexcept -> SysResult {
    return syscall(SYS_IRQ_OBSERVE, irq);
}

[[nodiscard]] inline auto irq_ack(
    cap_t irq,
    word_t generation,
    word_t sequence) noexcept -> SysResult {
    return syscall(SYS_IRQ_ACK, irq, generation, sequence);
}

[[nodiscard]] inline auto exit_query(cap_t target) noexcept
    -> SysResult {
    return syscall(SYS_EXIT_QUERY, target);
}

[[nodiscard]] inline auto exit_bind(
    cap_t target,
    cap_t notification,
    word_t badge) noexcept -> SysResult {
    return syscall(
        SYS_EXIT_BIND, target, notification, badge);
}

[[nodiscard]] inline auto pager_bind(
    cap_t pager,
    cap_t notification,
    word_t badge) noexcept -> SysResult {
    return syscall(SYS_PAGER_BIND, pager, notification, badge);
}

[[nodiscard]] inline auto memory_seal(cap_t memory) noexcept
    -> SysResult {
    return syscall(SYS_MEMORY_SEAL, memory);
}

[[nodiscard]] inline auto resource_create_child(
    cap_t pool,
    word_t memory,
    word_t caps,
    word_t kinds) noexcept -> SysResult {
    return syscall(
        SYS_RESOURCE_CREATE_CHILD, pool, memory, caps, kinds);
}

[[nodiscard]] inline auto resource_close(cap_t pool) noexcept
    -> SysResult {
    return syscall(SYS_RESOURCE_CLOSE, pool);
}

[[nodiscard]] inline auto vspace_create(cap_t pool) noexcept
    -> SysResult {
    return syscall(SYS_VSPACE_CREATE, pool);
}

[[nodiscard]] inline auto cspace_create(
    cap_t pool,
    word_t slots,
    word_t pages) noexcept -> SysResult {
    return syscall(SYS_CSPACE_CREATE, pool, slots, pages);
}

[[nodiscard]] inline auto thread_create(
    cap_t pool,
    cap_t vspace,
    cap_t cspace,
    cap_t start_memory,
    word_t start_offset = 0) noexcept -> SysResult {
    return syscall(
        SYS_THREAD_CREATE,
        pool, vspace, cspace, start_memory, start_offset);
}

[[nodiscard]] inline auto sc_create(
    cap_t pool,
    cap_t domain,
    word_t budget_ns,
    word_t period_ns,
    word_t urgency,
    word_t home_cpu) noexcept -> SysResult {
    return syscall(
        SYS_SC_CREATE,
        pool, domain, budget_ns, period_ns, urgency, home_cpu);
}

[[nodiscard]] inline auto notification_create(
    cap_t pool,
    word_t badge) noexcept -> SysResult {
    return syscall(SYS_NOTIFICATION_CREATE, pool, badge);
}

[[nodiscard]] inline auto resource_close_async(cap_t pool, cap_t events,
                                              word_t badge) noexcept -> SysResult {
    return syscall(SYS_RESOURCE_CLOSE_ASYNC, pool, events, badge);
}

[[nodiscard]] inline auto notification_signal(
    cap_t notification) noexcept -> SysResult {
    return syscall(SYS_NOTIFICATION_SIGNAL, notification);
}

[[nodiscard]] inline auto notification_take(
    cap_t notification) noexcept -> SysResult {
    return syscall(SYS_NOTIFICATION_TAKE, notification);
}

[[nodiscard]] inline auto notification_wait(
    cap_t notification,
    word_t deadline = 0) noexcept -> SysResult {
    return syscall(SYS_NOTIFICATION_WAIT, notification, deadline);
}

[[nodiscard]] inline auto endpoint_create(
    cap_t pool,
    cap_t vspace,
    cap_t cspace,
    cap_t descriptor_memory,
    word_t descriptor_offset = 0) noexcept -> SysResult {
    return syscall(
        SYS_ENDPOINT_CREATE,
        pool, vspace, cspace, descriptor_memory, descriptor_offset);
}

[[nodiscard]] inline auto endpoint_mint(
    cap_t endpoint,
    cap_t destination_cspace,
    word_t badge,
    word_t cap_limit,
    word_t rights) noexcept -> SysResult {
    return syscall(
        SYS_ENDPOINT_MINT,
        endpoint, destination_cspace, badge, cap_limit, rights);
}

[[nodiscard]] inline auto endpoint_call(
    cap_t endpoint,
    word_t first = 0,
    word_t second = 0,
    word_t third = 0,
    word_t timeout_ns = 0) noexcept -> SysResult {
    return syscall(
        SYS_ENDPOINT_CALL,
        endpoint, first, second, third, timeout_ns);
}

[[nodiscard]] inline auto endpoint_reply(
    status_t status,
    word_t value = 0) noexcept -> SysResult {
    return syscall(
        SYS_ENDPOINT_REPLY,
        static_cast<word_t>(status), value);
}

[[nodiscard]] inline auto endpoint_abort(
    word_t detail = 0) noexcept -> SysResult {
    return syscall(SYS_ENDPOINT_ABORT, detail);
}

[[nodiscard]] inline auto endpoint_close(cap_t endpoint) noexcept
    -> SysResult {
    return syscall(SYS_ENDPOINT_CLOSE, endpoint);
}

[[nodiscard]] inline auto channel_create(
    cap_t pool,
    word_t queue_capacity,
    word_t max_words,
    word_t max_caps,
    word_t relation_capacity) noexcept -> SysResult {
    return syscall(
        SYS_CHANNEL_CREATE,
        pool,
        queue_capacity,
        max_words,
        max_caps,
        relation_capacity);
}

[[nodiscard]] inline auto channel_try_send(cap_t channel) noexcept
    -> SysResult {
    return syscall(SYS_CHANNEL_TRY_SEND, channel);
}

[[nodiscard]] inline auto channel_try_recv(cap_t channel) noexcept
    -> SysResult {
    return syscall(SYS_CHANNEL_TRY_RECV, channel);
}

[[nodiscard]] inline auto channel_send(cap_t channel) noexcept
    -> SysResult {
    return syscall(SYS_CHANNEL_SEND, channel);
}

[[nodiscard]] inline auto channel_recv(cap_t channel) noexcept
    -> SysResult {
    return syscall(SYS_CHANNEL_RECV, channel);
}

[[nodiscard]] inline auto channel_close(cap_t channel) noexcept
    -> SysResult {
    return syscall(SYS_CHANNEL_CLOSE, channel);
}

[[nodiscard]] inline auto channel_bind(
    cap_t channel,
    cap_t notification,
    word_t condition) noexcept -> SysResult {
    return syscall(SYS_CHANNEL_BIND, channel, notification, condition);
}

[[nodiscard]] inline auto channel_arm(
    cap_t channel,
    word_t relation,
    word_t observed) noexcept -> SysResult {
    return syscall(SYS_CHANNEL_ARM, channel, relation, observed);
}

[[nodiscard]] inline auto channel_mint(
    cap_t root,
    cap_t destination_cspace,
    word_t badge,
    word_t rights) noexcept -> SysResult {
    return syscall(
        SYS_CHANNEL_MINT,
        root,
        destination_cspace,
        badge,
        rights);
}

[[nodiscard]] inline auto clock_now() noexcept -> SysResult {
    return syscall(SYS_CLOCK_NOW);
}

[[nodiscard]] inline auto clock_frequency() noexcept -> SysResult {
    return syscall(SYS_CLOCK_FREQUENCY);
}

[[nodiscard]] inline auto cap_revoke(
    cap_t capability,
    bool include_source) noexcept -> SysResult {
    return syscall(
        SYS_CAP_REVOKE, capability, include_source ? 1 : 0);
}

[[nodiscard]] inline auto vm_slice(
    cap_t vspace,
    word_t address,
    word_t size,
    word_t access,
    word_t rights) noexcept -> SysResult {
    return syscall(
        SYS_VM_SLICE,
        vspace, address, size, access, rights);
}

[[nodiscard]] inline auto vm_map(
    cap_t vspace,
    cap_t memory,
    word_t address,
    word_t size,
    word_t object_page,
    word_t access) noexcept -> SysResult {
    return syscall(
        SYS_VM_MAP,
        vspace, memory, address, size, object_page, access);
}

[[nodiscard]] inline auto vm_protect(
    cap_t vspace,
    word_t address,
    word_t size,
    word_t access) noexcept -> SysResult {
    return syscall(SYS_VM_PROTECT, vspace, address, size, access);
}

[[nodiscard]] inline auto vm_unmap(
    cap_t vspace,
    word_t address,
    word_t size) noexcept -> SysResult {
    return syscall(SYS_VM_UNMAP, vspace, address, size);
}

[[nodiscard]] inline auto vm_clear(cap_t region) noexcept
    -> SysResult {
    return syscall(SYS_VM_CLEAR, region);
}

[[noreturn]] inline void exit(
    status_t status = STATUS_OK) noexcept {
    (void)syscall(
        SYS_EXIT, static_cast<word_t>(status));
    for (;;) {
        asm volatile("wfi");
    }
}

} // namespace sys

namespace sys {

using UserEntry = void (*)(word_t, word_t) noexcept;

// Leaves the runtime stack and starts a user-managed continuation on the
// supplied stack.  The selected architecture provides the narrow register
// transition; a returning continuation exits the current execution target.
extern "C" [[noreturn]] void user_enter(
    UserEntry entry,
    word_t stack,
    word_t arg0,
    word_t arg1) noexcept;

} // namespace sys

namespace sys::cap {
inline void encode(
    const CapView& value,
    uint8_t (&output)[CAP_ATTENUATION_SIZE]) noexcept {
    const auto put = [&output](
        size_t offset, uint64_t field, size_t width) noexcept {
        for (size_t byte = 0; byte < width; ++byte) {
            output[offset + byte] = static_cast<uint8_t>(field >> (byte * 8));
        }
    };
    put(CAP_ATTENUATION_VERSION_OFFSET, value.version, 2);
    put(CAP_ATTENUATION_KIND_OFFSET, value.kind, 2);
    put(CAP_ATTENUATION_SIZE_OFFSET, value.size, 4);
    put(CAP_ATTENUATION_RIGHTS_OFFSET, value.rights, 8);
    for (size_t index = 0; index < 6; ++index) {
        put(CAP_ATTENUATION_WORD0_OFFSET + index * 8,
            value.words[index], 8);
    }
}

} // namespace sys::cap
