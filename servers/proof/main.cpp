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
    for (;;) {
        __atomic_add_fetch(
            &flags[TunnelHeartbeatSlot], myos_word_t{1}, __ATOMIC_RELAXED);
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

    const myos_cap_t signal = flags[TunnelSignalSlot];
    const myos_cap_t wait = flags[TunnelWaitSlot];
    if (signal == 0 || wait == 0
        || myos::tunnel_take(wait).status != MYOS_STATUS_DENIED) {
        fail();
    }
    const auto invoked = myos::tunnel_invoke(signal);
    if (invoked.status != MYOS_STATUS_OK || invoked.value == 0) {
        fail();
    }
    flags[TunnelSourceGenerationSlot] = invoked.value;
    flags[TunnelSourceStateSlot] = TunnelInvoked;
    for (;;) {
        myos::yield();
    }
}

[[noreturn]] void vproc_upcall(
    myos_word_t generation,
    myos_word_t event_address,
    myos_word_t control_address,
    myos_word_t ready_mask,
    myos_word_t shared_address) noexcept {
    auto* const flags = reinterpret_cast<volatile myos_word_t*>(
        shared_address);
    if (generation == 0) {
        fail();
    }

    const auto* const events =
        reinterpret_cast<const myos_vproc_event_page*>(event_address);
    auto* const control =
        reinterpret_cast<myos_vproc_control_page*>(control_address);
    if (ready_mask != 0) {
        const myos_operation_key_t key = flags[VprocKeySlot];
        const myos_word_t slot = key & MYOS_OPERATION_SLOT_MASK;
        const auto polled = myos::vproc_poll();
        const auto completed = myos::operation_take(key);
        const auto stale = myos::operation_take(key);
        const auto wrong_generation = myos::vproc_return(generation + 1);
        if (key == 0 || (ready_mask & (myos_word_t{1} << slot)) == 0
            || polled.status != MYOS_STATUS_OK || polled.value == 0
            || completed.status != MYOS_STATUS_OK || completed.value == 0
            || stale.status != MYOS_STATUS_NOT_FOUND
            || wrong_generation.status != MYOS_STATUS_BUSY) {
            fail();
        }
        flags[VprocStateSlot] = VprocComplete | completed.value;
    } else {
        const uint64_t bit = uint64_t{1} << TunnelIngressSlot;
        const uint64_t ingress = __atomic_load_n(
            &events->ingress_mask, __ATOMIC_ACQUIRE);
        const uint64_t delivery = __atomic_load_n(
            &events->ingress_generation[TunnelIngressSlot],
            __ATOMIC_ACQUIRE);
        const myos_word_t tag = __atomic_load_n(
            &events->ingress_tag[TunnelIngressSlot], __ATOMIC_ACQUIRE);
        const myos_cap_t signal = flags[TunnelSignalSlot];
        const myos_cap_t wait = flags[TunnelWaitSlot];
        if ((ingress & bit) == 0 || delivery == 0 || tag != TunnelTag
            || signal == 0 || wait == 0
            || myos::tunnel_invoke(signal).status != MYOS_STATUS_DENIED) {
            fail();
        }
        const auto taken = myos::tunnel_take(wait);
        const auto stale = myos::tunnel_take(wait);
        if (taken.status != MYOS_STATUS_OK || taken.value != delivery
            || stale.status != MYOS_STATUS_RETRY) {
            fail();
        }
        flags[TunnelTargetGenerationSlot] = taken.value;
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
    myos_word_t persistent_shared) noexcept {
    if (persistent_shared != 0 && vproc_task_stack != 0) {
        if (bootstrap_address == 0) {
            if (vproc_magic == VprocMagic) {
                myos::user_enter(
                    &target_task,
                    vproc_task_stack,
                    bootstrap_size,
                    vproc_shared);
            }
            if (vproc_magic == SourceVprocMagic) {
                myos::user_enter(
                    &source_task,
                    vproc_task_stack,
                    0,
                    vproc_shared);
            }
        }
        vproc_upcall(
            bootstrap_address,
            bootstrap_size,
            vproc_shared,
            vproc_magic,
            persistent_shared);
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
            const myos_cap_t tunnel_signal = flags[TunnelSignalSlot];
            const myos_cap_t tunnel_wait = flags[TunnelWaitSlot];
            if (notification == 0 || tunnel_signal == 0 || tunnel_wait == 0
                || myos::tunnel_invoke(tunnel_signal).status
                    != MYOS_STATUS_DENIED
                || myos::tunnel_take(tunnel_wait).status
                    != MYOS_STATUS_DENIED
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
