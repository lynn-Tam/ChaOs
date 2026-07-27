#include <test/test.hpp>

#include <fault/observation.hpp>
#include <fault/terminal.hpp>
#include <irq/irq.hpp>
#include <mm/page_state.hpp>
#include <pager/pager.hpp>
#include <ipc/notification.hpp>
#include <libk/manual_lifetime.hpp>
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
    if (!request || checked.state != kernel::mm::PageSlotState::Requested
        || checked.begin_fill(0)
        || !checked.begin_fill(11)
        || checked.supply(10, 1)
        || !checked.supply(11, 1)
        || checked.state != kernel::mm::PageSlotState::ResidentClean
        || !checked.mark_dirty(2)
        || !checked.queue_writeback()
        || !checked.begin_writeback(2)
        || !checked.mark_dirty(3)
        || !checked.complete_writeback(2, true)
        || checked.state != kernel::mm::PageSlotState::ResidentDirty
        || !checked.queue_writeback()
        || !checked.begin_writeback(3)
        || !checked.complete_writeback(3, true)
        || checked.state != kernel::mm::PageSlotState::ResidentClean) {
        return false;
    }
    return true;
}

bool test_pager_fifo_claim_close_and_requeue(const TestContext&) noexcept {
    auto& pager = e7_pager.emplace();
    PagerReset reset{};
    const auto first = pager.publish(kernel::mm::PageKey{1, 4}, 4, 1, 1);
    const auto second = pager.publish(kernel::mm::PageKey{2, 8}, 8, 1, 1);
    if (!first || !second || pager.pending() != 2) {
        return false;
    }
    const auto claimed = pager.try_claim();
    if (!claimed || claimed.value().page_key.index != 4
        || !pager.requeue(claimed.value().key)) {
        return false;
    }
    const auto second_claim = pager.try_claim();
    if (!second_claim || second_claim.value().page_key.index != 8
        || !pager.complete(second_claim.value().key)) {
        return false;
    }
    const auto requeued = pager.try_claim();
    if (!requeued || requeued.value().page_key.index != 4
        || pager.close(false)
        || pager.state() != kernel::pager::State::Closing) {
        return false;
    }
    return pager.complete(requeued.value().key)
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
    return pager.complete(claimed.value().key)
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
    return request && pager.complete(request.value().key)
        && pager.close(false);
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
        "e7", "Pager closes after claimed completion",
        test_pager_claimed_request_closes_after_completion);
    (void)registry.add(
        "e7", "Pager notification is only a readiness projection",
        test_pager_notification_is_a_readiness_projection);
    (void)registry.add(
        "e7", "Irq sequence prevents stale acknowledgement",
        test_irq_sequence_reassert_requires_latest_ack);
    (void)registry.add(
        "e7", "TerminalObservation projects a single terminal winner",
        test_terminal_observation_is_read_only_projection);
}
