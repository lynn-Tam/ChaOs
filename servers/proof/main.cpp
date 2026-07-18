#include <servers/proof/protocol.hpp>
#include <user/lib/context.hpp>
#include <user/lib/syscall.hpp>
#include <uapi/bootstrap.h>

namespace {

constexpr myos_word_t ProofAddress = 0x600000;
using namespace myos::proof;

[[nodiscard]] auto valid(
    const myos_bootstrap_info* bootstrap,
    myos_word_t size) noexcept -> bool {
    return bootstrap != nullptr
        && size >= sizeof(myos_bootstrap_info)
        && bootstrap->magic == MYOS_BOOTSTRAP_MAGIC
        && bootstrap->major == MYOS_BOOTSTRAP_MAJOR
        && bootstrap->minor >= MYOS_BOOTSTRAP_MINOR
        && bootstrap->size == sizeof(myos_bootstrap_info)
        && bootstrap->cap_count <= MYOS_BOOTSTRAP_MAX_CAPS
        && bootstrap->stack_size >= PageSize;
}

[[nodiscard]] auto capability(
    const myos_bootstrap_info& bootstrap,
    uint32_t kind) noexcept -> myos_cap_t {
    for (uint32_t index = 0; index < bootstrap.cap_count; ++index) {
        if (bootstrap.caps[index].kind == kind) {
            return bootstrap.caps[index].handle;
        }
    }
    return 0;
}

[[nodiscard]] auto completed(myos::SysResult result) noexcept -> bool {
    return result.status == MYOS_STATUS_OK
        || result.status == MYOS_STATUS_PENDING;
}

[[noreturn]] void fail() noexcept {
    (void)*reinterpret_cast<volatile const myos_word_t*>(0x1000);
    myos::exit();
}

[[noreturn]] void target_task(
    myos_word_t notification,
    myos_word_t shared_address) noexcept {
    auto* const flags = reinterpret_cast<volatile myos_word_t*>(
        shared_address);
    const auto waiting = myos::notification_wait(notification, VprocCookie);
    if (waiting.status != MYOS_STATUS_PENDING || waiting.value == 0) {
        fail();
    }
    flags[VprocKeySlot] = waiting.value;
    flags[VprocStateSlot] = VprocReady;
    const auto tunnel = myos::tunnel_open(
        flags[PoolSlot], TunnelIngressSlot, TunnelTag);
    if (tunnel.status != MYOS_STATUS_OK) {
        fail();
    }
    const auto connect = myos::cap_delegate(
        tunnel.value, flags[CSpaceSlot], MYOS_RIGHT_CONNECT);
    if (connect.status != MYOS_STATUS_OK) {
        fail();
    }
    flags[TunnelAdminSlot] = tunnel.value;
    flags[TunnelConnectSlot] = connect.value;
    auto* const control = reinterpret_cast<myos_vproc_control_page*>(
        ControlAddress + TargetVproc * VprocRuntimeStride);
    __atomic_store_n(
        &control->upcall_disable_depth, myos_word_t{1}, __ATOMIC_RELEASE);
    bool enabled{};
    for (;;) {
        __atomic_add_fetch(
            &flags[TunnelHeartbeatSlot], myos_word_t{1}, __ATOMIC_RELAXED);
        if (!enabled
            && flags[TunnelSourceStateSlot] == TunnelInvoked) {
            enabled = true;
            __atomic_store_n(
                &control->upcall_disable_depth,
                myos_word_t{0},
                __ATOMIC_RELEASE);
            if (myos::vproc_checkpoint().status != MYOS_STATUS_OK) {
                fail();
            }
        }
    }
}

[[noreturn]] void source_task(
    myos_word_t,
    myos_word_t shared_address) noexcept {
    auto* const flags = reinterpret_cast<volatile myos_word_t*>(
        shared_address);
    flags[TunnelSourceStateSlot] = TunnelSourceReady;
    while (flags[VprocStateSlot] != (VprocComplete | VprocBadge)
        || flags[TunnelHeartbeatSlot] == 0) {
        myos::yield();
    }

    while (flags[TunnelConnectSlot] == 0) {
        myos::yield();
    }
    const auto connected = myos::tunnel_connect(
        flags[TunnelConnectSlot]);
    if (connected.status != MYOS_STATUS_OK || connected.value == 0) {
        fail();
    }
    flags[TunnelTxSlot] = connected.value;
    if (myos::tunnel_ack(connected.value, 1).status
        != MYOS_STATUS_BAD_RIGHTS) {
        fail();
    }
    const auto first = myos::tunnel_invoke(connected.value);
    const auto second = myos::tunnel_invoke(connected.value);
    if (first.status != MYOS_STATUS_OK || first.value == 0
        || second.status != MYOS_STATUS_OK
        || second.value != first.value + 1) {
        fail();
    }
    flags[TunnelSourceSequenceSlot] = second.value;
    flags[TunnelSourceStateSlot] = TunnelInvoked;
    for (;;) {
        myos::yield();
    }
}

[[noreturn]] void vproc_upcall(
    myos_word_t generation,
    myos_word_t event_address,
    myos_word_t control_address,
    myos_word_t pending_sequence) noexcept {
    auto* const flags = reinterpret_cast<volatile myos_word_t*>(
        SharedAddress);
    if (generation == 0 || pending_sequence == 0) {
        fail();
    }

    const auto* const events =
        reinterpret_cast<const myos_vproc_event_page*>(event_address);
    auto* const control =
        reinterpret_cast<myos_vproc_control_page*>(control_address);
    const uint64_t ready_mask = __atomic_load_n(
        &events->ready_mask, __ATOMIC_ACQUIRE);
    const uint64_t ingress_mask = __atomic_load_n(
        &events->ingress_mask, __ATOMIC_ACQUIRE);
    if (ready_mask != 0) {
        const myos_operation_key_t key = flags[VprocKeySlot];
        const myos_word_t slot = key & MYOS_OPERATION_SLOT_MASK;
        const auto checkpoint = myos::vproc_checkpoint();
        const auto completed = myos::operation_take(key);
        const auto stale = myos::operation_take(key);
        const auto wrong_generation = myos::vproc_return(generation + 1);
        if (key == 0 || (ready_mask & (myos_word_t{1} << slot)) == 0
            || checkpoint.status != MYOS_STATUS_OK
            || checkpoint.value < pending_sequence
            || completed.status != MYOS_STATUS_OK || completed.value == 0
            || stale.status != MYOS_STATUS_NOT_FOUND
            || wrong_generation.status != MYOS_STATUS_BUSY) {
            fail();
        }
        flags[VprocStateSlot] = VprocComplete | completed.value;
    }
    if ((ingress_mask & (uint64_t{1} << TunnelIngressSlot)) != 0) {
        const uint64_t bit = uint64_t{1} << TunnelIngressSlot;
        const uint64_t ingress = __atomic_load_n(
            &events->ingress_mask, __ATOMIC_ACQUIRE);
        const uint64_t sequence = __atomic_load_n(
            &events->ingress_sequence[TunnelIngressSlot],
            __ATOMIC_ACQUIRE);
        const myos_word_t tag = __atomic_load_n(
            &events->ingress_tag[TunnelIngressSlot], __ATOMIC_ACQUIRE);
        const myos_cap_t admin = flags[TunnelAdminSlot];
        const myos_cap_t tx = flags[TunnelTxSlot];
        if ((ingress & bit) == 0 || sequence == 0 || tag != TunnelTag
            || admin == 0 || tx == 0
            || myos::tunnel_invoke(admin).status != MYOS_STATUS_BAD_RIGHTS) {
            fail();
        }
        const auto acknowledged = myos::tunnel_ack(admin, sequence);
        const auto stale = myos::tunnel_ack(admin, sequence);
        if (acknowledged.status != MYOS_STATUS_OK
            || acknowledged.value != sequence
            || stale.status != MYOS_STATUS_RETRY) {
            fail();
        }
        flags[TunnelTargetSequenceSlot] = acknowledged.value;
        flags[TunnelTargetStateSlot] = TunnelDelivered;
    }

    for (myos_word_t index = 0; index < MYOS_VPROC_CONTEXT_WORDS; ++index) {
        control->resume.words[index] = events->delivered.words[index];
    }
    __atomic_store_n(
        &control->resume_generation, generation, __ATOMIC_RELEASE);
    (void)myos::vproc_return(generation);
    fail();
}

} // namespace

//Confirmatory experiment.
// Exit condition: replace this proof image with Stage E user-service tests once
// init can construct isolated tasks through ResourcePool capabilities.
extern "C" void myos_main(
    myos_word_t bootstrap_address,
    myos_word_t bootstrap_size,
    myos_word_t vproc_shared,
    myos_word_t vproc_magic,
    myos_word_t vproc_task_stack,
    myos_word_t) noexcept {
    if (vproc_task_stack != 0
        && (vproc_magic == VprocMagic
            || vproc_magic == SourceVprocMagic)) {
        for (;;) {
            const auto armed = myos::vproc_arm(
                bootstrap_address, bootstrap_size);
            if (armed.status == MYOS_STATUS_OK) {
                break;
            }
            if (armed.status != MYOS_STATUS_BUSY
                && armed.status != MYOS_STATUS_RETRY) {
                fail();
            }
            myos::yield();
        }
        auto* const flags = reinterpret_cast<volatile myos_word_t*>(
            SharedAddress);
        if (vproc_magic == VprocMagic) {
            myos::user_enter(
                &target_task,
                vproc_task_stack,
                flags[VprocNotificationSlot],
                SharedAddress);
        }
        myos::user_enter(
            &source_task,
            vproc_task_stack,
            0,
            SharedAddress);
    }
    if (bootstrap_address != 0 && bootstrap_size != 0
        && vproc_shared != 0) {
        vproc_upcall(
            bootstrap_address, bootstrap_size, vproc_shared, vproc_magic);
    }
    const auto* const bootstrap =
        reinterpret_cast<const myos_bootstrap_info*>(bootstrap_address);
    if (!valid(bootstrap, bootstrap_size)) {
        //Confirmatory experiment.
        // Exit condition: the Stage E1 child proof is replaced by a real
        // service protocol after Endpoint IPC exists. The registered start
        // descriptor passes a bounded shared result page and lane index.
        if (bootstrap_address >= 64 * 1024 && bootstrap_size < 2) {
            auto* const flags = reinterpret_cast<volatile myos_word_t*>(
                bootstrap_address);
            const myos_cap_t notification = flags[NotificationSlot];
            if (notification == 0
                || myos::notification_signal(notification).status
                    != MYOS_STATUS_OK) {
                fail();
            }
            flags[bootstrap_size] = ChildReady + bootstrap_size;
            for (;;) {
                myos::yield();
            }
        }
        fail();
    }
    const myos_cap_t vspace = capability(
        *bootstrap, MYOS_BOOTSTRAP_CAP_VSPACE);
    const myos_cap_t bundle = capability(
        *bootstrap, MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE);
    if (vspace == 0 || bundle == 0) {
        fail();
    }

    myos::yield();

    const auto region = myos::vm_create_region(
        vspace,
        ProofAddress,
        PageSize,
        MYOS_VM_READ,
        MYOS_VM_NORMAL,
        MYOS_RIGHT_MAP);
    if (region.status != MYOS_STATUS_OK
        || !completed(myos::vm_map(
            region.value,
            bundle,
            ProofAddress,
            PageSize,
            0,
            MYOS_VM_READ))
        || myos::cap_revoke(bundle, true).status != MYOS_STATUS_OK
        || !completed(myos::vm_protect(
            vspace,
            bootstrap->stack_base,
            PageSize,
            MYOS_VM_READ | MYOS_VM_WRITE))) {
        fail();
    }

    // The low guard is deliberately unmapped. Reaching it proves that the
    // original TrapFrame survived yield and the blocking revoke continuation.
    fail();
}
