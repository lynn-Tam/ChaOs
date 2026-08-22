#include <servers/proof/protocol.hpp>
#include <user/lib/context.hpp>
#include <user/lib/syscall.hpp>
#include <uapi/bootstrap.h>
/*luna change: expose the registered pager descriptor to the proof worker, reason: pager_claim/supply validation is part of the userspace ABI evidence*/
#include <uapi/pager.h>

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

[[noreturn]] void channel_fail(const Shared& shared) noexcept {
    shared.store(ChannelFailureSlot, ChannelFailure);
    fail();
}

[[noreturn]] void channel_sender_task(
    myos_word_t shared_address,
    myos_word_t ipc_address) noexcept {
    const Shared shared{shared_address};
    auto* const message = reinterpret_cast<myos_channel_message*>(ipc_address);
    if (!shared || message == nullptr) {
        fail();
    }
    shared.progress(
        ProgressActor::Sender,
        ProgressStage::Boot,
        ProgressWait::ChannelReady);
    while (shared.load(ChannelReadySlot) != ChannelReady) {
        myos::yield();
    }

    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    message->transaction = 0x1001;
    message->tag = 0x5345'4e44'0001;
    message->word_count = 1;
    message->words[0] = 0x1111;
    message->cap_count = 1;
    message->caps[0].source = shared.load(ChannelNotifyRSlot);
    message->caps[0].rights = MYOS_RIGHT_SIGNAL;
    message->caps[0].operation = MYOS_CAP_COPY;
    if (myos::channel_send(shared.load(ChannelSenderSlot)).status
        != MYOS_STATUS_OK) {
        channel_fail(shared);
    }
    shared.store(ChannelFirstSentSlot, ChannelFirstSent);
    shared.progress(
        ProgressActor::Sender,
        ProgressStage::FirstSend,
        ProgressWait::FirstReceive);

    while (shared.load(ChannelFirstReceivedSlot) != ChannelFirstReceived) {
        myos::yield();
    }

    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    message->transaction = 0x2001;
    message->tag = 0x5345'4e44'0002;
    message->word_count = 1;
    message->words[0] = 0x2221;
    if (myos::channel_try_send(shared.load(ChannelSenderSlot)).status
        != MYOS_STATUS_OK) {
        channel_fail(shared);
    }
    shared.store(ChannelBlockedSlot, ChannelBlocked);
    shared.progress(
        ProgressActor::Sender,
        ProgressStage::QueueFull,
        ProgressWait::Space);

    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    message->transaction = 0x2002;
    message->tag = 0x5345'4e44'0003;
    message->word_count = 1;
    message->words[0] = 0x2222;
    if (myos::channel_send(shared.load(ChannelSenderAltSlot)).status
        != MYOS_STATUS_OK) {
        channel_fail(shared);
    }
    shared.store(ChannelSecondSentSlot, ChannelSecondSent);
    shared.progress(ProgressActor::Sender, ProgressStage::BlockingSend);

    while (shared.load(ChannelClosedSlot) != ChannelClosed) {
        myos::yield();
    }
    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    message->transaction = 0x3001;
    if (myos::channel_try_send(shared.load(ChannelSenderSlot)).status
        != MYOS_STATUS_PEER_CLOSED) {
        channel_fail(shared);
    }
    shared.store(ChannelCompleteSlot, ChannelComplete);
    shared.progress(ProgressActor::Sender, ProgressStage::Complete);
    for (;;) {
        myos::yield();
    }
}

[[noreturn]] void channel_receiver_task(
    myos_word_t shared_address,
    myos_word_t ipc_address) noexcept {
    const Shared shared{shared_address};
    auto* const message = reinterpret_cast<myos_channel_message*>(ipc_address);
    if (!shared || message == nullptr) {
        fail();
    }

    const auto readable = myos::channel_bind(
        shared.load(ChannelReceiverSlot),
        shared.load(ChannelNotifyRSlot),
        MYOS_CHANNEL_READABLE);
    const auto readable_second = myos::channel_bind(
        shared.load(ChannelReceiverSlot),
        shared.load(ChannelNotifySSlot),
        MYOS_CHANNEL_READABLE);
    if (readable.status != MYOS_STATUS_OK
        || readable_second.status != MYOS_STATUS_OK) {
        channel_fail(shared);
    }
    shared.store(ChannelRelationRSlot, readable.value);
    shared.store(ChannelRelationSSlot, readable_second.value);
    shared.progress(ProgressActor::Receiver, ProgressStage::ChannelBound);
    shared.store(ChannelReadySlot, ChannelReady);
    shared.progress(
        ProgressActor::Receiver,
        ProgressStage::ChannelBound,
        ProgressWait::FirstReceive);

    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    message->receive_limit = 1;
    if (myos::channel_recv(shared.load(ChannelReceiverSlot)).status
            != MYOS_STATUS_OK
        || message->transaction != 0x1001
        || message->tag != 0x5345'4e44'0001
        || message->word_count != 1 || message->words[0] != 0x1111
        || message->cap_count != 1 || message->received_count != 1
        || message->sender_badge != 10 || message->received[0] == 0) {
        channel_fail(shared);
    }
    shared.progress(ProgressActor::Receiver, ProgressStage::FirstReceive);
    const auto first_r = myos::notification_take(
        shared.load(ChannelNotifyRSlot));
    const auto first_s = myos::notification_take(
        shared.load(ChannelNotifySSlot));
    if (first_r.status != MYOS_STATUS_OK
        || first_r.value != ChannelNotifyRBadge
        || first_s.status != MYOS_STATUS_OK
        || first_s.value != ChannelNotifySBadge
        || myos::notification_signal(message->received[0]).status
            != MYOS_STATUS_OK
        || myos::notification_take(shared.load(ChannelNotifyRSlot)).status
            != MYOS_STATUS_OK
        || myos::channel_arm(
               shared.load(ChannelReceiverSlot),
               shared.load(ChannelRelationRSlot), message->sequence).status
            != MYOS_STATUS_OK
        || myos::channel_arm(
               shared.load(ChannelReceiverSlot),
               shared.load(ChannelRelationSSlot), message->sequence).status
            != MYOS_STATUS_OK) {
        channel_fail(shared);
    }
    shared.store(ChannelFirstReceivedSlot, ChannelFirstReceived);
    shared.progress(
        ProgressActor::Receiver,
        ProgressStage::FirstReceive,
        ProgressWait::Space);

    while (shared.load(ChannelBlockedSlot) != ChannelBlocked) {
        myos::yield();
    }
    for (myos_word_t index = 0; index < 64; ++index) {
        myos::yield();
    }
    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    if (myos::channel_recv(shared.load(ChannelReceiverSlot)).status
            != MYOS_STATUS_OK
        || message->transaction != 0x2001
        || message->sender_badge != 10
        || myos::notification_take(shared.load(ChannelNotifyRSlot)).status
            != MYOS_STATUS_OK
        || myos::notification_take(shared.load(ChannelNotifySSlot)).status
            != MYOS_STATUS_OK
        || myos::channel_arm(
               shared.load(ChannelReceiverSlot),
               shared.load(ChannelRelationRSlot), message->sequence).status
            != MYOS_STATUS_OK
        || myos::channel_arm(
               shared.load(ChannelReceiverSlot),
               shared.load(ChannelRelationSSlot), message->sequence).status
            != MYOS_STATUS_OK) {
        channel_fail(shared);
    }
    shared.progress(ProgressActor::Receiver, ProgressStage::SecondReceive);

    shared.progress(
        ProgressActor::Receiver,
        ProgressStage::SecondReceive,
        ProgressWait::Space);
    const auto next = myos::notification_wait(
        shared.load(ChannelNotifyRSlot));
    const auto next_s = myos::notification_take(
        shared.load(ChannelNotifySSlot));
    if (next.status != MYOS_STATUS_OK
        || (next.value & ChannelNotifyRBadge) == 0
        || next_s.status != MYOS_STATUS_OK
        || next_s.value != ChannelNotifySBadge) {
        channel_fail(shared);
    }
    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    if (myos::channel_try_recv(shared.load(ChannelReceiverSlot)).status
            != MYOS_STATUS_OK
        || message->transaction != 0x2002
        || message->sender_badge != 20
        || myos::channel_arm(
               shared.load(ChannelReceiverSlot),
               shared.load(ChannelRelationRSlot), message->sequence).status
            != MYOS_STATUS_OK
        || myos::channel_arm(
               shared.load(ChannelReceiverSlot),
               shared.load(ChannelRelationSSlot), message->sequence).status
            != MYOS_STATUS_OK) {
        channel_fail(shared);
    }
    shared.store(ChannelVprocBindSlot, ChannelReady);
    shared.progress(
        ProgressActor::Receiver,
        ProgressStage::BindRequested,
        ProgressWait::VprocReady);
    while (shared.load(ChannelVprocReadySlot) != ChannelVprocReady) {
        myos::yield();
    }
    shared.store(ChannelVprocGoSlot, ChannelReady);
    shared.progress(
        ProgressActor::Receiver,
        ProgressStage::GoPublished,
        ProgressWait::VprocEvent);
    while (shared.load(ChannelVprocDoneSlot) != ChannelVprocDone) {
        myos::yield();
    }
    while (shared.load(ChannelDrainSentSlot) != ChannelReady) {
        myos::yield();
    }
    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    if (myos::channel_close(shared.load(ChannelReceiverSlot)).status
            != MYOS_STATUS_OK) {
        channel_fail(shared);
    }
    if (myos::channel_try_recv(shared.load(ChannelReceiverSlot)).status
            != MYOS_STATUS_OK
        || message->transaction != 0x5001
        || message->sender_badge != 10) {
        channel_fail(shared);
    }
    shared.store(ChannelDrainReceivedSlot, ChannelReady);
    shared.store(ChannelClosedSlot, ChannelClosed);
    shared.progress(ProgressActor::Receiver, ProgressStage::Complete);
    for (;;) {
        myos::yield();
    }
}

/*luna change: service one production PageIn request from a blocking Thread, reason: the proof must cover notification admission, descriptor identity, WOULD_BLOCK re-claim, and pager_supply with a real registered IPC page*/
[[noreturn]] void pager_worker_task(
    myos_word_t shared_address,
    myos_word_t ipc_address) noexcept {
    const Shared shared{shared_address};
    auto* const request = reinterpret_cast<myos_pager_request*>(ipc_address);
    if (!shared || request == nullptr) {
        fail();
    }
    const myos_cap_t pager = shared.load(PagerCapSlot);
    const myos_cap_t target = shared.load(PagerTargetCapSlot);
    const myos_cap_t staging = shared.load(PagerSourceCapSlot);
    /*luna change: read the frozen root-role mode once per worker, reason: only pressure bundles own the extra staging remap authority*/
    const bool pressure = shared.load(RunModeSlot) == ModePressure;
    /*luna change: consume the exact staging Region authority in the worker,
      reason: only Thread's later dirty fold needs the shared Protect-only
      VSpace authority*/
    const myos_cap_t staging_region =
        shared.load(PagerStagingRegionSlot);
    const myos_cap_t notification =
        shared.load(PagerNotifyCapSlot);
    if (pager == 0 || target == 0 || staging == 0
        || notification == 0
        || (pressure && staging_region == 0)) {
        fail();
    }
    bool seed_page = true;
    /*luna change: keep one Pager worker loop for PageIn and Writeback, reason: the existing actor and ABI already carry both descriptor kinds and no parallel service state is needed*/
    for (;;) {
        shared.progress(
            ProgressActor::Pager,
            ProgressStage::Pager,
            ProgressWait::Notification);
        const auto woke = myos::notification_wait(notification);
        if (woke.status != MYOS_STATUS_OK
            || (woke.value & PagerBadge) == 0) {
            fail();
        }
        shared.store(PagerWorkerSlot, PagerWorkerQueued);
        shared.progress(
            ProgressActor::Pager,
            ProgressStage::Pager,
            ProgressWait::Pager);
        if (seed_page) {
            while (shared.load(PagerVprocSlot) != PagerVprocPending) {
                myos::yield();
            }
        }

        const auto claimed = myos::pager_claim(pager);
        if (claimed.status != MYOS_STATUS_OK
            || request->version != MYOS_PAGER_REQUEST_VERSION
            || request->flags != MYOS_PAGER_REQUEST_FLAGS_NONE
            || request->delivery_generation == 0
            || request->page_generation == 0
            || request->claim_generation == 0) {
            fail();
        }
        if (request->kind == MYOS_PAGER_REQUEST_PAGE_IN) {
            if (request->page_index != 0
                || request->payload.page_in.first != 0
                || request->payload.page_in.count != 1
                || request->payload.page_in.backing_epoch == 0) {
                fail();
            }
            request->payload.page_in.content_epoch = 1;
            shared.store(PagerWorkerSlot, PagerWorkerClaimed);
            const auto repeated = myos::pager_claim(pager);
            if (repeated.status != MYOS_STATUS_WOULD_BLOCK) {
                fail();
            }
            myos::SysResult supplied{};
            for (;;) {
                supplied = myos::pager_supply(
                    pager, target, staging, request->page_index);
                if (supplied.status == MYOS_STATUS_OK) {
                    break;
                }
                if (supplied.status != MYOS_STATUS_BUSY) {
                    fail();
                }
                myos::yield();
            }
            shared.store(PagerWorkerSlot, PagerWorkerSupplied);
            /*luna change: prepare one reusable source only in pressure mode,
              reason: the later PageIn must not allocate its source while the
              clean-pressure retry consumes the returned frame*/
            if (pressure && seed_page) {
                /*luna change: retry staging map on the formal transient VM
                  statuses, reason: PageIn continuations share VSpace claim
                  ownership and only OK/PENDING commits the next phase*/
                for (;;) {
                    const auto mapped = myos::vm_map(
                        staging_region,
                        staging,
                        StagingAddress,
                        PageSize,
                        0,
                        MYOS_VM_READ | MYOS_VM_WRITE);
                    if (completed(mapped)) {
                        break;
                    }
                    if (mapped.status != MYOS_STATUS_BUSY
                        && mapped.status != MYOS_STATUS_RETRY) {
                        fail();
                    }
                    myos::yield();
                }
                /*luna change: publish MapAccepted before staging access, reason: the worker cell must distinguish vm_map acceptance from zero-page materialization*/
                shared.store(PagerWorkerSlot, PagerWorkerMapAccepted);
                *reinterpret_cast<volatile myos_word_t*>(StagingAddress) =
                    PagerValue;
                /*luna change: publish the completed staging write before detach, reason: timeout evidence must separate source materialization from asynchronous unmap drain*/
                shared.store(PagerWorkerSlot, PagerWorkerWritten);
                /*luna change: retry exact-region unmap on the same transient
                  statuses, reason: teardown must wait for the shared VSpace
                  claim lane without weakening non-retry failures*/
                for (;;) {
                    const auto unmapped = myos::vm_unmap(
                        staging_region, StagingAddress, PageSize);
                    if (completed(unmapped)) {
                        break;
                    }
                    if (unmapped.status != MYOS_STATUS_BUSY
                        && unmapped.status != MYOS_STATUS_RETRY) {
                        fail();
                    }
                    myos::yield();
                }
                shared.store(PagerWorkerSlot, PagerWorkerPrepared);
            }
            seed_page = false;
        } else if (request->kind == MYOS_PAGER_REQUEST_WRITEBACK) {
            if (request->payload.writeback.writeback_generation == 0
                || request->payload.writeback.dirty_epoch == 0) {
                fail();
            }
            shared.store(PagerWorkerSlot, PagerWorkerWritebackClaimed);
            const auto complete = myos::pager_complete(pager, target);
            if (complete.status != MYOS_STATUS_OK) {
                fail();
            }
            shared.store(PagerWorkerSlot, PagerWorkerWritebackDone);
        } else {
            fail();
        }
        shared.progress(ProgressActor::Pager, ProgressStage::Complete);
    }
}

/*luna change: report the exact resilience worker gate through the fault
  address, reason: the contained-fault line is the only userspace console
  edge and coded gates locate the first failure without new diagnostics*/
[[noreturn]] void resilience_fail(myos_word_t code) noexcept {
    (void)*reinterpret_cast<volatile const myos_word_t*>(0x1000 + code);
    myos::exit();
}

/*luna change: service the resilience proof from two pre-created workers,
  reason: the doomed claim, the replacement generation and the stale reply
  are deployment facts rather than a runtime supervision machine*/
[[noreturn]] void resilience_worker_task(
    myos_word_t shared_address,
    myos_word_t ipc_address,
    myos_word_t peer_ipc,
    bool doomed) noexcept {
    const Shared shared{shared_address};
    auto* const request = reinterpret_cast<myos_pager_request*>(ipc_address);
    if (!shared || request == nullptr) {
        fail();
    }
    const myos_cap_t pager = shared.load(PagerCapSlot);
    const myos_cap_t target = shared.load(PagerTargetCapSlot);
    const myos_cap_t staging = shared.load(PagerSourceCapSlot);
    const myos_cap_t notification = shared.load(PagerNotifyCapSlot);
    if (pager == 0 || target == 0 || staging == 0 || notification == 0) {
        fail();
    }
    if (doomed) {
        // Worker A is the sole notification receiver: claim once, publish
        // the doomed phase, then terminate mid-claim. The kernel's terminal
        // edge must redeliver the record with an advanced claim generation.
        shared.progress(
            ProgressActor::Pager,
            ProgressStage::Pager,
            ProgressWait::Notification);
        const auto woke = myos::notification_wait(notification);
        if (woke.status != MYOS_STATUS_OK
            || (woke.value & PagerBadge) == 0) {
            resilience_fail(11);
        }
        shared.store(PagerWorkerSlot, PagerWorkerQueued);
        shared.progress(
            ProgressActor::Pager,
            ProgressStage::Pager,
            ProgressWait::Pager);
        const auto claimed = myos::pager_claim(pager);
        if (claimed.status != MYOS_STATUS_OK
            || request->version != MYOS_PAGER_REQUEST_VERSION
            || request->kind != MYOS_PAGER_REQUEST_PAGE_IN
            || request->claim_generation == 0) {
            resilience_fail(12);
        }
        request->payload.page_in.content_epoch = 1;
        shared.store(PagerWorkerSlot, PagerWorkerDoomedClaimed);
        shared.progress(
            ProgressActor::Pager,
            ProgressStage::Pager,
            ProgressWait::Pager);
        myos::exit();
    }
    const auto* const peer =
        reinterpret_cast<const myos_pager_request*>(peer_ipc);
    if (peer == nullptr) {
        resilience_fail(21);
    }
    // Worker B exists from the start but only claims after the doomed claim
    // is published, so worker A always owns the first delivery.
    while (shared.load(PagerWorkerSlot) != PagerWorkerDoomedClaimed) {
        myos::yield();
    }
    const myos_pager_request stale_descriptor = *peer;
    if (stale_descriptor.claim_generation == 0
        || stale_descriptor.kind != MYOS_PAGER_REQUEST_PAGE_IN) {
        resilience_fail(22);
    }
    shared.progress(
        ProgressActor::Pager,
        ProgressStage::Pager,
        ProgressWait::Pager);
    myos::SysResult claimed{};
    for (;;) {
        claimed = myos::pager_claim(pager);
        if (claimed.status == MYOS_STATUS_OK) {
            break;
        }
        if (claimed.status != MYOS_STATUS_WOULD_BLOCK
            && claimed.status != MYOS_STATUS_RETRY) {
            resilience_fail(23);
        }
        myos::yield();
    }
    if (request->kind != MYOS_PAGER_REQUEST_PAGE_IN
        || request->claim_generation <= stale_descriptor.claim_generation) {
        resilience_fail(24);
    }
    myos_pager_request own = *request;
    own.payload.page_in.content_epoch = 1;
    /*luna change: replay the dead worker's exact reply identity first,
      reason: the redelivered record must reject it without page effect*/
    *request = stale_descriptor;
    const auto stale = myos::pager_supply(
        pager, target, staging, stale_descriptor.page_index);
    if (stale.status == MYOS_STATUS_OK
        || stale.status == MYOS_STATUS_BUSY) {
        resilience_fail(25);
    }
    *request = own;
    for (;;) {
        const auto supplied = myos::pager_supply(
            pager, target, staging, own.page_index);
        if (supplied.status == MYOS_STATUS_OK) {
            break;
        }
        if (supplied.status != MYOS_STATUS_BUSY) {
            resilience_fail(0x600 - static_cast<int>(supplied.status));
        }
        myos::yield();
    }
    shared.store(PagerWorkerSlot, PagerWorkerSupplied);
    shared.progress(
        ProgressActor::Pager,
        ProgressStage::Pager,
        ProgressWait::Pager);
    // The second request is armed once Thread0 publishes its second fault
    // and the target Vproc runtime holds a pending FaultSlot; both fan-out
    // continuations then settle through one pager_fail.
    while (shared.load(PagerThreadSlot) != PagerThreadFaulting2
        || shared.load(PagerVprocSlot) != PagerVprocPending) {
        myos::yield();
    }
    shared.progress(
        ProgressActor::Pager,
        ProgressStage::Pager,
        ProgressWait::Pager);
    myos::SysResult second{};
    for (;;) {
        second = myos::pager_claim(pager);
        if (second.status == MYOS_STATUS_OK) {
            break;
        }
        if (second.status != MYOS_STATUS_WOULD_BLOCK
            && second.status != MYOS_STATUS_RETRY) {
            fail();
        }
        myos::yield();
    }
    if (request->kind != MYOS_PAGER_REQUEST_PAGE_IN
        || request->page_index != 1) {
        resilience_fail(27);
    }
    if (myos::pager_fail(pager, target).status != MYOS_STATUS_OK) {
        resilience_fail(28);
    }
    shared.store(PagerWorkerSlot, PagerWorkerFailed);
    shared.progress(ProgressActor::Pager, ProgressStage::Complete);
    for (;;) {
        myos::yield();
    }
}

/*luna change: drive the target Vproc through the pager_fail edge, reason:
   the FaultSlot must survive claim and drop while the stale key is
   rejected twice*/
[[noreturn]] void resilience_target_task(
    myos_word_t notification,
    myos_word_t shared_address) noexcept {
    const Shared shared{shared_address};
    (void)notification;
    if (!shared) {
        fail();
    }
    shared.progress(
        ProgressActor::TargetVproc,
        ProgressStage::Boot,
        ProgressWait::Pager);
    while (shared.load(PagerThreadSlot) != PagerThreadFaulting2) {
        myos::yield();
    }
    shared.store(PagerVprocSlot, PagerVprocFaulting);
    (void)*reinterpret_cast<volatile const myos_word_t*>(
        PagerAddress + PageSize);
    // The fault redirects into the Faulted upcall; this frame never resumes.
    fail();
}

[[noreturn]] void target_task(
    myos_word_t notification,
    myos_word_t shared_address) noexcept {
    const Shared shared{shared_address};
    /*luna change: gate the extended target fault from frozen manifest mode, reason: ordinary E1 must stop after the original Pager PageIn completion*/
    const bool pressure = shared.load(RunModeSlot) == ModePressure;
    shared.progress(
        ProgressActor::TargetVproc,
        ProgressStage::Boot,
        ProgressWait::Notification);
    if (!pressure) {
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
    }
    /*luna change: drive the target Vproc through the same Pager-backed VA as Thread0, reason: the runtime fault frame and resumed instruction must be proven on the real Vproc path*/
    while (shared.load(PagerWorkerSlot) != PagerWorkerQueued) {
        myos::yield();
    }
    shared.store(PagerVprocSlot, PagerVprocFaulting);
    /*luna change: project target Vproc fault admission and completion in its existing actor trace, reason: diagnostics must expose the fault lane without becoming its synchronization source*/
    shared.progress(
        ProgressActor::TargetVproc,
        ProgressStage::Pager,
        ProgressWait::Pager);
    const auto value = *reinterpret_cast<volatile const myos_word_t*>(
        PagerAddress);
    if (value != PagerValue) {
        fail();
    }
    shared.store(PagerVprocSlot, PagerVprocDone);
    shared.progress(
        ProgressActor::TargetVproc,
        ProgressStage::Pager,
        ProgressWait::None,
        PagerVprocDone);
    /*luna change: run the dirty-pressure edge only in pressure mode,
      reason: ordinary E1 has no stress span or writeback barrier to await*/
    if (pressure) {
        /*luna change: wait for Thread clean retry and Pager rematerialization,
          reason: the dirty Vproc fault must observe a resident, dirty Pager page
          rather than outrunning the second PageIn*/
        while (shared.load(PagerThreadSlot) != PagerThreadDirtyReady) {
            myos::yield();
        }
        /*luna change: rearm the existing Vproc fault proof lane before the
          pressure access, reason: the second retained FaultSlot must follow
          the same claim/resume protocol as the seed Pager fault*/
        shared.store(PagerVprocSlot, PagerVprocFaulting);
        shared.progress(
            ProgressActor::TargetVproc,
            ProgressStage::Pager,
            ProgressWait::Pager);
        /*luna change: drive the target Vproc through the dirty-pressure edge,
          reason: this final anonymous page follows the production Pager write and
          A/D fold rather than consuming the clean winner*/
        /*luna change: keep the dirty target at page one of the fixed Region,
          reason: Thread owns clean page zero and no remap authority is needed*/
        static_cast<void>(*reinterpret_cast<volatile const myos_word_t*>(
            StressAddress + PageSize));
        shared.store(PagerVprocSlot, PagerVprocDirtyRetryDone);
        shared.progress(
            ProgressActor::TargetVproc,
            ProgressStage::Pager,
            ProgressWait::None,
            PagerVprocDirtyRetryDone);
        /*luna change: park the pressure target at its canonical completion,
          reason: unrelated tunnel and channel setup must not allocate after
          the scenario owns every remaining PMM frame*/
        for (;;) {
            myos::yield();
        }
    }
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
    shared.progress(
        ProgressActor::TargetVproc,
        ProgressStage::BindRequested,
        ProgressWait::ChannelBind);

    // Channel readiness is a second receiver on ChannelNotifyR, not a
    // replacement for the ordinary Vproc notification.  The receiver
    // publishes the dependency first; this lane commits the binding and only
    // then releases the receiver to publish ChannelVprocGo.
    while (shared.load(ChannelVprocBindSlot) != ChannelReady) {
        myos::yield();
    }
    const auto channel_bound = myos::notification_bind_vproc(
        shared.load(ChannelNotifyRSlot),
        ChannelVprocIngress,
        ChannelVprocTag);
    if (channel_bound.status != MYOS_STATUS_OK) {
        fail();
    }
    shared.progress(ProgressActor::TargetVproc, ProgressStage::BindCommitted);
    shared.store(ChannelVprocReadySlot, ChannelVprocReady);
    shared.progress(
        ProgressActor::TargetVproc,
        ProgressStage::ReadyPublished,
        ProgressWait::Tunnel);

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
    shared.progress(
        ProgressActor::TargetVproc,
        ProgressStage::VprocDone,
        ProgressWait::VprocEvent,
        ParkRejected);
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
    shared.progress(
        ProgressActor::TargetVproc,
        ProgressStage::Complete,
        ProgressWait::VprocEvent,
        ParkCommitted);
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
    myos_word_t ipc_address,
    myos_word_t shared_address) noexcept {
    const Shared shared{shared_address};
    auto* const message = reinterpret_cast<myos_channel_message*>(ipc_address);
    if (!shared || message == nullptr) {
        fail();
    }
    shared.progress(
        ProgressActor::SourceVproc,
        ProgressStage::Boot,
        ProgressWait::VprocReady);
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

    while (shared.load(ChannelVprocGoSlot) != ChannelReady) {
        myos::yield();
    }
    shared.progress(
        ProgressActor::SourceVproc,
        ProgressStage::GoPublished,
        ProgressWait::ChannelGo);
    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    message->transaction = 0x4001;
    message->tag = ChannelVprocTag;
    message->word_count = 1;
    message->words[0] = 0x4441;
    if (myos::channel_try_send(shared.load(ChannelSenderSlot)).status
        != MYOS_STATUS_OK) {
        fail();
    }
    shared.store(ChannelVprocSentSlot, ChannelVprocSent);
    shared.progress(
        ProgressActor::SourceVproc,
        ProgressStage::VprocSend,
        ProgressWait::Tunnel);

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
    shared.progress(
        ProgressActor::SourceVproc,
        ProgressStage::VprocDone,
        ProgressWait::Tunnel,
        TunnelFirstInvoked);
    while (shared.load(TunnelDeliveryCountSlot) < 1
        || shared.load(ParkProbeSlot) != TunnelSecondReady) {
        myos::yield();
    }

    while (shared.load(ChannelVprocDoneSlot) != ChannelVprocDone) {
        myos::yield();
    }
    *message = {};
    message->version = MYOS_CHANNEL_VERSION;
    message->transaction = 0x5001;
    message->tag = 0x5345'4e44'0005;
    message->word_count = 1;
    message->words[0] = 0x5551;
    // Vproc lanes never enter the kernel's blocking Wait path; the queue is
    // empty after the first Vproc message was consumed, so a retryable send is
    // the correct non-blocking drain publication.
    if (myos::channel_try_send(shared.load(ChannelSenderSlot)).status
        != MYOS_STATUS_OK) {
        fail();
    }
    shared.store(ChannelDrainSentSlot, ChannelReady);

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
    shared.progress(
        ProgressActor::SourceVproc,
        ProgressStage::Complete,
        ProgressWait::None,
        TunnelSecondInvoked);
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
    const bool pressure = shared.load(RunModeSlot) == ModePressure;
    const bool resilience =
        shared.load(RunModeSlot) == ModeResilience;

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
    const bool faulting =
        shared.load(PagerVprocSlot) == PagerVprocFaulting;
    /*luna change: settle the resilience fault entirely inside the upcall,
      reason: claim, drop and the stale-key rejections are the deferred
      row-2 dynamic evidence and need no second continuation*/
    if (resilience) {
        // The first Faulted delivery may precede the pending-sequence commit,
        // exactly as in the E1 lane: the fault-key edge is the truth here.
        if (generation == 0 || !faulting) {
            shared.store(PagerDetailSlot, PagerDetailPending);
            fail();
        }
        if (ready_mask != 0) {
            shared.store(PagerDetailSlot, PagerDetailReady);
            fail();
        }
        shared.store(PagerVprocSlot, PagerVprocPending);
        shared.progress(
            ProgressActor::TargetVproc,
            ProgressStage::Pager,
            ProgressWait::Pager);
        myos_fault_key_t key{};
        for (;;) {
            key = libk::AtomicRef{events->fault_key}
                .load<libk::MemoryOrder::Acquire>();
            if (key != 0) {
                break;
            }
            myos::yield();
        }
        const auto kind = libk::AtomicRef{events->fault_kind}
            .load<libk::MemoryOrder::Acquire>();
        const auto access = libk::AtomicRef{events->fault_access}
            .load<libk::MemoryOrder::Acquire>();
        const auto address = libk::AtomicRef{events->fault_address}
            .load<libk::MemoryOrder::Acquire>();
        if (kind != MYOS_VPROC_FAULT_KIND_BACKING_FAILED
            || access != MYOS_VPROC_FAULT_ACCESS_READ
            || address != PagerAddress + PageSize) {
            shared.store(PagerDetailSlot, PagerDetailKind);
            fail();
        }
        const auto claimed = myos::vproc_fault_claim(key);
        if (claimed.status != MYOS_STATUS_OK
            || claimed.value != MYOS_VPROC_FAULT_KIND_BACKING_FAILED) {
            shared.store(PagerDetailSlot, PagerDetailClaimStatus);
            fail();
        }
        const auto duplicate = myos::vproc_fault_claim(key);
        if (duplicate.status != MYOS_STATUS_BUSY) {
            shared.store(PagerDetailSlot, PagerDetailDuplicate);
            fail();
        }
        if (myos::vproc_fault_drop(key).status != MYOS_STATUS_OK) {
            shared.store(PagerDetailSlot, PagerDetailResume);
            fail();
        }
        /*luna change: replay the consumed FaultKey through both exits,
          reason: a dropped continuation must never be resurrectable*/
        const auto stale_resume = myos::vproc_fault_resume(key);
        const auto stale_drop = myos::vproc_fault_drop(key);
        if (stale_resume.status == MYOS_STATUS_OK
            || stale_drop.status == MYOS_STATUS_OK) {
            shared.store(PagerDetailSlot, PagerDetailDuplicate);
            fail();
        }
        shared.store(PagerVprocSlot, PagerVprocDropped);
        shared.progress(
            ProgressActor::TargetVproc,
            ProgressStage::Complete,
            ProgressWait::None);
        myos::exit();
    }
    /*luna change: publish one-shot TargetVproc fault detail before proof failure, reason: init must report the first failed gate without changing barrier truth or progress epochs*/
    if (generation == 0) {
        shared.store(PagerDetailSlot, PagerDetailGeneration);
        fail();
    }
    if (!faulting && pending_sequence == 0) {
        shared.store(PagerDetailSlot, PagerDetailPending);
        fail();
    }
    /*luna change: keep the initial fault-key-zero runtime in the same Vproc policy as resumed delivery, reason: pending_sequence is intentionally not yet committed when the reserved fault frame first redirects execution*/
    if (faulting) {
        if (ready_mask != 0) {
            shared.store(PagerDetailSlot, PagerDetailReady);
            fail();
        }
        /*luna change: publish the pending fault edge once before waiting, reason: unchanged fault state must not advance diagnostic progress on each yield*/
        shared.store(PagerVprocSlot, PagerVprocPending);
        shared.progress(
            ProgressActor::TargetVproc,
            ProgressStage::Pager,
            ProgressWait::Pager);
        myos_fault_key_t key{};
        for (;;) {
            key = libk::AtomicRef{events->fault_key}
                .load<libk::MemoryOrder::Acquire>();
            if (key != 0) {
                break;
            }
            myos::yield();
        }
        const auto kind = libk::AtomicRef{events->fault_kind}
            .load<libk::MemoryOrder::Acquire>();
        const auto access = libk::AtomicRef{events->fault_access}
            .load<libk::MemoryOrder::Acquire>();
        const auto address = libk::AtomicRef{events->fault_address}
            .load<libk::MemoryOrder::Acquire>();
        if (kind != MYOS_VPROC_FAULT_KIND_PAGE_READY) {
            shared.store(PagerDetailSlot, PagerDetailKind);
            fail();
        }
        if (access != MYOS_VPROC_FAULT_ACCESS_READ) {
            shared.store(PagerDetailSlot, PagerDetailAccess);
            fail();
        }
        const bool dirty_fault = pressure
            && shared.load(PagerThreadSlot) == PagerThreadDirtyReady;
        const myos_word_t expected_address = dirty_fault
            ? StressAddress + PageSize : PagerAddress;
        if (address != expected_address) {
            shared.store(PagerDetailSlot, PagerDetailAddress);
            fail();
        }
        const auto claimed = myos::vproc_fault_claim(key);
        if (claimed.status != MYOS_STATUS_OK) {
            shared.store(PagerDetailSlot, PagerDetailClaimStatus);
            fail();
        }
        if (claimed.value != MYOS_VPROC_FAULT_KIND_PAGE_READY) {
            shared.store(PagerDetailSlot, PagerDetailClaimKind);
            fail();
        }
        const auto duplicate = myos::vproc_fault_claim(key);
        if (duplicate.status != MYOS_STATUS_BUSY) {
            shared.store(PagerDetailSlot, PagerDetailDuplicate);
            fail();
        }
        /*luna change: retry resume only across the published RETRY disposition, reason: a callback publisher may still own the previous fault slot generation*/
        for (;;) {
            const auto resumed = myos::vproc_fault_resume(key);
            if (resumed.status == MYOS_STATUS_RETRY) {
                myos::yield();
                continue;
            }
            shared.store(PagerDetailSlot, PagerDetailResume);
            fail();
        }
    }
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
    if ((notification_mask
            & (uint64_t{1} << ChannelVprocIngress)) != 0) {
        const uint64_t sequence = libk::AtomicRef{
            events->notification_sequence[ChannelVprocIngress]}
                .load<libk::MemoryOrder::Acquire>();
        const myos_word_t tag = libk::AtomicRef{
            events->notification_tag[ChannelVprocIngress]}
                .load<libk::MemoryOrder::Acquire>();
        const auto taken = myos::notification_take(
            shared.load(ChannelNotifyRSlot));
        auto* const message = reinterpret_cast<myos_channel_message*>(
            ChannelVprocIpcAddress + TargetVproc * PageSize);
        *message = {};
        message->version = MYOS_CHANNEL_VERSION;
        if (sequence == 0 || tag != ChannelVprocTag
            || taken.status != MYOS_STATUS_OK
            || (taken.value & ChannelNotifyRBadge) == 0
            || myos::channel_try_recv(shared.load(ChannelReceiverSlot)).status
                != MYOS_STATUS_OK
            || message->transaction != 0x4001
            || message->tag != ChannelVprocTag
            || message->word_count != 1 || message->words[0] != 0x4441
            || message->sender_badge != 10) {
            fail();
        }
        shared.store(ChannelVprocDoneSlot, ChannelVprocDone);
        shared.progress(
            ProgressActor::TargetVproc,
            ProgressStage::VprocDone,
            ProgressWait::Tunnel,
            sequence);
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
    myos_word_t vproc_ipc) noexcept {
    if (bootstrap_address == SharedAddress
        && (bootstrap_size == ChannelSenderMagic
            || bootstrap_size == ChannelReceiverMagic)) {
        if (bootstrap_size == ChannelSenderMagic) {
            channel_sender_task(SharedAddress, vproc_shared);
        }
        channel_receiver_task(SharedAddress, vproc_shared);
    }
    /*luna change: route the extra ordinary start descriptor to the Pager worker, reason: the service lane must not enter Vproc arm/upcall ownership*/
    if (bootstrap_address == SharedAddress
        && bootstrap_size == PagerWorkerMagic) {
        if (Shared{SharedAddress}.load(RunModeSlot) == ModeResilience) {
            resilience_worker_task(SharedAddress, vproc_ipc, 0, true);
        }
        pager_worker_task(SharedAddress, vproc_ipc);
    }
    if (bootstrap_address == SharedAddress
        && bootstrap_size == PagerWorkerBMagic) {
        resilience_worker_task(
            SharedAddress, vproc_ipc, vproc_task_stack, false);
    }
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
        const Shared shared{SharedAddress};
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
        if (vproc_magic == VprocMagic) {
            if (shared.load(RunModeSlot) == ModeResilience) {
                myos::user_enter(
                    &resilience_target_task,
                    vproc_task_stack,
                    0,
                    SharedAddress);
            }
            myos::user_enter(
                &target_task,
                vproc_task_stack,
                shared.load(VprocNotificationSlot),
                SharedAddress);
        }
        myos::user_enter(
            &source_task,
            vproc_task_stack,
            vproc_ipc,
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
            /*luna change: gate the stress continuation from frozen manifest mode, reason: ordinary E1 retains its original PageIn/coalesce completion path*/
            const myos_word_t run_mode = shared.load(RunModeSlot);
            const bool pressure = run_mode == ModePressure;
            const myos_cap_t notification = shared.load(NotificationSlot);
            if (notification == 0
                || myos::notification_signal(notification).status
                    != MYOS_STATUS_OK) {
                fail();
            }
            if (bootstrap_size == 0) {
                if (run_mode == ModeOrdinary) {
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
                /*luna change: let Thread0 own its faulting-to-done cell, reason: the three actor cells must not be reset or written by another lane*/
                shared.store(PagerThreadSlot, PagerThreadFaulting);
                const auto value = *reinterpret_cast<volatile const myos_word_t*>(
                    PagerAddress);
                if (value != PagerValue) {
                    fail();
                }
                /*luna change: continue the resilience proof from the resumed
                  continuation, reason: the second fault ends in the contained
                  fault terminal after the worker's pager_fail*/
                if (run_mode == ModeResilience) {
                    shared.store(PagerThreadSlot, PagerThreadFaulting2);
                    (void)*reinterpret_cast<volatile const myos_word_t*>(
                        PagerAddress + PageSize);
                    fail();
                }
                shared.store(PagerThreadSlot, PagerThreadDone);
                /*luna change: run clean and dirty pressure phases only when
                  the root role authorizes them, reason: ordinary E1 has no
                  stress span, rematerialization source or writeback barrier*/
                if (pressure) {
                    /*luna change: consume the existing Thread0 authority handoff,
                      reason: one Region and one anonymous object must survive
                      every bounded remap without a new shared capability slot*/
                    const myos_cap_t stress_region =
                        static_cast<myos_cap_t>(vproc_shared);
                    const myos_cap_t stress_memory =
                        static_cast<myos_cap_t>(vproc_magic);
                    if (stress_region == 0 || stress_memory == 0) {
                        fail();
                    }
                    /*luna change: wait for the worker's one prepared source,
                      reason: stress pressure must not force a second PageIn to
                      allocate staging while the PMM is already exhausted*/
                    while (shared.load(PagerWorkerSlot)
                        != PagerWorkerPrepared) {
                        myos::yield();
                    }
                    /*luna change: map one four-page Region once, reason: page
                      two prewarms metadata before page zero drains capacity
                      and page three remains lazy for post-proof release*/
                    for (;;) {
                        const auto mapped = myos::vm_map(
                            stress_region, stress_memory, StressAddress,
                            StressSize, 0, MYOS_VM_READ | MYOS_VM_WRITE);
                        if (completed(mapped)) {
                            break;
                        }
                        if (mapped.status != MYOS_STATUS_BUSY
                            && mapped.status != MYOS_STATUS_RETRY) {
                            fail();
                        }
                        myos::yield();
                    }
                    /*luna change: prewarm page two before fixture drain,
                      reason: this capacity invariant establishes anonymous,
                      alias, MappedPage and same-leaf table metadata without
                      becoming success truth or control*/
                    static_cast<void>(*reinterpret_cast<
                        volatile const myos_word_t*>(StressAddress + 2 * PageSize));
                    // Target has consumed the seed PageIn and is parked at the
                    // dirty-pressure barrier; no unrelated E1 actor exists in
                    // the pressure role.
                    while (shared.load(PagerVprocSlot) != PagerVprocDone) {
                        myos::yield();
                    }
                    static_cast<void>(*reinterpret_cast<
                        volatile const myos_word_t*>(StressAddress));
                    if (*reinterpret_cast<volatile const myos_word_t*>(
                            PagerAddress) != PagerValue) {
                        fail();
                    }
                    while (shared.load(PagerWorkerSlot)
                        != PagerWorkerSupplied) {
                        myos::yield();
                    }
                    shared.store(PagerThreadSlot, PagerThreadCleanRetryDone);
                    *reinterpret_cast<volatile myos_word_t*>(PagerAddress) =
                        PagerValue + 1;
                    for (;;) {
                        const auto folded = myos::vm_protect(
                            shared.load(PagerRegionSlot),
                            PagerAddress,
                            PageSize,
                            MYOS_VM_READ);
                        if (completed(folded)) {
                            break;
                        }
                        if (folded.status != MYOS_STATUS_BUSY
                            && folded.status != MYOS_STATUS_RETRY) {
                            fail();
                        }
                        myos::yield();
                    }
                    /*luna change: release the dirty-pressure Vproc edge only
                      after the ordinary Pager write and A/D fold, reason: the
                      next fault must race a canonical dirty PageSlot candidate*/
                    shared.store(PagerThreadSlot, PagerThreadDirtyReady);
                    while (shared.load(PagerWorkerSlot)
                            != PagerWorkerWritebackDone
                        || shared.load(PagerVprocSlot)
                            != PagerVprocDirtyRetryDone) {
                        myos::yield();
                    }
                    // All production pressure barriers precede this lazy-page
                    // fault. The scenario releases its PMM owner before the
                    // post-proof PageFault is created, and this returning load
                    // is the fixture-lifetime acknowledgement seen by init.
                    static_cast<void>(*reinterpret_cast<
                        volatile const myos_word_t*>(PressureReleaseAddress));
                    shared.store(
                        PagerThreadSlot, PagerThreadPressureReleased);
                }
            }
            if (bootstrap_size != 0) {
                shared.store(bootstrap_size, ChildReady + bootstrap_size);
            }
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
