#include <test/test.hpp>

#include <fault/observation.hpp>
#include <fault/terminal.hpp>
#include <irq/irq.hpp>
#include <mm/page_state.hpp>
#include <mm/reclaim.hpp>
#include <pager/pager.hpp>
#include <ipc/notification.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/scope_guard.hpp>
#include <uapi/status.h>

namespace {

constinit libk::ManualLifetime<kernel::pager::Pager> e7_pager{};

struct PagerReset final {
    ~PagerReset() noexcept { e7_pager.reset(); }
};

bool test_page_slot_protocol_is_generation_checked(
    const TestContext&) noexcept {
    kernel::mm::PageSlot checked{};
    auto request = checked.begin_request(kernel::mm::PageKey{7, 3}, 3, 1);
    /*luna change: drive the semantic request through explicit transport phases, reason: PageRequest publication must not infer Pager slot identity*/
    if (!request || !request.value()->begin_publish()
        || !request.value()->publish()
        || checked.state != kernel::mm::PageSlotState::Requested
        || checked.begin_fill(0)
        || !checked.begin_fill(11)
        || checked.supply(10, 1)
        || !checked.supply(11, 1)
        || checked.state != kernel::mm::PageSlotState::ResidentClean
        || !checked.mark_dirty(2)) {
        return false;
    }
    const auto first = checked.queue_writeback();
    if (!first || !checked.begin_writeback_publish(first.value())
        || !checked.publish_writeback(first.value(), 21)
        || !checked.claim_writeback(first.value(), 21, 22)
        || !checked.requeue_writeback(first.value(), 21, 22)
        || !checked.claim_writeback(first.value(), 21, 23)
        || !checked.mark_dirty(3)
        || !checked.begin_writeback_complete(first.value(), 21, 23)
        || !checked.complete_writeback(first.value(), 21, 23)
        || checked.state != kernel::mm::PageSlotState::ResidentDirty) {
        return false;
    }
    const auto second = checked.queue_writeback();
    if (!second || !checked.begin_writeback_publish(second.value())
        || !checked.publish_writeback(second.value(), 31)
        || !checked.claim_writeback(second.value(), 31, 32)
        || !checked.begin_writeback_complete(second.value(), 31, 32)
        || !checked.complete_writeback(second.value(), 31, 32)
        || checked.state != kernel::mm::PageSlotState::ResidentClean) {
        return false;
    }
    kernel::mm::PageSlot aborted{};
    /*luna change: use the same explicit request admission in the abort case, reason: every PageSlot fill test must cross the queued transport phase*/
    if (!aborted.begin_request(kernel::mm::PageKey{8, 2}, 2, 1)
        || !aborted.request.begin_publish()
        || !aborted.request.publish()
        || !aborted.begin_fill(5)
        || !aborted.supply(5, 1)
        || !aborted.mark_dirty(9)) {
        return false;
    }
    const auto publish = aborted.queue_writeback();
    return publish && aborted.begin_writeback_publish(publish.value())
        && aborted.abort_writeback_publish(publish.value())
        && aborted.state == kernel::mm::PageSlotState::WritebackQueued;
}

bool test_pager_fifo_claim_close_and_requeue(const TestContext&) noexcept {
    auto& pager = e7_pager.emplace();
    PagerReset reset{};
    const auto first = pager.publish(kernel::mm::PageKey{1, 4}, 4, 1, 1, 7);
    const auto second = pager.publish(kernel::mm::PageKey{2, 8}, 8, 1, 1);
    if (!first || !second || pager.pending() != 2) {
        return false;
    }
    const auto claimed = pager.try_claim();
    if (!claimed || claimed.value().page_key.index != 4
        || !pager.requeue(
            claimed.value().claim, claimed.value())) {
        return false;
    }
    const auto second_claim = pager.try_claim();
    if (!second_claim || second_claim.value().page_key.index != 8) {
        return false;
    }
    auto second_reply = pager.begin_reply(second_claim.value().claim);
    if (!second_reply || !second_reply.value().commit()) {
        return false;
    }
    const auto requeued = pager.try_claim();
    if (!requeued || requeued.value().page_key.index != 4
        || pager.close(false)
        || pager.state() != kernel::pager::State::Closing) {
        return false;
    }
    auto reply = pager.begin_reply(requeued.value().claim);
    return reply && reply.value().commit()
        && pager.state() == kernel::pager::State::Closed;
}

bool test_pager_claimed_request_closes_after_completion(
    const TestContext&) noexcept {
    auto& pager = e7_pager.emplace();
    PagerReset reset{};
    const auto published = pager.publish(kernel::mm::PageKey{1, 9}, 9, 1, 1);
    if (!published) {
        return false;
    }
    const auto claimed = pager.try_claim();
    if (!claimed || pager.close(false) || pager.state() != kernel::pager::State::Closing) {
        return false;
    }
    auto reply = pager.begin_reply(claimed.value().claim);
    return reply && reply.value().commit()
        && pager.state() == kernel::pager::State::Closed;
}

bool test_pager_notification_is_a_readiness_projection(
    const TestContext&) noexcept {
    auto& pager = e7_pager.emplace();
    PagerReset reset{};
    kernel::ipc::Notification notification{};
    if (!pager.bind(notification, 0x20)
        || !pager.publish(kernel::mm::PageKey{1, 12}, 12, 1, 1)
        || !notification.take()
        || !pager.unbind()) {
        return false;
    }
    const auto request = pager.try_claim();
    if (!request) {
        return false;
    }
    auto reply = pager.begin_reply(request.value().claim);
    return reply && reply.value().commit() && pager.close(false);
}

void page_wait_published(void* context, kernel::mm::PageWaitResult result) noexcept {
    auto* const seen = static_cast<kernel::mm::PageWaitResult*>(context);
    *seen = result;
}

bool pager_attachment_finish(
    void*,
    const kernel::pager::Request&,
    kernel::pager::PagerAttachment::Event) noexcept {
    return true;
}

void pager_attachment_release(void*) noexcept {}

struct PagerTransition final {
    kernel::pager::Pager* pager{};
    kernel::pager::PagerAttachment* attachment{};
    bool reject_claim{};
    bool force_claim{};
    bool force_requeue{};
    bool close_claim{};
    bool detach_requeue{};
    bool force_forced{};
    usize forced{};

    static auto run(
        void* context,
        const kernel::pager::Request&,
        kernel::pager::PagerAttachment::Event event) noexcept -> bool {
        auto& self = *static_cast<PagerTransition*>(context);
        if (event == kernel::pager::PagerAttachment::Event::Forced) {
            ++self.forced;
            if (self.force_forced) {
                static_cast<void>(self.pager->close(true));
            }
            return true;
        }
        if (event == kernel::pager::PagerAttachment::Event::Claim) {
            if (self.force_claim) {
                static_cast<void>(self.pager->close(true));
            }
            if (self.close_claim) {
                static_cast<void>(self.pager->close(false));
            }
            return !self.reject_claim;
        }
        if (self.detach_requeue) {
            static_cast<void>(self.pager->detach(*self.attachment));
        }
        if (self.force_requeue) {
            static_cast<void>(self.pager->close(true));
        }
        return true;
    }
};

bool test_pager_admitted_claim_completes_while_closing(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    PagerTransition transition{.pager = &pager, .close_claim = true};
    kernel::pager::PagerAttachment attachment{
        .context = &transition,
        .transition = &PagerTransition::run,
        .drained = &pager_attachment_release,
    };
    auto detach = libk::on_scope_exit([&]() noexcept {
        if (attachment.state != kernel::pager::PagerAttachment::State::Detached) {
            static_cast<void>(pager.detach(attachment));
        }
    });
    transition.attachment = &attachment;
    if (!pager.attach(attachment)
        || !pager.publish(attachment, kernel::mm::PageKey{1, 9}, 9, 1, 1)) {
        return false;
    }
    const auto claim = pager.try_claim();
    if (!claim || pager.state() != kernel::pager::State::Closing) {
        return false;
    }
    auto reply = pager.begin_reply(claim.value().claim);
    return reply && reply.value().commit()
        && pager.state() == kernel::pager::State::Closed;
}

bool test_pager_claim_owner_rejection_stays_queued(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    PagerTransition transition{.pager = &pager, .reject_claim = true};
    kernel::ipc::Notification notification{};
    kernel::pager::PagerAttachment attachment{
        .context = &transition,
        .transition = &PagerTransition::run,
        .drained = &pager_attachment_release,
    };
    auto detach = libk::on_scope_exit([&]() noexcept {
        if (attachment.state != kernel::pager::PagerAttachment::State::Detached) {
            static_cast<void>(pager.detach(attachment));
        }
    });
    if (!pager.attach(attachment) || !pager.bind(notification, 1)
        || !pager.publish(attachment, kernel::mm::PageKey{1, 1}, 1, 1, 1)
        || !notification.take() || pager.try_claim() || pager.pending() != 1
        || !notification.take()) {
        return false;
    }
    return pager.close(true) && transition.forced == 1;
}

bool test_pager_force_during_claim_compensates_owner(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    PagerTransition transition{.pager = &pager, .force_claim = true};
    kernel::pager::PagerAttachment attachment{
        .context = &transition,
        .transition = &PagerTransition::run,
        .drained = &pager_attachment_release,
    };
    auto detach = libk::on_scope_exit([&]() noexcept {
        if (attachment.state != kernel::pager::PagerAttachment::State::Detached) {
            static_cast<void>(pager.detach(attachment));
        }
    });
    if (!pager.attach(attachment)
        || !pager.publish(attachment, kernel::mm::PageKey{2, 1}, 1, 1, 1)
        || pager.try_claim()) {
        return false;
    }
    return pager.state() == kernel::pager::State::Closed
        && transition.forced == 1;
}

bool test_pager_force_during_requeue_compensates_owner(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    PagerTransition transition{.pager = &pager, .force_requeue = true};
    kernel::pager::PagerAttachment attachment{
        .context = &transition,
        .transition = &PagerTransition::run,
        .drained = &pager_attachment_release,
    };
    auto detach = libk::on_scope_exit([&]() noexcept {
        if (attachment.state != kernel::pager::PagerAttachment::State::Detached) {
            static_cast<void>(pager.detach(attachment));
        }
    });
    if (!pager.attach(attachment)
        || !pager.publish(attachment, kernel::mm::PageKey{3, 1}, 1, 1, 1)) {
        return false;
    }
    const auto claim = pager.try_claim();
    if (!claim || pager.requeue(claim.value().claim, claim.value())) {
        return false;
    }
    return pager.state() == kernel::pager::State::Closed
        && transition.forced == 1;
}

bool test_pager_requeue_rescans_late_forced(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    PagerTransition transition{
        .pager = &pager,
        .detach_requeue = true,
        .force_forced = true,
    };
    kernel::pager::PagerAttachment attachment{
        .context = &transition,
        .transition = &PagerTransition::run,
        .drained = &pager_attachment_release,
    };
    auto detach = libk::on_scope_exit([&]() noexcept {
        if (attachment.state != kernel::pager::PagerAttachment::State::Detached) {
            static_cast<void>(pager.detach(attachment));
        }
    });
    transition.attachment = &attachment;
    if (!pager.attach(attachment)
        || !pager.publish(attachment, kernel::mm::PageKey{7, 1}, 1, 1, 1)) {
        return false;
    }
    const auto claim = pager.try_claim();
    if (!claim || pager.requeue(claim.value().claim, claim.value())) {
        return false;
    }
    return transition.forced == 1
        && pager.state() == kernel::pager::State::Closed;
}

bool test_pager_detach_aborts_exact_reply(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    PagerTransition transition{.pager = &pager};
    kernel::pager::PagerAttachment attachment{
        .context = &transition,
        .transition = &PagerTransition::run,
        .drained = &pager_attachment_release,
    };
    auto detach = libk::on_scope_exit([&]() noexcept {
        if (attachment.state != kernel::pager::PagerAttachment::State::Detached) {
            static_cast<void>(pager.detach(attachment));
        }
    });
    if (!pager.attach(attachment)
        || !pager.publish(attachment, kernel::mm::PageKey{4, 1}, 1, 1, 1)) {
        return false;
    }
    const auto claim = pager.try_claim();
    if (!claim) {
        return false;
    }
    auto reply = pager.begin_reply(claim.value().claim);
    if (!reply || !pager.detach(attachment) || !reply.value().abort()) {
        return false;
    }
    return transition.forced == 1 && pager.close(true);
}

bool test_pager_force_drains_unleased_claims(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    PagerTransition transition{.pager = &pager};
    kernel::pager::PagerAttachment attachment{
        .context = &transition,
        .transition = &PagerTransition::run,
        .drained = &pager_attachment_release,
    };
    auto detach = libk::on_scope_exit([&]() noexcept {
        if (attachment.state != kernel::pager::PagerAttachment::State::Detached) {
            static_cast<void>(pager.detach(attachment));
        }
    });
    if (!pager.attach(attachment)
        || !pager.publish(attachment, kernel::mm::PageKey{5, 1}, 1, 1, 1)
        || !pager.publish(attachment, kernel::mm::PageKey{6, 1}, 1, 1, 1)) {
        return false;
    }
    const auto first = pager.try_claim();
    const auto second = pager.try_claim();
    if (!first || !second) {
        return false;
    }
    auto reply = pager.begin_reply(first.value().claim);
    if (!reply || pager.close(true)
        || transition.forced != 1
        || !reply.value().commit()) {
        return false;
    }
    return pager.state() == kernel::pager::State::Closed;
}

bool test_page_wait_relation_is_owner_storage_and_terminal_checked(
    const TestContext&) noexcept {
    kernel::mm::PageRequest request{
        .key = kernel::mm::PageKey{9, 1},
        .first = 1,
        .count = 1,
        .state = kernel::mm::PageRequestState::Published,
    };
    kernel::mm::WaitRelation relation{};
    auto seen = kernel::mm::PageWaitResult::Canceled;
    if (!request.attach(relation, &seen, &page_wait_published)) {
        return false;
    }
    kernel::mm::WaitClaim ready[1]{};
    /*luna change: finalize relation claims before delivery in focused test, reason: host unlink must close the continuation reuse window before callback publication*/
    if (request.claim_waiters(ready, 1, kernel::mm::PageWaitResult::Ready)
            != 1
        || ready[0].relation() != &relation
        || !request.finish_claim(ready[0])
        || !ready[0]
        || ready[0].relation() != nullptr
        || relation.attached()
        || !ready[0].publish()
        || seen != kernel::mm::PageWaitResult::Ready) {
        return false;
    }
    if (!ready[0].release()) {
        return false;
    }
    ready[0].reset();
    const u64 previous_generation = relation.generation;
    if (!request.reset()) {
        return false;
    }
    request.key = kernel::mm::PageKey{9, 1};
    request.first = 1;
    request.count = 1;
    request.state = kernel::mm::PageRequestState::Published;
    if (!request.attach(relation, &seen, &page_wait_published)) {
        return false;
    }
    if (relation.generation == previous_generation) {
        return false;
    }
    kernel::mm::WaitClaim canceled{};
    if (relation.claim(
            previous_generation,
            kernel::mm::PageWaitResult::Canceled,
            canceled)) {
        return false;
    }
    if (request.claim_waiters(
            &canceled, 1, kernel::mm::PageWaitResult::Canceled) != 1
        || canceled.relation() != &relation
        || !request.finish_claim(canceled)
        || !canceled
        || canceled.relation() != nullptr
        || relation.attached()
        || !canceled.publish()
        || !canceled.release()) {
        return false;
    }
    canceled.reset();
    return request.reset();
}

bool test_typed_writeback_delivery_rejects_stale_claim(
    const TestContext&) noexcept {
    auto& pager = e7_pager.emplace();
    PagerReset reset{};
    const auto published = pager.publish_writeback(
        kernel::mm::PageKey{3, 7}, 4, 11);
    if (!published || published.value().kind
            != kernel::pager::DeliveryKind::Writeback) {
        return false;
    }
    const auto claimed = pager.try_claim();
    if (!claimed || !claimed.value().claim
        || claimed.value().claim.delivery != claimed.value().key) {
        return false;
    }
    const auto old_claim = claimed.value().claim;
    auto mismatched = claimed.value();
    ++mismatched.page_key.index;
    if (pager.requeue(old_claim, mismatched)
        || pager.state() != kernel::pager::State::Open) {
        return false;
    }
    if (!pager.requeue(claimed.value().claim, claimed.value())) {
        return false;
    }
    const auto retried = pager.try_claim();
    if (!retried || retried.value().claim.generation == old_claim.generation) {
        return false;
    }
    const auto stale = pager.begin_reply(old_claim);
    if (stale || stale.error() != kernel::pager::Error::Stale) {
        return false;
    }
    auto reply = pager.begin_reply(retried.value().claim);
    return reply && reply.value().commit() && pager.close(false);
}

bool test_pager_force_close_waits_for_claim_lease(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    const auto published = pager.publish(
        kernel::mm::PageKey{4, 2}, 2, 1, 1);
    if (!published) {
        return false;
    }
    const auto claimed = pager.try_claim();
    if (!claimed) {
        return false;
    }
    auto reply = pager.begin_reply(claimed.value().claim);
    if (!reply) {
        return false;
    }
    if (pager.close(true) || pager.state() != kernel::pager::State::Forced) {
        return false;
    }
    if (pager.close(false) || pager.state() != kernel::pager::State::Forced) {
        return false;
    }
    if (!reply.value().commit() || pager.state() != kernel::pager::State::Closed) {
        return false;
    }
    const auto stale = pager.begin_reply(claimed.value().claim);
    return !stale && stale.error() == kernel::pager::Error::Stale;
}

bool test_pager_detach_invalidates_new_claim_leases(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    u8 context{};
    kernel::pager::PagerAttachment attachment{
        .context = &context,
        .transition = &pager_attachment_finish,
        .drained = &pager_attachment_release,
    };
    auto detach = libk::on_scope_exit([&]() noexcept {
        if (attachment.state != kernel::pager::PagerAttachment::State::Detached) {
            static_cast<void>(pager.detach(attachment));
        }
    });
    if (!pager.attach(attachment)
        || !pager.publish(attachment, kernel::mm::PageKey{6, 4}, 4, 1, 1)) {
        return false;
    }
    const auto claimed = pager.try_claim();
    if (!claimed) {
        return false;
    }
    auto reply = pager.begin_reply(claimed.value().claim);
    if (!reply || !pager.detach(attachment)) {
        return false;
    }
    if (pager.begin_reply(claimed.value().claim)
        || !reply.value().commit()) {
        return false;
    }
    return pager.close(true);
}

bool test_writeback_dirty_during_completion_rejects_stale_ack(
    const TestContext&) noexcept {
    kernel::mm::PageSlot slot{};
    /*luna change: make page-in admission explicit before writeback setup, reason: focused writeback tests must exercise the single PageRequest phase path*/
    if (!slot.begin_request(kernel::mm::PageKey{5, 3}, 3, 1)
        || !slot.request.begin_publish()
        || !slot.request.publish()
        || !slot.begin_fill(7)
        || !slot.supply(7, 1)
        || !slot.mark_dirty(1)) {
        return false;
    }
    const auto key = slot.queue_writeback();
    if (!key
        || !slot.begin_writeback_publish(key.value())
        || !slot.publish_writeback(key.value(), 11)
        || !slot.claim_writeback(key.value(), 11, 13)
        || !slot.begin_writeback_complete(key.value(), 11, 13)
        || !slot.mark_dirty(2)
        || !slot.complete_writeback(key.value(), 11, 13)
        || slot.state != kernel::mm::PageSlotState::ResidentDirty
        || slot.complete_writeback(key.value(), 11, 13)) {
        return false;
    }
    const auto retry = slot.queue_writeback();
    return retry && retry.value().generation != key.value().generation;
}

bool test_writeback_failure_is_owner_terminal(
    const TestContext&) noexcept {
    kernel::mm::PageSlot permanent{};
    /*luna change: make terminal writeback fixtures cross explicit page-in publication, reason: PageRequest state is the only page admission truth*/
    if (!permanent.begin_request(kernel::mm::PageKey{9, 4}, 4, 1)
        || !permanent.request.begin_publish()
        || !permanent.request.publish()
        || !permanent.begin_fill(1) || !permanent.supply(1, 1)
        || !permanent.mark_dirty(1)) {
        return false;
    }
    const auto io = permanent.queue_writeback();
    if (!io || !permanent.begin_writeback_publish(io.value())
        || !permanent.publish_writeback(io.value(), 2)
        || !permanent.claim_writeback(io.value(), 2, 3)
        || !permanent.begin_writeback_complete(io.value(), 2, 3)
        || !permanent.fail_writeback(
            io.value(), 2, 3, kernel::mm::WritebackFailure::Io)
        || permanent.state != kernel::mm::PageSlotState::WritebackFailed
        || permanent.writeback.failure != kernel::mm::WritebackFailure::Io) {
        return false;
    }
    kernel::mm::PageSlot forced{};
    if (!forced.begin_request(kernel::mm::PageKey{10, 4}, 4, 1)
        || !forced.request.begin_publish()
        || !forced.request.publish()
        || !forced.begin_fill(1) || !forced.supply(1, 1)
        || !forced.mark_dirty(1)) {
        return false;
    }
    const auto unavailable = forced.queue_writeback();
    return unavailable
        && forced.begin_writeback_publish(unavailable.value())
        && forced.publish_writeback(unavailable.value(), 2)
        && forced.fail_writeback(
            unavailable.value(), 2, 0,
            kernel::mm::WritebackFailure::BackingUnavailable)
        && forced.state == kernel::mm::PageSlotState::WritebackFailed
        && forced.writeback.failure
            == kernel::mm::WritebackFailure::BackingUnavailable;
}

bool test_page_slot_commits_before_waiter_drain(
    const TestContext&) noexcept {
    kernel::mm::PageSlot slot{};
    /*luna change: publish the request before beginning fill in the waiter fixture, reason: tests must follow the production PageRequest transition order*/
    if (!slot.begin_request(kernel::mm::PageKey{17, 2}, 2, 1)
        || !slot.request.begin_publish()
        || !slot.request.publish()
        || !slot.begin_fill(9)) {
        return false;
    }
    kernel::mm::WaitRelation relation{};
    auto seen = kernel::mm::PageWaitResult::Canceled;
    if (!slot.request.attach(relation, &seen, &page_wait_published)
        || !slot.supply(9, 1)
        || slot.state != kernel::mm::PageSlotState::ResidentClean
        || slot.request.state != kernel::mm::PageRequestState::Ready
        || slot.request.waiters.empty()) {
        return false;
    }
    kernel::mm::WaitClaim claim[1]{};
    /*luna change: exercise host finalization before waiter publication, reason: the slot test must encode the production claim order*/
    if (slot.request.claim_waiters(
            claim, 1, kernel::mm::PageWaitResult::Ready) != 1
        || claim[0].relation() != &relation
        || !slot.request.finish_claim(claim[0])
        || !claim[0]
        || claim[0].relation() != nullptr
        || relation.attached()
        || !claim[0].publish()
        || !claim[0].release()) {
        return false;
    }
    claim[0].reset();
    return seen == kernel::mm::PageWaitResult::Ready
        && slot.request.reset();
}

bool test_page_request_bounded_multiwaiter_drain(
    const TestContext&) noexcept {
    kernel::mm::PageRequest request{
        .key = kernel::mm::PageKey{18, 2},
        .first = 2,
        .count = 1,
        .state = kernel::mm::PageRequestState::Ready,
    };
    kernel::mm::WaitRelation relations[3]{};
    kernel::mm::PageWaitResult seen[3]{
        kernel::mm::PageWaitResult::Canceled,
        kernel::mm::PageWaitResult::Canceled,
        kernel::mm::PageWaitResult::Canceled,
    };
    for (usize index = 0; index < 3; ++index) {
        if (!request.attach(relations[index], &seen[index], &page_wait_published)) {
            return false;
        }
    }
    kernel::mm::WaitClaim claims[2]{};
    usize drained{};
    for (usize pass = 0; pass < 2; ++pass) {
        const usize count = request.claim_waiters(
            claims, pass == 0 ? 2 : 1, kernel::mm::PageWaitResult::Ready);
        if (count != (pass == 0 ? 2 : 1)) {
            return false;
        }
        for (usize index = 0; index < count; ++index) {
            /*luna change: finalize each bounded waiter claim before callback, reason: focused drains must prove unlink-before-publish for every relation*/
            auto* const relation = claims[index].relation();
            if (relation == nullptr || !request.finish_claim(claims[index])
                || !claims[index] || claims[index].relation() != nullptr
                || relation->attached() || !claims[index].publish()
                || !claims[index].release()) {
                return false;
            }
            claims[index].reset();
            ++drained;
        }
    }
    return drained == 3 && request.waiters.empty()
        && seen[0] == kernel::mm::PageWaitResult::Ready
        && seen[1] == kernel::mm::PageWaitResult::Ready
        && seen[2] == kernel::mm::PageWaitResult::Ready
        && request.reset();
}

bool test_writeback_queue_full_retains_obligation(
    const TestContext&) noexcept {
    kernel::pager::Pager pager{};
    for (usize index = 0; index < kernel::pager::max_requests; ++index) {
        if (!pager.publish(
                kernel::mm::PageKey{index + 1, index}, index, 1, index + 1)) {
            return false;
        }
    }
    kernel::mm::PageSlot slot{};
    /*luna change: publish the queued page request before writeback setup, reason: test setup must not bypass the semantic request phase*/
    if (!slot.begin_request(kernel::mm::PageKey{1, 0}, 0, 1)
        || !slot.request.begin_publish()
        || !slot.request.publish()
        || !slot.begin_fill(1)
        || !slot.supply(1, 1)
        || !slot.mark_dirty(1)) {
        return false;
    }
    const auto writeback = slot.queue_writeback();
    if (!writeback) {
        return false;
    }
    const auto published = pager.publish_writeback(
        writeback.value().page,
        writeback.value().generation,
        writeback.value().dirty_epoch);
    const bool retained = !published
        && published.error() == kernel::pager::Error::Full
        && slot.state == kernel::mm::PageSlotState::WritebackQueued;
    if (!retained) {
        return false;
    }
    if (!pager.close(true)) {
        return false;
    }
    return true;
}

bool test_pressure_result_and_frame_progress_are_distinct(
    const TestContext&) noexcept {
    kernel::mm::PageReclaimer reclaimer{};
    constexpr usize relation_count = kernel::mm::PageReclaimer::pass_budget + 3;
    kernel::mm::WaitRelation relations[relation_count]{};
    const auto publish = [](void*, kernel::mm::PageWaitResult) noexcept {};
    for (usize index = 0; index < relation_count; ++index) {
        if (!reclaimer.retain(relations[index], 1, nullptr, publish)) {
            return false;
        }
    }
    if (reclaimer.pending() != relation_count) {
        return false;
    }
    kernel::mm::WaitClaim ready[kernel::mm::PageReclaimer::pass_budget]{};
    usize drained{};
    for (usize pass = 0; pass < 2; ++pass) {
        const usize count = reclaimer.wake(
            2, ready, kernel::mm::PageReclaimer::pass_budget);
        const usize expected = pass == 0
            ? kernel::mm::PageReclaimer::pass_budget
            : relation_count - kernel::mm::PageReclaimer::pass_budget;
        if (count != expected) {
            return false;
        }
        for (usize index = 0; index < count; ++index) {
            /*luna change: consume reclaimer claims after wake-side finalization, reason: wake owns unlink/finalize and callbacks only publish the detached snapshot*/
            if (!ready[index] || ready[index].relation() != nullptr
                || !ready[index].publish()
                || !ready[index].release()) {
                return false;
            }
            ready[index].reset();
            ++drained;
        }
    }
    if (drained != relation_count || reclaimer.pending() != 0) {
        return false;
    }
    auto& reused = relations[0];
    const u64 previous_generation = reused.generation;
    if (!reclaimer.retain(reused, 2, nullptr, publish)
        || reused.generation == previous_generation
        || reclaimer.release(reused, previous_generation)) {
        return false;
    }
    if (reclaimer.wake(3, ready, 1) != 1) {
        return false;
    }
    return ready[0]
        && ready[0].relation() == nullptr
        && ready[0].publish()
        && ready[0].release()
        && (ready[0].reset(), reclaimer.pending() == 0);
}

bool test_irq_sequence_reassert_requires_latest_ack(
    const TestContext&) noexcept {
    kernel::ipc::Notification notification{};
    kernel::irq::Irq irq{kernel::irq::SourceToken::from_bootstrap(10)};
    if (!irq.bind(notification, 0x40)) {
        return false;
    }
    const auto first = irq.observe();
    const auto first_notice = notification.take();
    const auto second = irq.observe();
    if (!first || !second || !first_notice
        || irq.ack(first.value().sequence)) {
        return false;
    }
    return irq.ack(second.value().sequence)
        && notification.take()
        && irq.unbind()
        && irq.state() == kernel::irq::State::MaskedUnbound;
}

bool test_terminal_observation_is_read_only_projection(
    const TestContext&) noexcept {
    kernel::fault::TerminalRecord target{};
    kernel::ipc::Notification notification{};
    kernel::ipc::Notification debugger_notification{};
    kernel::fault::TerminalObservation observation{};
    kernel::fault::TerminalObservation debugger{};
    if (!observation.bind(target, notification, 0x80)
        || !debugger.bind(target, debugger_notification, 0x81)
        || !target.claim(
            kernel::fault::Reason::Stop, MYOS_STATUS_CANCELED, 7)
        || !notification.take() || !debugger_notification.take()) {
        return false;
    }
    const auto snapshot = observation.query();
    if (snapshot.reason != kernel::fault::Reason::Stop
        || snapshot.status != MYOS_STATUS_CANCELED
        || snapshot.detail != 7
        || target.claim(kernel::fault::Reason::Fault, MYOS_STATUS_INTERNAL)) {
        return false;
    }
    observation.reset();
    debugger.reset();
    return target.published();
}

} // namespace

void register_e7_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "e7", "PageSlot rejects stale supply and tracks dirty writeback",
        test_page_slot_protocol_is_generation_checked);
    (void)registry.add(
        "e7", "Pager keeps bounded FIFO request ownership",
        test_pager_fifo_claim_close_and_requeue);
    (void)registry.add(
        "e7", "Pager completes an admitted claim while Closing",
        test_pager_admitted_claim_completes_while_closing);
    (void)registry.add(
        "e7", "Pager retains queued work when its owner rejects Claim",
        test_pager_claim_owner_rejection_stays_queued);
    (void)registry.add(
        "e7", "Pager force during Claim compensates the owner edge",
        test_pager_force_during_claim_compensates_owner);
    (void)registry.add(
        "e7", "Pager force during Requeue compensates the owner edge",
        test_pager_force_during_requeue_compensates_owner);
    (void)registry.add(
        "e7", "Pager requeue rescans a late Forced winner",
        test_pager_requeue_rescans_late_forced);
    (void)registry.add(
        "e7", "Pager detach terminalizes an outstanding Reply abort",
        test_pager_detach_aborts_exact_reply);
    (void)registry.add(
        "e7", "Pager force drains claims without a slot lease",
        test_pager_force_drains_unleased_claims);
    (void)registry.add(
        "e7", "Pager closes after claimed completion",
        test_pager_claimed_request_closes_after_completion);
    (void)registry.add(
        "e7", "Pager notification is only a readiness projection",
        test_pager_notification_is_a_readiness_projection);
    (void)registry.add(
        "e7", "Page wait relation terminal claim drains before reuse",
        test_page_wait_relation_is_owner_storage_and_terminal_checked);
    (void)registry.add(
        "e7", "Typed writeback delivery rejects stale service claims",
        test_typed_writeback_delivery_rejects_stale_claim);
    (void)registry.add(
        "e7", "Pager force close waits for claim lease",
        test_pager_force_close_waits_for_claim_lease);
    (void)registry.add(
        "e7", "Pager detach invalidates retired attachment leases",
        test_pager_detach_invalidates_new_claim_leases);
    (void)registry.add(
        "e7", "Writeback completion preserves dirty epoch",
        test_writeback_dirty_during_completion_rejects_stale_ack);
    (void)registry.add(
        "e7", "Writeback terminal failure remains PageSlot-owned",
        test_writeback_failure_is_owner_terminal);
    (void)registry.add(
        "e7", "PageSlot commits content before waiter drain",
        test_page_slot_commits_before_waiter_drain);
    (void)registry.add(
        "e7", "PageRequest drains multiple waiters by fixed budget",
        test_page_request_bounded_multiwaiter_drain);
    (void)registry.add(
        "e7", "Writeback queue-full retains semantic obligation",
        test_writeback_queue_full_retains_obligation);
    (void)registry.add(
        "e7", "Pressure and frame progress remain distinct",
        test_pressure_result_and_frame_progress_are_distinct);
    (void)registry.add(
        "e7", "Irq sequence prevents stale acknowledgement",
        test_irq_sequence_reassert_requires_latest_ack);
    (void)registry.add(
        "e7", "TerminalObservation projects a single terminal winner",
        test_terminal_observation_is_read_only_projection);
}
