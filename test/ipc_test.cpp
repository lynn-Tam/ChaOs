#include <task/exit.hpp>
#include <irq/irq.hpp>
#include <test/test.hpp>

#include <cap/cap.hpp>
#include <ipc/notification.hpp>
#include <uapi/ipc.h>

namespace {

class TestSource final {
public:
    TestSource() noexcept
        : binding_(ipc::NotificationSource::Closed::bind<&TestSource::closed>(*this)) {}

    ~TestSource() noexcept { binding_.reset(); }

    [[nodiscard]] auto sc() noexcept
        -> ipc::NotificationSource& {
        return binding_;
    }

    void publish() noexcept {
        ++sequence_;
        ready_ = true;
        static_cast<void>(binding_.signal());
    }

    [[nodiscard]] auto consume() noexcept -> u64 {
        ready_ = false;
        return sequence_;
    }

    void rearm(u64 observed) noexcept {
        if (ready_ || sequence_ != observed) {
            static_cast<void>(binding_.signal());
        }
    }

    [[nodiscard]] auto was_closed() const noexcept -> bool {
        return closed_;
    }

private:
    void closed() noexcept { closed_ = true; }

    ipc::NotificationSource binding_;
    u64 sequence_{};
    bool ready_{};
    bool closed_{};
};

bool test_notification_badges_are_coalesced(const TestContext&) noexcept {
    ipc::Notification notification{};
    if (!notification.signal(1) || !notification.signal(2)
        || !notification.signal(1)) {
        return false;
    }
    const auto first = notification.take();
    const auto second = notification.take();
    return first && first.value().badges == 3
        && first.value().sequence == 3
        && !second
        && second.error() == ipc::NotificationError::Empty;
}

bool test_notification_source_rearm_preserves_level(
    const TestContext&) noexcept {
    ipc::Notification notification{};
    TestSource source{};
    if (!notification.bind(source.sc(), 4)) {
        return false;
    }

    source.publish();
    const auto initial = notification.take();
    if (!initial || initial.value().badges != 4) {
        return false;
    }

    const u64 observed = source.consume();
    // A new readiness transition in the consume/rearm window must be
    // projected again even if an earlier badge was already taken.
    source.publish();
    source.rearm(observed);
    const auto replayed = notification.take();
    source.sc().reset();
    return replayed && replayed.value().badges == 4
        && replayed.value().sequence > initial.value().sequence
        && !source.sc().attached() && !source.was_closed();
}

bool test_notification_source_has_one_receiver(const TestContext&) noexcept {
    ipc::Notification first{};
    ipc::Notification second{};
    TestSource source{};
    const auto attached = first.bind(source.sc(), 8);
    const auto duplicate = second.bind(source.sc(), 16);
    source.sc().reset();
    return attached && !duplicate
        && duplicate.error() == ipc::NotificationError::Busy;
}

bool test_notification_badge_is_immutable_authority(
    const TestContext&) noexcept {
    const auto rights = cap::Rights::of(
        cap::Right::Signal);
    const cap::View ceiling{
        rights, cap::Badge{32}};
    const auto exact = cap::compose(
        object::ObjectKind::Notification,
        ceiling,
        cap::View{
            rights, cap::Badge{32}});
    const auto changed = cap::compose(
        object::ObjectKind::Notification,
        ceiling,
        cap::View{
            rights, cap::Badge{64}});
    return exact && !changed
        && changed.error() == cap::PolicyError::Amplification;
}

bool test_endpoint_authority_narrows_badge_and_limits(
    const TestContext&) noexcept {
    using cap::View;
    using cap::EpLimit;
    using cap::View;
    using cap::PolicyError;
    using cap::Right;
    using cap::Rights;
    using object::ObjectKind;

    const auto rights = Rights::of(
        Right::Delegate, Right::Call, Right::Inspect);
    const EpLimit root{
        .badge = 0,
        .fixed = 0,
        .cap_limit = ENDPOINT_MAX_CAPS,
    };
    const EpLimit caller{
        .badge = 0x42,
        .fixed = ~u64{},
        .cap_limit = 2,
    };
    const auto narrowed = cap::compose(
        ObjectKind::Endpoint,
        View{rights, root},
        View{rights, caller});
    const auto widened_caps = cap::compose(
        ObjectKind::Endpoint,
        View{rights, caller},
        View{rights, EpLimit{
            .badge = 0x42,
            .fixed = ~u64{},
            .cap_limit = 3,
        }});
    const auto changed_badge = cap::compose(
        ObjectKind::Endpoint,
        View{rights, caller},
        View{rights, EpLimit{
            .badge = 0x43,
            .fixed = ~u64{},
            .cap_limit = 2,
        }});
    return narrowed
        && std::get<EpLimit>(narrowed.value().data).callable()
        && !widened_caps && widened_caps.error() == PolicyError::Amplification
        && !changed_badge && changed_badge.error() == PolicyError::Amplification;
}

bool test_channel_root_cannot_fix_badge_generically(
    const TestContext&) noexcept {
    using cap::View;
    using cap::ChanLimit;
    using cap::ChannelSide;
    using cap::View;
    using cap::PolicyError;
    using cap::Right;
    using cap::Rights;
    using object::ObjectKind;

    const auto rights = Rights::of(
        Right::Duplicate, Right::Delegate, Right::Inspect,
        Right::Send, Right::Receive, Right::Close, Right::Revoke);
    const ChanLimit root{
        .side = ChannelSide::Any,
        .badge = 0,
        .fixed = 0,
    };
    const auto side = cap::compose(
        ObjectKind::Channel,
        View{rights, root},
        View{rights, ChanLimit{
            .side = ChannelSide::A,
            .badge = 0,
            .fixed = 0,
        }});
    const auto exact = cap::compose(
        ObjectKind::Channel,
        View{rights, root},
        View{rights, ChanLimit{
            .side = ChannelSide::A,
            .badge = 0x55,
            .fixed = ~u64{},
        }});
    return side && !exact && exact.error() == PolicyError::Amplification;
}

bool test_channel_badge_and_side_are_immutable(
    const TestContext&) noexcept {
    using cap::View;
    using cap::ChanLimit;
    using cap::ChannelSide;
    using cap::View;
    using cap::PolicyError;
    using cap::Right;
    using cap::Rights;
    using object::ObjectKind;

    const auto rights = Rights::of(
        Right::Duplicate, Right::Delegate, Right::Inspect,
        Right::Send, Right::Receive, Right::Close, Right::Revoke);
    const ChanLimit exact{
        .side = ChannelSide::A,
        .badge = 0x55,
        .fixed = ~u64{},
    };
    const auto same = cap::compose(
        ObjectKind::Channel,
        View{rights, exact}, View{rights, exact});
    const auto changed_badge = cap::compose(
        ObjectKind::Channel,
        View{rights, exact},
        View{rights, ChanLimit{
            .side = ChannelSide::A,
            .badge = 0x56,
            .fixed = ~u64{},
        }});
    const auto changed_side = cap::compose(
        ObjectKind::Channel,
        View{rights, exact},
        View{rights, ChanLimit{
            .side = ChannelSide::B,
            .badge = 0x55,
            .fixed = ~u64{},
        }});
    return same && !changed_badge
        && changed_badge.error() == PolicyError::Amplification
        && !changed_side
        && changed_side.error() == PolicyError::Amplification;
}

} // namespace

static bool test_irq_sequence_reassert_requires_latest_ack(const TestContext &) noexcept {
    ipc::Notification notification{};
    ipc::Notification rebound_notification{};
    irq::Routes routes{};
    irq::Irq irq{{routes, 10}};
    auto take = []() noexcept -> u32 { return 10; };
    if (!irq.bind(notification, 0x40)) {
        return false;
    }
    routes.dispatch(irq::Routes::Take::bind(take));
    const auto first = irq.delivery();
    const auto first_notice = notification.take();
    routes.dispatch(irq::Routes::Take::bind(take));
    const auto second = irq.delivery();
    if (!first || !second || !first_notice ||
        irq.ack(first.value().generation, first.value().sequence)) {
        return false;
    }
    if (!irq.ack(second.value().generation, second.value().sequence) || !notification.take() ||
        !irq.unbind() || (irq.bound() || irq.pending()) || !irq.bind(notification, 0x40)) {
        return false;
    }
    routes.dispatch(irq::Routes::Take::bind(take));
    const auto pending = irq.delivery();
    if (!pending || !notification.take() || !irq.unbind() || (irq.bound() || !irq.pending())) {
        return false;
    }
    if (!irq.bind(rebound_notification, 0x40) || (!irq.bound() || !irq.pending())) {
        return false;
    }
    const auto retained = irq.delivery();
    if (!retained || retained.value().sequence != pending.value().sequence ||
        retained.value().generation == pending.value().generation || !rebound_notification.take()) {
        return false;
    }
    const auto stale = irq.ack(pending.value().generation, pending.value().sequence);
    if (stale || stale.error() != irq::Error::StaleSequence) {
        return false;
    }
    return irq.ack(retained.value().generation, retained.value().sequence) && irq.bound() &&
           !irq.pending() && irq.unbind() && !irq.bound() && !irq.pending();
}

static bool test_exit_notification_is_read_only(const TestContext &) noexcept {
    ipc::Notification notification{};
    Exit first{}, second{};
    if (!first.observe(notification, 0x80) || first.observe(notification, 0x40) ||
        !second.claim(Exit::Reason::Normal, STATUS_OK) ||
        !second.observe(notification, 0x40) ||
        !first.claim(Exit::Reason::Stop, STATUS_CANCELED, 7))
        return false;
    const auto event = notification.take();
    const auto result = first.read();
    return event && event.value().badges == 0xc0 && result.reason == Exit::Reason::Stop &&
           result.status == STATUS_CANCELED && result.detail == 7 &&
           !first.claim(Exit::Reason::Fault, STATUS_INTERNAL) && first.published();
}

void register_ipc_tests(TestRegistry& registry) noexcept {
    (void)registry.add("ipc", "IRQ acknowledgement rejects stale sequence",
        test_irq_sequence_reassert_requires_latest_ack);
    (void)registry.add("ipc", "Exit notification observes one result",
        test_exit_notification_is_read_only);

    (void)registry.add(
        "ipc",
        "Notification ORs badges and one take wins the pending state",
        test_notification_badges_are_coalesced);
    (void)registry.add(
        "ipc",
        "Notification source rearm preserves level readiness",
        test_notification_source_rearm_preserves_level);
    (void)registry.add(
        "ipc",
        "Notification source has one receiver-owned binding",
        test_notification_source_has_one_receiver);
    (void)registry.add(
        "ipc",
        "Notification authority cannot rewrite its fixed badge",
        test_notification_badge_is_immutable_authority);
    (void)registry.add(
        "ipc",
        "Endpoint authority fixes caller badge and only narrows admission",
        test_endpoint_authority_narrows_badge_and_limits);
    (void)registry.add(
        "ipc",
        "Channel side root cannot generically choose a sender badge",
        test_channel_root_cannot_fix_badge_generically);
    (void)registry.add(
        "ipc",
        "Channel badge and side remain immutable after mint",
        test_channel_badge_and_side_are_immutable);
}
