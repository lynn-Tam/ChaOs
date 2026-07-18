#include <test/test.hpp>

#include <cap/policy.hpp>
#include <ipc/notification.hpp>

namespace {

class TestSource final {
public:
    TestSource() noexcept
        : binding_(kernel::ipc::NotificationSource::bind<
              TestSource, &TestSource::closed>(*this)) {}

    ~TestSource() noexcept { binding_.reset(); }

    [[nodiscard]] auto binding() noexcept
        -> kernel::ipc::NotificationSource& {
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

    kernel::ipc::NotificationSource binding_;
    u64 sequence_{};
    bool ready_{};
    bool closed_{};
};

bool test_notification_badges_are_coalesced(const TestContext&) noexcept {
    kernel::ipc::Notification notification{};
    if (!notification.signal(1) || !notification.signal(2)
        || !notification.signal(1)) {
        return false;
    }
    const auto first = notification.take();
    const auto second = notification.take();
    return first && first.value() == 3
        && !second
        && second.error() == kernel::ipc::NotificationError::Empty;
}

bool test_notification_source_rearm_preserves_level(
    const TestContext&) noexcept {
    kernel::ipc::Notification notification{};
    TestSource source{};
    if (!notification.bind(source.binding(), 4)) {
        return false;
    }

    source.publish();
    const auto initial = notification.take();
    if (!initial || initial.value() != 4) {
        return false;
    }

    const u64 observed = source.consume();
    // A new readiness transition in the consume/rearm window must be
    // projected again even if an earlier badge was already taken.
    source.publish();
    source.rearm(observed);
    const auto replayed = notification.take();
    source.binding().reset();
    return replayed && replayed.value() == 4
        && !source.binding().attached() && !source.was_closed();
}

bool test_notification_source_has_one_receiver(const TestContext&) noexcept {
    kernel::ipc::Notification first{};
    kernel::ipc::Notification second{};
    TestSource source{};
    const auto attached = first.bind(source.binding(), 8);
    const auto duplicate = second.bind(source.binding(), 16);
    source.binding().reset();
    return attached && !duplicate
        && duplicate.error() == kernel::ipc::NotificationError::Busy;
}

bool test_notification_badge_is_immutable_authority(
    const TestContext&) noexcept {
    const auto rights = kernel::cap::Rights::of(
        kernel::cap::Right::Signal);
    const kernel::cap::GrantCeiling ceiling{
        rights, kernel::cap::NotificationAuthority{32}};
    const auto exact = kernel::cap::compose(
        kernel::object::ObjectKind::Notification,
        ceiling,
        kernel::cap::CapView{
            rights, kernel::cap::NotificationAuthority{32}});
    const auto changed = kernel::cap::compose(
        kernel::object::ObjectKind::Notification,
        ceiling,
        kernel::cap::CapView{
            rights, kernel::cap::NotificationAuthority{64}});
    return exact && !changed
        && changed.error() == kernel::cap::PolicyError::Amplification;
}

} // namespace

void register_ipc_tests(TestRegistry& registry) noexcept {
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
}
