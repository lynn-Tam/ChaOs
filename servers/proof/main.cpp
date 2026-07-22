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
    const Shared shared{shared_address};
    // Vproc has no kernel-owned blocking continuation. The common syscall
    // policy must reject Endpoint call before touching Endpoint admission.
    if (myos::endpoint_call(shared.load(EndpointSlot)).status
        != MYOS_STATUS_INVALID_OP) {
        fail();
    }
    const auto bound = myos::notification_bind_vproc(
        notification, VprocNotificationIngress, VprocNotificationTag);
    if (bound.status != MYOS_STATUS_OK) {
        fail();
    }
    shared.store(VprocStateSlot, VprocReady);
    const auto tunnel = myos::tunnel_open(
        shared.load(PoolSlot), TunnelIngressSlot, TunnelTag);
    if (tunnel.status != MYOS_STATUS_OK) {
        fail();
    }
    const auto connect = myos::cap_delegate(
        tunnel.value, shared.load(CSpaceSlot), MYOS_RIGHT_CONNECT);
    if (connect.status != MYOS_STATUS_OK) {
        fail();
    }
    shared.store(TunnelAdminSlot, tunnel.value);
    shared.store(TunnelConnectSlot, connect.value);
    auto* const control = reinterpret_cast<myos_vproc_control_page*>(
        ControlAddress + TargetVproc * VprocRuntimeStride);

    // First let the ordinary Notification activation complete.  The runtime
    // remains interruptible while bootstrapping its ingress relations.
    while (shared.load(VprocStateSlot) != (VprocComplete | VprocBadge)) {
        static_cast<void>(shared.add_relaxed(TunnelHeartbeatSlot));
        myos::yield();
    }

    // Producer-before-park: take an empty checkpoint, prevent upcalls, then
    // let the source publish.  Park must reject the stale observation instead
    // of losing the already-published level.
    libk::AtomicRef{control->upcall_disable_depth}
        .store<libk::MemoryOrder::Release>(1);
    const auto observed = myos::vproc_checkpoint();
    if (observed.status != MYOS_STATUS_OK) {
        fail();
    }
    shared.store(ParkObservedSlot, observed.value);
    shared.store(ParkProbeSlot, TunnelFirstReady);
    while (shared.load(TunnelSourceStateSlot) != TunnelFirstInvoked) {
        static_cast<void>(shared.add_relaxed(TunnelHeartbeatSlot));
        myos::yield();
    }
    const auto rejected = myos::vproc_park(observed.value);
    if (rejected.status != MYOS_STATUS_BUSY) {
        fail();
    }
    shared.store(ParkResultSlot, ParkRejected);
    libk::AtomicRef{control->upcall_disable_depth}
        .store<libk::MemoryOrder::Release>(0);
    if (myos::vproc_checkpoint().status != MYOS_STATUS_OK) {
        fail();
    }
    while (shared.load(TunnelDeliveryCountSlot) < 1) {
        myos::yield();
    }

    // Park-before-producer: publishing the ready marker and entering the park
    // syscall are adjacent.  On one hart the ordering is exact; on SMP the
    // source yields before publishing so the target can commit Parked.  The
    // syscall returns only after the retained activation makes the lane ready.
    libk::AtomicRef{control->upcall_disable_depth}
        .store<libk::MemoryOrder::Release>(1);
    const auto stable = myos::vproc_checkpoint();
    if (stable.status != MYOS_STATUS_OK) {
        fail();
    }
    shared.store(ParkObservedSlot, stable.value);
    shared.store(ParkProbeSlot, TunnelSecondReady);
    const auto parked = myos::vproc_park(stable.value);
    if (parked.status != MYOS_STATUS_OK) {
        fail();
    }
    shared.store(ParkWakeSlot, ParkCommitted);
    libk::AtomicRef{control->upcall_disable_depth}
        .store<libk::MemoryOrder::Release>(0);
    if (myos::vproc_checkpoint().status != MYOS_STATUS_OK) {
        fail();
    }
    for (;;) {
        static_cast<void>(shared.add_relaxed(TunnelHeartbeatSlot));
        if (shared.load(TunnelDeliveryCountSlot) >= 2) {
            myos::yield();
        }
    }
}

[[noreturn]] void source_task(
    myos_word_t,
    myos_word_t shared_address) noexcept {
    const Shared shared{shared_address};
    shared.store(TunnelSourceStateSlot, TunnelSourceReady);
    while (shared.load(VprocStateSlot) != (VprocComplete | VprocBadge)
        || shared.load(TunnelHeartbeatSlot) == 0) {
        myos::yield();
    }

    while (shared.load(TunnelConnectSlot) == 0) {
        myos::yield();
    }
    const auto connected = myos::tunnel_connect(
        shared.load(TunnelConnectSlot));
    if (connected.status != MYOS_STATUS_OK || connected.value == 0) {
        fail();
    }
    shared.store(TunnelTxSlot, connected.value);
    if (myos::tunnel_ack(connected.value, 1).status
        != MYOS_STATUS_BAD_RIGHTS) {
        fail();
    }

    while (shared.load(ParkProbeSlot) != TunnelFirstReady) {
        myos::yield();
    }
    // Both invokes occur in one pending epoch.  Tunnel sequencing must retain
    // the second edge so that the receiver acknowledges the latest sequence.
    const auto first = myos::tunnel_invoke(connected.value);
    const auto second = myos::tunnel_invoke(connected.value);
    if (first.status != MYOS_STATUS_OK || first.value == 0
        || second.status != MYOS_STATUS_OK
        || second.value != first.value + 1) {
        fail();
    }
    shared.store(TunnelSourceSequenceSlot, second.value);
    shared.store(TunnelSourceStateSlot, TunnelFirstInvoked);
    while (shared.load(TunnelDeliveryCountSlot) < 1
        || shared.load(ParkProbeSlot) != TunnelSecondReady) {
        myos::yield();
    }

    // Give the target a scheduling boundary after it publishes the marker.
    // This is required only by the confirmatory test, not by the park ABI.
    for (myos_word_t index = 0; index < 64; ++index) {
        myos::yield();
    }
    const auto wake = myos::tunnel_invoke(connected.value);
    if (wake.status != MYOS_STATUS_OK
        || wake.value <= shared.load(TunnelSourceSequenceSlot)) {
        fail();
    }
    shared.store(TunnelSourceSequenceSlot, wake.value);
    shared.store(TunnelSourceStateSlot, TunnelSecondInvoked);
    for (;;) {
        myos::yield();
    }
}

[[noreturn]] void vproc_upcall(
    myos_word_t generation,
    myos_word_t event_address,
    myos_word_t control_address,
    myos_word_t pending_sequence) noexcept {
    const Shared shared{SharedAddress};
    if (generation == 0 || pending_sequence == 0) {
        fail();
    }

    const auto* const events =
        reinterpret_cast<const myos_vproc_event_page*>(event_address);
    auto* const control =
        reinterpret_cast<myos_vproc_control_page*>(control_address);
    const uint64_t ready_mask = libk::AtomicRef{events->ready_mask}
        .load<libk::MemoryOrder::Acquire>();
    const uint64_t ingress_mask = libk::AtomicRef{events->ingress_mask}
        .load<libk::MemoryOrder::Acquire>();
    const uint64_t notification_mask =
        libk::AtomicRef{events->notification_mask}
            .load<libk::MemoryOrder::Acquire>();
    if (ready_mask != 0) {
        fail();
    }
    if ((notification_mask
            & (uint64_t{1} << VprocNotificationIngress)) != 0) {
        const uint64_t sequence = libk::AtomicRef{
            events->notification_sequence[VprocNotificationIngress]}
                .load<libk::MemoryOrder::Acquire>();
        const myos_word_t tag = libk::AtomicRef{
            events->notification_tag[VprocNotificationIngress]}
                .load<libk::MemoryOrder::Acquire>();
        const auto completed = myos::notification_take(
            shared.load(VprocNotificationSlot));
        if (sequence == 0 || tag != VprocNotificationTag
            || completed.status != MYOS_STATUS_OK
            || completed.value != VprocBadge
            || completed.value2 != sequence) {
            fail();
        }
        shared.store(VprocKeySlot, sequence);
        shared.store(VprocStateSlot, VprocComplete | completed.value);
    }
    if ((ingress_mask & (uint64_t{1} << TunnelIngressSlot)) != 0) {
        const uint64_t bit = uint64_t{1} << TunnelIngressSlot;
        const uint64_t ingress = libk::AtomicRef{events->ingress_mask}
            .load<libk::MemoryOrder::Acquire>();
        const uint64_t sequence = libk::AtomicRef{
            events->ingress_sequence[TunnelIngressSlot]}
                .load<libk::MemoryOrder::Acquire>();
        const myos_word_t tag = libk::AtomicRef{
            events->ingress_tag[TunnelIngressSlot]}
                .load<libk::MemoryOrder::Acquire>();
        const myos_cap_t admin = shared.load(TunnelAdminSlot);
        const myos_cap_t tx = shared.load(TunnelTxSlot);
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
        shared.store(TunnelTargetSequenceSlot, acknowledged.value);
        // The acknowledged sequence is published before the count marker.
        static_cast<void>(shared.add_release(TunnelDeliveryCountSlot));
    }

    for (myos_word_t index = 0; index < MYOS_VPROC_CONTEXT_WORDS; ++index) {
        control->resume.words[index] = events->delivered.words[index];
    }
    libk::AtomicRef{control->resume_generation}
        .store<libk::MemoryOrder::Release>(generation);
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
    if (bootstrap_address == EndpointAbortMagic) {
        (void)myos::endpoint_abort(EndpointAbortDetail);
        fail();
    }
    if (bootstrap_address == EndpointTimeoutMagic) {
        for (;;) {
            asm volatile("" ::: "memory");
        }
    }
    if (bootstrap_address == EndpointFaultMagic) {
        fail();
    }
    if (bootstrap_address == EndpointMagic) {
        auto* const caps = reinterpret_cast<myos_ipc_caps*>(
            EndpointIpcAddress);
        if (caps->version != MYOS_IPC_CAPS_VERSION
            || caps->received_count != 1 || caps->received[0] == 0
            || myos::notification_signal(caps->received[0]).status
                != MYOS_STATUS_OK) {
            fail();
        }
        caps->send_count = 1;
        caps->receive_limit = 0;
        caps->send[0].source = caps->received[0];
        caps->send[0].rights = MYOS_RIGHT_SIGNAL;
        caps->send[0].operation = MYOS_CAP_COPY;
        caps->send[0].flags = 0;
        const auto replied = myos::endpoint_reply(
            MYOS_STATUS_OK, bootstrap_size + vproc_shared);
        (void)replied;
        fail();
    }
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
        const Shared shared{SharedAddress};
        if (vproc_magic == VprocMagic) {
            myos::user_enter(
                &target_task,
                vproc_task_stack,
                shared.load(VprocNotificationSlot),
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
            const Shared shared{bootstrap_address};
            const myos_cap_t notification = shared.load(NotificationSlot);
            if (notification == 0
                || myos::notification_signal(notification).status
                    != MYOS_STATUS_OK) {
                fail();
            }
            if (bootstrap_size == 0) {
                const myos_cap_t endpoint = shared.load(EndpointSlot);
                if (myos::endpoint_abort().status != MYOS_STATUS_INVALID_OP
                    || myos::endpoint_reply(MYOS_STATUS_OK).status
                        != MYOS_STATUS_INVALID_OP) {
                    fail();
                }
                auto* const caps = reinterpret_cast<myos_ipc_caps*>(
                    StackAddress);
                *caps = {};
                caps->version = MYOS_IPC_CAPS_VERSION;
                caps->send_count = 1;
                caps->receive_limit = 1;
                caps->send[0].source = notification;
                caps->send[0].rights =
                    MYOS_RIGHT_SIGNAL | MYOS_RIGHT_DUPLICATE;
                caps->send[0].operation = MYOS_CAP_DELEGATE;
                const auto called = myos::endpoint_call(
                    endpoint, EndpointMagic, 19, 23);
                if (endpoint == 0 || called.status != MYOS_STATUS_OK
                    || called.value != 42
                    || caps->received_count != 1
                    || caps->received[0] == 0
                    || myos::notification_signal(caps->received[0]).status
                        != MYOS_STATUS_OK) {
                    fail();
                }
                *caps = {};
                caps->version = MYOS_IPC_CAPS_VERSION;
                const auto aborted = myos::endpoint_call(
                    endpoint, EndpointAbortMagic, 0, 0);
                if (aborted.status != MYOS_STATUS_PEER_ABORTED
                    || aborted.value != EndpointAbortDetail) {
                    fail();
                }
                const auto timed_out = myos::endpoint_call(
                    endpoint, EndpointTimeoutMagic, 0, 0, 1'000'000);
                if (timed_out.status != MYOS_STATUS_TIMED_OUT) {
                    fail();
                }
                const auto faulted = myos::endpoint_call(
                    endpoint, EndpointFaultMagic, 0, 0);
                if (faulted.status != MYOS_STATUS_PEER_FAULT) {
                    fail();
                }
                shared.store(EndpointResultSlot, EndpointTransfer);
            }
            shared.store(bootstrap_size, ChildReady + bootstrap_size);
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
