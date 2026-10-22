#include <task/thread.hpp>
#include <irq/irq.hpp>
#include <test/test.hpp>

#include <cap/cap.hpp>
#include <ipc/notification.hpp>
#include <uapi/ipc.h>

namespace {

bool test_notification_badges_are_coalesced(const TestContext&) noexcept {
    ipc::Notification notification{};
    if (!notification.signal(1) || !notification.signal(2)
        || !notification.signal(1)) {
        return false;
    }
    const auto first = notification.events().take();
    const auto second = notification.events().take();
    return first.status == STATUS_OK && first.value == 3 && first.sequence == 3
        && second.status == STATUS_RETRY;
}

bool test_notification_badge_is_immutable_authority(
    const TestContext&) noexcept {
    const auto rights = cap::Rights::of(
        cap::Right::Signal);
    const cap::View ceiling{
        rights, cap::Badge{32}};
    const auto exact = cap::compose(
        ObjNotification,
        ceiling,
        cap::View{
            rights, cap::Badge{32}});
    const auto changed = cap::compose(
        ObjNotification,
        ceiling,
        cap::View{
            rights, cap::Badge{64}});
    return exact && !changed
        && changed.error() == cap::PolicyError::Amplification;
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

    const auto rights = Rights::of(
        Right::Duplicate, Right::Delegate, Right::Inspect,
        Right::Send, Right::Receive, Right::Close, Right::Revoke);
    const ChanLimit root{
        .side = ChannelSide::Any,
        .badge = 0,
        .fixed = 0,
    };
    const auto side = cap::compose(
        ObjChannel,
        View{rights, root},
        View{rights, ChanLimit{
            .side = ChannelSide::A,
            .badge = 0,
            .fixed = 0,
        }});
    const auto exact = cap::compose(
        ObjChannel,
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

    const auto rights = Rights::of(
        Right::Duplicate, Right::Delegate, Right::Inspect,
        Right::Send, Right::Receive, Right::Close, Right::Revoke);
    const ChanLimit exact{
        .side = ChannelSide::A,
        .badge = 0x55,
        .fixed = ~u64{},
    };
    const auto same = cap::compose(
        ObjChannel,
        View{rights, exact}, View{rights, exact});
    const auto changed_badge = cap::compose(
        ObjChannel,
        View{rights, exact},
        View{rights, ChanLimit{
            .side = ChannelSide::A,
            .badge = 0x56,
            .fixed = ~u64{},
        }});
    const auto changed_side = cap::compose(
        ObjChannel,
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

static bool test_irq_sequence_reassert_requires_latest_ack(const TestContext&) noexcept {
    irq::Routes routes;
    irq::Irq irq{{routes, 10}};
    auto take = []() noexcept -> u32 { return 10; };
    if (!irq.enable(true)) return false;
    routes.dispatch(irq::Routes::Take::bind(take));
    const auto first = irq.delivery();
    if (!first || irq.events().snapshot().value != SOURCE_READY) return false;
    routes.dispatch(irq::Routes::Take::bind(take));
    const auto second = irq.delivery();
    if (!second || irq.ack(first->generation, first->sequence)
        || !irq.ack(second->generation, second->sequence) || irq.events().snapshot().value) return false;
    routes.dispatch(irq::Routes::Take::bind(take));
    const auto pending = irq.delivery();
    if (!pending || !irq.enable(false) || irq.bound() || !irq.pending() || !irq.enable(true)) return false;
    const auto retained = irq.delivery();
    if (!retained || retained->sequence != pending->sequence || retained->generation == pending->generation
        || irq.events().snapshot().value != SOURCE_READY || irq.ack(pending->generation, pending->sequence)) return false;
    return bool(irq.ack(retained->generation, retained->sequence)) && !irq.pending() && bool(irq.enable(false));
}

static bool test_exit_result_is_immutable(const TestContext&) noexcept {
    Thread::Exit result;
    return result.claim(STATUS_CANCELED) && result.read() == STATUS_CANCELED
        && !result.claim(STATUS_INTERNAL) && result.published()
        && result.events().snapshot().value == SOURCE_READY;
}

void register_ipc_tests(TestRegistry& registry) noexcept {
    (void)registry.add("ipc", "bounded source preserves inline and deferred pulses",
        +[](const TestContext&) noexcept -> bool {
            WorkQueue work;
            Src<SOURCE_READY> source;
            source.bind_work(work, {});
            usize ready{}, closed{};
            auto receive = [&](WaitResult value) noexcept {
                if (value.status == STATUS_CLOSED) ++closed;
                else if (value.status == STATUS_OK && value.value == SOURCE_READY) ++ready;
            };
            auto drained = []() noexcept {};
            const auto deliver = libk::delegate<void(WaitResult) noexcept>::bind(receive);
            const auto done = Source::Drained::bind(drained);
            std::array<std::optional<Source::Watch>, 20> watches;
            for (auto& watch : watches) watch.emplace(deliver, done);
            libk::scope_exit cleanup{[&]() noexcept {
                for (auto& watch : watches) watch->detach();
                source.close();
                while (work.run()) {}
            }};
            auto install = [](auto commit) noexcept { commit(); return STATUS_OK; };
            for (auto& watch : watches)
                if (source.watch(*watch, {SOURCE_READY}, install) != STATUS_OK) return false;
            source.post(SOURCE_READY);
            if (ready != 8) return false;
            source.update(0);
            // Re-arm in place before the queued scan reaches this middle item.
            if (source.arm(*watches[18], {SOURCE_READY}) != STATUS_OK) return false;
            while (work.run()) {}
            if (ready != 19 || closed || source.snapshot().value) return false;
            source.close();
            while (work.run()) {}
            return ready == 19 && closed == watches.size();
        });
    (void)registry.add("ipc", "IRQ acknowledgement rejects stale sequence",
        test_irq_sequence_reassert_requires_latest_ack);
    (void)registry.add("ipc", "Exit result and readiness are immutable",
        test_exit_result_is_immutable);

    (void)registry.add(
        "ipc",
        "Notification ORs badges and one take wins the pending state",
        test_notification_badges_are_coalesced);
    (void)registry.add(
        "ipc",
        "Notification authority cannot rewrite its fixed badge",
        test_notification_badge_is_immutable_authority);
    (void)registry.add(
        "ipc",
        "Channel side root cannot generically choose a sender badge",
        test_channel_root_cannot_fix_badge_generically);
    (void)registry.add(
        "ipc",
        "Channel badge and side remain immutable after mint",
        test_channel_badge_and_side_are_immutable);
}
