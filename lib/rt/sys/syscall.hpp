#pragma once

#include <uapi/abi.h>
#include <uapi/cap.h>
#include <uapi/ipc.h>
#include <uapi/io.h>
#include <uapi/task.h>
#include <uapi/mem.h>



// Selectors are borrowed. Successful creates return owned local slots;
// PENDING create returns the initializing object with an OP facet (Channel
// also returns its peer). VM/IO submit instead return a separate owned Op.
// The userspace syscall ABI returns one status and two word-sized values.
// Keep this representation below deployment so raw syscall consumers do not
// inherit policy, wire parsers or mapping ownership.
struct _sys_result final {
    status_t status{};
    word_t value{};
    word_t value2{};
};

[[nodiscard]] inline auto _call(
    word_t operation,
    word_t arg0 = 0,
    word_t arg1 = 0,
    word_t arg2 = 0,
    word_t arg3 = 0,
    word_t arg4 = 0,
    word_t arg5 = 0,
    word_t arg6 = 0) noexcept -> _sys_result {
    register word_t a0 asm("a0") = arg0;
    register word_t a1 asm("a1") = arg1;
    register word_t a2 asm("a2") = arg2;
    register word_t a3 asm("a3") = arg3;
    register word_t a4 asm("a4") = arg4;
    register word_t a5 asm("a5") = arg5;
    register word_t a6 asm("a6") = arg6;
    register word_t a7 asm("a7") = operation;
    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1), "+r"(a2)
        : "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
        : "memory");
    return _sys_result{
        .status = static_cast<status_t>(a0),
        .value = a1,
        .value2 = a2,
    };
}

[[nodiscard]] inline auto _memory_create_physical(CreateAuth auth, cap_t physical, word_t address,
                                                  word_t bytes, word_t perms) noexcept -> _sys_result {
    return _call(SYS_MEMORY_CREATE_PHYSICAL, auth.quota, physical, address, bytes, perms, 0, auth.scope);
}
[[nodiscard]] inline auto _irq_controller_create(CreateAuth auth, cap_t physical, cap_t config,
                                                word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_IRQ_CONTROLLER_CREATE, auth.quota, physical, config, offset, 0, 0, auth.scope);
}
[[nodiscard]] inline auto _irq_create(CreateAuth auth, cap_t controller, word_t source) noexcept -> _sys_result {
    return _call(SYS_IRQ_CREATE, auth.quota, controller, source, 0, 0, 0, auth.scope);
}

[[nodiscard]] inline auto _iommu_create(CreateAuth auth, cap_t physical, cap_t config, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_IOMMU_CREATE, auth.quota, physical, config, offset, 0, 0, auth.scope);
}
[[nodiscard]] inline auto _io_space_create(CreateAuth auth, cap_t controller, word_t requester) noexcept -> _sys_result {
    return _call(SYS_IO_SPACE_CREATE, auth.quota, controller, requester, 0, 0, 0, auth.scope);
}
[[nodiscard]] inline auto _io_submit(CreateAuth auth, cap_t space, word_t kind, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_IO_SUBMIT, auth.quota, space, kind, offset, 0, 0, auth.scope);
}
[[nodiscard]] inline auto _io_space_state(cap_t domain) noexcept -> _sys_result { return _call(SYS_IO_SPACE_STATE, domain); }
[[nodiscard]] inline auto _iommu_state(cap_t ctl) noexcept -> _sys_result { return _call(SYS_IOMMU_STATE, ctl); }
[[nodiscard]] inline auto _io_space_close(cap_t domain) noexcept -> _sys_result { return _call(SYS_IO_SPACE_CLOSE, domain); }
inline void _yield() noexcept {
    (void)_call(SYS_YIELD);
}

[[nodiscard]] inline auto _execution_move(cap_t target, word_t cpu, word_t epoch) noexcept -> _sys_result {
    return _call(SYS_EXECUTION_MOVE, target, cpu, epoch);
}

[[nodiscard]] inline auto _sc_bind(
    cap_t context,
    cap_t thread) noexcept -> _sys_result {
    return _call(SYS_SC_BIND, context, thread);
}

[[nodiscard]] inline auto _scope_create(CreateAuth auth) noexcept -> _sys_result {
    return _call(SYS_SCOPE_CREATE, auth.quota, 0, 0, 0, 0, 0, auth.scope);
}
[[nodiscard]] inline auto _scope_close(cap_t scope) noexcept -> _sys_result { return _call(SYS_SCOPE_CLOSE, scope); }
[[nodiscard]] inline auto _scope_info(cap_t scope, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_SCOPE_INFO, scope, offset);
}

[[nodiscard]] inline auto _scope_query(cap_t scope, uint64_t object, word_t facet, word_t cursor = 0,
    word_t epoch = 0, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_SCOPE_QUERY, scope, object, facet, cursor, epoch, offset);
}
[[nodiscard]] inline auto _scope_members(cap_t scope, ScopeCursor cursor = {}, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_SCOPE_MEMBERS, scope, offset, cursor.source, cursor.version, cursor.last);
}
[[nodiscard]] inline auto _scope_adopt(cap_t object, cap_t source, cap_t destination, word_t epoch) noexcept -> _sys_result {
    return _call(SYS_SCOPE_ADOPT, object, source, destination, epoch);
}

[[nodiscard]] inline auto _object_info(cap_t target, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_OBJECT_INFO, target, offset);
}

[[nodiscard]] inline auto _object_reassign(cap_t object, cap_t old_quota, cap_t new_quota, word_t epoch,
    cap_t source = 0, cap_t destination = 0, word_t member_epoch = 0) noexcept -> _sys_result {
    return _call(SYS_OBJECT_REASSIGN, object, old_quota, new_quota, epoch, source, destination, member_epoch);
}

[[nodiscard]] inline auto _quota_info(cap_t target, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_QUOTA_INFO, target, offset);
}
[[nodiscard]] inline auto _vspace_records(cap_t target, word_t cursor = 0, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_VSPACE_RECORDS, target, offset, cursor);
}

[[nodiscard]] inline auto _vspace_info(cap_t target, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_VSPACE_INFO, target, offset);
}
[[nodiscard]] inline auto _cspace_info(cap_t target, cap_t cursor = 0, word_t offset = 0, word_t epoch = 0) noexcept -> _sys_result {
    return _call(SYS_CSPACE_INFO, target, offset, cursor, epoch);
}
[[nodiscard]] inline auto _cap_info(cap_t target, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_CAP_INFO, target, offset);
}

[[nodiscard]] inline auto _thread_info(cap_t target, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_THREAD_INFO, target, offset);
}
[[nodiscard]] inline auto _sc_info(cap_t target, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_SC_INFO, target, offset);
}
[[nodiscard]] inline auto _execution_stop(cap_t target) noexcept -> _sys_result {
    return _call(SYS_EXECUTION_STOP, target);
}
[[nodiscard]] inline auto _execution_detach(cap_t target, word_t generation) noexcept -> _sys_result {
    return _call(SYS_EXECUTION_DETACH, target, generation);
}
[[nodiscard]] inline auto _sc_configure(cap_t target, word_t budget, word_t period,
                                      word_t weight, word_t cpus) noexcept -> _sys_result {
    return _call(SYS_SC_CONFIGURE, target, budget, period, weight, cpus);
}

[[nodiscard]] inline auto _sc_unbind(cap_t context, word_t generation) noexcept -> _sys_result {
    return _call(SYS_SC_UNBIND, context, generation);
}

[[nodiscard]] inline auto _execution_set(cap_t target, bool enabled) noexcept
    -> _sys_result {
    return _call(SYS_EXECUTION_SET, target, enabled);
}

[[nodiscard]] inline auto _cap_close(cap_t capability) noexcept
    -> _sys_result {
    return _call(SYS_CAP_CLOSE, capability, 0);
}

[[nodiscard]] inline auto _cap_close(
    cap_t capability,
    cap_t destination_cspace) noexcept -> _sys_result {
    return _call(SYS_CAP_CLOSE, capability, destination_cspace);
}

[[nodiscard]] inline auto _cap_duplicate(
    cap_t source,
    cap_t destination_cspace,
    word_t rights) noexcept -> _sys_result {
    return _call(
        SYS_CAP_DUPLICATE, source, destination_cspace, rights);
}

[[nodiscard]] inline auto _cap_delegate(
    cap_t source,
    cap_t destination_cspace,
    word_t rights,
    cap_t quota = 0) noexcept -> _sys_result {
    return _call(
        SYS_CAP_DELEGATE, source, destination_cspace, rights, quota);
}

[[nodiscard]] inline auto _cap_typed_delegate(
    cap_t source,
    cap_t destination_cspace,
    cap_t descriptor_memory,
    word_t descriptor_offset = 0, cap_t quota = 0) noexcept -> _sys_result {
    return _call(
        SYS_CAP_TYPED_DELEGATE,
        source,
        destination_cspace,
        descriptor_memory,
        descriptor_offset, quota);
}

[[nodiscard]] inline auto _cap_move(
    cap_t source,
    cap_t destination_cspace) noexcept -> _sys_result {
    return _call(SYS_CAP_MOVE, source, destination_cspace);
}

[[nodiscard]] inline auto _object_destroy(cap_t capability) noexcept
    -> _sys_result {
    return _call(SYS_OBJECT_DESTROY, capability);
}

[[nodiscard]] inline auto _memory_create(
    CreateAuth auth,
    word_t size,
    word_t access) noexcept -> _sys_result {
    return _call(SYS_MEMORY_CREATE, auth.quota, size, access, 0, 0, 0, auth.scope);
}

// Data is snapshotted from the registered IPC buffer. The destination must
// remain private, writable and anonymous; no mapping/TLB teardown is needed.
[[nodiscard]] inline auto _mem_trim(cap_t mem, word_t first, word_t count) noexcept -> _sys_result {
    auto result = _call(SYS_MEM_TRIM, mem, first, count);
    if (result.status == STATUS_OK && result.value == MEM_TRIM_DIRTY) result.status = STATUS_DIRTY;
    if (result.status == STATUS_OK && result.value == MEM_TRIM_BUSY) result.status = STATUS_BUSY;
    if (result.status == STATUS_OK && result.value == MEM_TRIM_CANCELED) result.status = STATUS_CANCELED;
    return result;
}
[[nodiscard]] inline auto _mem_writeback(cap_t mem, word_t page) noexcept -> _sys_result {
    return _call(SYS_MEM_WRITEBACK, mem, page);
}

[[nodiscard]] inline auto _memory_populate(cap_t memory, word_t page) noexcept -> _sys_result {
    return _call(SYS_MEMORY_POPULATE, memory, page);
}
[[nodiscard]] inline auto _memory_write(cap_t memory, word_t offset,
    word_t ipc_offset, word_t size) noexcept -> _sys_result {
    return _call(SYS_MEMORY_WRITE, memory, offset, ipc_offset, size);
}
[[nodiscard]] inline auto _memory_read(cap_t memory, word_t offset,
    word_t ipc_offset, word_t size) noexcept -> _sys_result {
    return _call(SYS_MEMORY_READ, memory, offset, ipc_offset, size);
}

[[nodiscard]] inline auto _memory_create_pager(
    CreateAuth auth,
    word_t size,
    word_t access,
    cap_t pager,
    word_t flags = 0, word_t credits = 64) noexcept -> _sys_result {
    return _call(SYS_MEMORY_CREATE_PAGER, auth.quota, size, access, pager, flags, credits, auth.scope);
}

[[nodiscard]] inline auto _pager_create(CreateAuth auth) noexcept -> _sys_result {
    return _call(SYS_PAGER_CREATE, auth.quota, 0, 0, 0, 0, 0, auth.scope);
}

[[nodiscard]] inline auto _pager_claim(cap_t pager) noexcept -> _sys_result {
    return _call(SYS_PAGER_CLAIM, pager);
}

[[nodiscard]] inline auto _pager_complete(cap_t pager, cap_t memory, word_t id) noexcept -> _sys_result {
    return _call(SYS_PAGER_COMPLETE, pager, memory, id);
}

[[nodiscard]] inline auto _pager_fail(cap_t pager, cap_t memory, word_t id) noexcept -> _sys_result {
    return _call(SYS_PAGER_FAIL, pager, memory, id);
}

[[nodiscard]] inline auto _pager_requeue(cap_t pager, word_t id) noexcept -> _sys_result {
    return _call(SYS_PAGER_REQUEUE, pager, id);
}

[[nodiscard]] inline auto _pager_supply(cap_t pager, cap_t memory, cap_t staging,
    word_t page, word_t id) noexcept -> _sys_result {
    return _call(SYS_PAGER_SUPPLY, pager, memory, staging, page, id);
}

[[nodiscard]] inline auto _irq_set(cap_t irq, bool enabled) noexcept -> _sys_result {
    return _call(SYS_IRQ_SET, irq, enabled);
}
[[nodiscard]] inline auto _irq_observe(cap_t irq) noexcept -> _sys_result {
    return _call(SYS_IRQ_OBSERVE, irq);
}

[[nodiscard]] inline auto _irq_ack(
    cap_t irq,
    word_t generation,
    word_t sequence) noexcept -> _sys_result {
    return _call(SYS_IRQ_ACK, irq, generation, sequence);
}

[[nodiscard]] inline auto _exit_query(cap_t target) noexcept
    -> _sys_result {
    return _call(SYS_EXIT_QUERY, target);
}

[[nodiscard]] inline auto _memory_seal(cap_t memory) noexcept
    -> _sys_result {
    return _call(SYS_MEMORY_SEAL, memory);
}

[[nodiscard]] inline auto _quota_create(
    CreateAuth auth,
    word_t memory,
    word_t caps,
    word_t kinds, word_t max_memory = 0, word_t max_caps = 0) noexcept -> _sys_result {
    return _call(SYS_QUOTA_CREATE, auth.quota, memory, caps, kinds, max_memory, max_caps, auth.scope);
}

[[nodiscard]] inline auto _quota_resize(cap_t quota, word_t memory, word_t caps, cap_t source = 0) noexcept -> _sys_result {
    return _call(SYS_QUOTA_RESIZE, quota, memory, caps, source);
}

[[nodiscard]] inline auto _quota_close(cap_t pool) noexcept
    -> _sys_result {
    return _call(SYS_QUOTA_CLOSE, pool);
}

[[nodiscard]] inline auto _vspace_create(CreateAuth auth) noexcept
    -> _sys_result {
    return _call(SYS_VSPACE_CREATE, auth.quota, 0, 0, 0, 0, 0, auth.scope);
}

[[nodiscard]] inline auto _cspace_create(
    CreateAuth auth,
    word_t slots,
    word_t pages) noexcept -> _sys_result {
    return _call(SYS_CSPACE_CREATE, auth.quota, slots, pages, 0, 0, 0, auth.scope);
}

[[nodiscard]] inline auto _thread_create(
    CreateAuth auth,
    cap_t vspace,
    cap_t cspace,
    cap_t start_memory,
    word_t start_offset, word_t home_cpu) noexcept -> _sys_result {
    return _call(SYS_THREAD_CREATE, auth.quota, vspace, cspace, start_memory, start_offset, home_cpu, auth.scope);
}

[[nodiscard]] inline auto _domain_create(CreateAuth auth, cap_t parent, word_t budget_ns,
    word_t period_ns, word_t weight, word_t cpus) noexcept -> _sys_result {
    return _call(SYS_DOMAIN_CREATE, auth.quota, parent, budget_ns, period_ns, weight, cpus, auth.scope);
}
[[nodiscard]] inline auto _domain_configure(cap_t domain, word_t budget_ns, word_t period_ns,
    word_t weight, word_t cpus, cap_t parent = 0) noexcept -> _sys_result {
    return _call(SYS_DOMAIN_CONFIGURE, domain, budget_ns, period_ns, weight, cpus, parent);
}
[[nodiscard]] inline auto _domain_execution(cap_t domain, bool enabled) noexcept -> _sys_result {
    return _call(SYS_DOMAIN_EXECUTION, domain, enabled);
}
[[nodiscard]] inline auto _domain_move(cap_t domain, cap_t parent) noexcept -> _sys_result { return _call(SYS_DOMAIN_MOVE, domain, parent); }
[[nodiscard]] inline auto _domain_close(cap_t domain) noexcept -> _sys_result { return _call(SYS_DOMAIN_CLOSE, domain); }
[[nodiscard]] inline auto _domain_info(cap_t domain, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_DOMAIN_INFO, domain, offset);
}

[[nodiscard]] inline auto _sc_create(
    CreateAuth auth,
    cap_t domain,
    word_t budget_ns,
    word_t period_ns,
    word_t weight,
    word_t cpus) noexcept -> _sys_result {
    return _call(SYS_SC_CREATE, auth.quota, domain, budget_ns, period_ns, weight, cpus, auth.scope);
}

[[nodiscard]] inline auto _notification_create(
    CreateAuth auth,
    word_t badge) noexcept -> _sys_result {
    return _call(SYS_NOTIFICATION_CREATE, auth.quota, badge, 0, 0, 0, 0, auth.scope);
}

[[nodiscard]] inline auto _notification_signal(
    cap_t notification) noexcept -> _sys_result {
    return _call(SYS_NOTIFICATION_SIGNAL, notification);
}

[[nodiscard]] inline auto _notification_take(
    cap_t notification) noexcept -> _sys_result {
    return _call(SYS_NOTIFICATION_TAKE, notification);
}

[[nodiscard]] inline auto _source_info(cap_t source, word_t facet = SOURCE_STATE) noexcept -> _sys_result {
    return _call(SYS_SOURCE_INFO, source, facet);
}
[[nodiscard]] inline auto _wait_one(cap_t source, word_t mask, word_t seen = 0,
    word_t deadline = 0, word_t flags = 0, word_t facet = SOURCE_STATE) noexcept -> _sys_result {
    return _call(SYS_WAIT_ONE, source, facet, mask, seen, deadline, flags);
}
[[nodiscard]] inline auto _port_create(CreateAuth auth, word_t watches, word_t queue) noexcept -> _sys_result {
    return _call(SYS_PORT_CREATE, auth.quota, watches, queue, 0, 0, 0, auth.scope);
}
[[nodiscard]] inline auto _port_watch(cap_t port, cap_t source, word_t mask, word_t seen,
    word_t key, word_t flags = 0, word_t facet = SOURCE_STATE) noexcept -> _sys_result {
    return _call(SYS_PORT_WATCH, port, source, facet, mask, seen, key, flags);
}
[[nodiscard]] inline auto _grant_source(cap_t scope, uint64_t cspace, cap_t seed, uint64_t selected = 0) noexcept -> _sys_result {
    return _call(SYS_GRANT_SOURCE, scope, cspace, seed, selected);
}
[[nodiscard]] inline auto _port_grant(cap_t port, cap_t scope, uint64_t cspace, cap_t seed,
    uint64_t selected, uint64_t seen, word_t key = 0) noexcept -> _sys_result {
    return _call(SYS_PORT_GRANT, scope, cspace, seed, selected, port, seen, key);
}
[[nodiscard]] inline auto _port_arm(cap_t port, word_t watch, word_t seen = 0) noexcept -> _sys_result {
    return _call(SYS_PORT_ARM, port, watch, seen);
}
[[nodiscard]] inline auto _port_unwatch(cap_t port, word_t watch) noexcept -> _sys_result {
    return _call(SYS_PORT_UNWATCH, port, watch);
}
[[nodiscard]] inline auto _port_take(cap_t port, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_PORT_TAKE, port, offset);
}
[[nodiscard]] inline auto _port_records(cap_t port, word_t offset = 0, word_t epoch = 0,
    word_t last = 0, word_t limit = 0) noexcept -> _sys_result { return _call(SYS_PORT_RECORDS, port, offset, epoch, last, limit); }
[[nodiscard]] inline auto _port_resync(cap_t port, word_t epoch) noexcept -> _sys_result {
    return _call(SYS_PORT_RESYNC, port, epoch);
}

[[nodiscard]] inline auto _channel_create(
    CreateAuth auth,
    word_t queue_capacity,
    word_t max_words,
    word_t max_caps) noexcept -> _sys_result {
    return _call(SYS_CHANNEL_CREATE, auth.quota, queue_capacity, max_words, max_caps, 0, 0, auth.scope);
}

[[nodiscard]] inline auto _channel_try_send(cap_t channel) noexcept
    -> _sys_result {
    return _call(SYS_CHANNEL_TRY_SEND, channel);
}

[[nodiscard]] inline auto _channel_try_recv(cap_t channel) noexcept
    -> _sys_result {
    return _call(SYS_CHANNEL_TRY_RECV, channel);
}

[[nodiscard]] inline auto _channel_call(cap_t endpoint, cap_t reply) noexcept -> _sys_result {
    return _call(SYS_CHANNEL_CALL, endpoint, reply);
}
[[nodiscard]] inline auto _vspace_stop(cap_t vm) noexcept -> _sys_result { return _call(SYS_VSPACE_STOP, vm); }
[[nodiscard]] inline auto _vspace_exit(cap_t vm, word_t offset = 0) noexcept -> _sys_result { return _call(SYS_VSPACE_EXIT, vm, offset); }

[[nodiscard]] inline auto _channel_drop(cap_t receiver, word_t sequence) noexcept -> _sys_result {
    return _call(SYS_CHANNEL_DROP, receiver, sequence);
}
[[nodiscard]] inline auto _channel_cancel(cap_t receiver, word_t token, bool discard = false) noexcept -> _sys_result {
    return _call(SYS_CHANNEL_CANCEL, receiver, token, discard ? CHANNEL_DISCARD : 0);
}

[[nodiscard]] inline auto _channel_close(cap_t channel) noexcept
    -> _sys_result {
    return _call(SYS_CHANNEL_CLOSE, channel);
}

[[nodiscard]] inline auto _channel_mint(
    cap_t root,
    cap_t destination_cspace,
    word_t badge,
    word_t rights) noexcept -> _sys_result {
    return _call(
        SYS_CHANNEL_MINT,
        root,
        destination_cspace,
        badge,
        rights);
}

[[nodiscard]] inline auto _clock_now() noexcept -> _sys_result {
    return _call(SYS_CLOCK_NOW);
}

[[nodiscard]] inline auto _clock_frequency() noexcept -> _sys_result {
    return _call(SYS_CLOCK_FREQUENCY);
}

[[nodiscard]] inline auto _cap_revoke(cap_t branch, cap_t space = 0, word_t flags = 0, word_t selected = 0) noexcept -> _sys_result {
    return _call(SYS_CAP_REVOKE, branch, space, flags, selected);
}

// IPC VmAlloc -> owned claim selector/base. Exact overlap is CONFLICT;
// no fitting gap or metadata capacity is EXHAUSTED. No partial claim on error.
[[nodiscard]] inline auto _vm_reserve(cap_t space, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_VM_RESERVE, space, offset);
}
[[nodiscard]] inline auto _vm_guard(cap_t region, word_t address, word_t bytes) noexcept -> _sys_result {
    return _call(SYS_VM_GUARD, region, address, bytes);
}
[[nodiscard]] inline auto _vm_slice(
    cap_t vspace,
    word_t address,
    word_t size,
    word_t access,
    word_t rights) noexcept -> _sys_result {
    return _call(
        SYS_VM_SLICE,
        vspace, address, size, access, rights);
}

[[nodiscard]] inline auto _vm_submit(CreateAuth auth, cap_t space, word_t kind, word_t offset = 0) noexcept -> _sys_result {
    return _call(SYS_VM_SUBMIT, auth.quota, space, kind, offset, 0, 0, auth.scope);
}
[[nodiscard]] inline auto _op_query(cap_t op) noexcept -> _sys_result { return _call(SYS_OP_QUERY, op); }
[[nodiscard]] inline auto _op_cancel(cap_t op) noexcept -> _sys_result { return _call(SYS_OP_CANCEL, op); }

[[noreturn]] inline void _thread_exit(
    status_t status = STATUS_OK) noexcept {
    (void)_call(
        SYS_EXIT, static_cast<word_t>(status));
    for (;;) {
        asm volatile("wfi");
    }
}
