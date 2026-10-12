#include <sys/protocol.hpp>
#include <utility>
#include <test/user/channel/export_protocol.hpp>
#include <sys/clock.hpp>
#include <test/user/task.hpp>

namespace {
using namespace sys;
proc::package program;
using Supervisor = proc::tasks<4>;
Supervisor supervisor;
unsigned step{};
void expect(status_t status) noexcept {
    ++step;
    // Fixture exit code retains the failed check and original status.
    if (status != STATUS_OK) exit(-static_cast<status_t>(step * 100) + status);
}
void check(bool value) noexcept { expect(value ? STATUS_OK : STATUS_INTERNAL); }
void destroy(SysResult pair) noexcept {
    expect(object_destroy(pair.value).status);
    expect(cap_close(pair.value).status);
    expect(cap_close(pair.value2).status);
}
void configured_capacity(cap_t pool, cap_t cspace) noexcept {
    constexpr unsigned depth = 33, bindings = 8;
    const auto pair = channel_create(pool, depth, CHANNEL_MAX_WORDS, 0, bindings);
    expect(pair.status);
    const auto sender = channel_mint(pair.value, cspace, 1, RIGHT_SEND);
    expect(sender.status);
    cap_owner events[bindings];
    word_t relations[bindings];
    for (unsigned i = 0; i != bindings; ++i) {
        const auto event = notification_create(pool, 1);
        expect(event.status);
        events[i] = cap_owner{event.value};
        const auto bound = channel_bind(pair.value2, event.value, CHANNEL_READABLE);
        expect(bound.status);
        relations[i] = bound.value;
    }
    for (unsigned round = 0; round != 3; ++round) {
        for (unsigned i = 0; i != depth; ++i)
            expect(sys::send(sender.value, {.id = i}, false).status);
        check(sys::send(sender.value, {.id = depth}, false).status == STATUS_WOULD_BLOCK);
        for (auto& event : events) expect(notification_take(event.selector()).status);
        sys::Message message;
        for (unsigned i = 0; i != depth; ++i) {
            expect(sys::receive(pair.value2, message, false).status);
            check(message.id == i);
        }
        check(sys::receive(pair.value2, message, false).status == STATUS_WOULD_BLOCK);
        for (unsigned i = 0; i != bindings; ++i) {
            const auto arm = channel_arm(pair.value2, relations[i], 0);
            expect(arm.status);
            expect(notification_take(events[i].selector()).status);
            expect(channel_arm(pair.value2, relations[i], arm.value).status);
        }
    }
    expect(cap_close(sender.value).status);
    destroy(pair);
    for (auto& event : events) expect(object_destroy(event.selector()).status);
}

// An exported endpoint queued in an abandoned reply has no future holder.
// Dropping that escrow must notify its provider, even without explicit Close.
void abandoned_reply(cap_t pool, cap_t cspace, cap_t events) noexcept {
    Channel provider, reply;
    expect(provider.open(pool, 1, 0, 1));
    expect(reply.open(pool, 1, 1, 1));
    const auto exported = channel_mint(provider.ends[0].selector(), cspace, 1,
        RIGHT_SEND | RIGHT_DUPLICATE);
    expect(exported.status);
    cap_owner endpoint{exported.value};
    expect(provider.ends[0].close());
    const auto minted = channel_mint(reply.ends[1].selector(), cspace, 1, RIGHT_SEND);
    expect(minted.status);
    cap_owner sender{minted.value};
    const CapXfer cap{endpoint.selector(), RIGHT_SEND, CAP_COPY, 0};
    expect(sys::send(sender.selector(), {}, false, {&cap, 1}).status);
    expect(endpoint.close());
    Port peer{provider.ends[1].selector(), events};
    expect(reply.close());
    Clock clock;
    expect(clock.open());
    const auto deadline = clock.after_ms(1'000);
    check(deadline.has_value());
    for (;;) {
        Message msg;
        const auto r = peer.try_receive(msg);
        if (r.status == STATUS_PEER_CLOSED) break;
        check(r.status == STATUS_WOULD_BLOCK);
        expect(peer.arm().status);
        expect(notification_wait(events, *deadline).status);
    }
}

void stale_channel(cap_t pool, cap_t cspace) noexcept {
    const auto old_pool = resource_create_child(pool, 256 * 1024, 64,
        proc::base_kinds | OBJ_BIT(ObjChannel));
    expect(old_pool.status);
    const auto ended = notification_create(pool, 1);
    expect(ended.status);
    const auto old = channel_create(old_pool.value, 1, CHANNEL_MAX_WORDS, 0, 1);
    expect(old.status);
    const auto old_sender = channel_mint(old.value, cspace, 1, RIGHT_SEND);
    expect(old_sender.status);
    expect(sys::send(old_sender.value, {.id = 1}).status);
    sys::Message message;
    expect(sys::receive(old.value2, message).status);
    check(message.id == 1);
    expect(resource_close_async(old_pool.value, ended.value, 1).status);
    expect(notification_wait(ended.value).status);

    const auto current = channel_create(pool, 1, CHANNEL_MAX_WORDS, 0, 1);
    expect(current.status);
    const auto current_sender = channel_mint(current.value, cspace, 1, RIGHT_SEND);
    expect(current_sender.status);
    check(sys::send(old_sender.value, {.id = 2}, false).status == STATUS_BUSY);
    expect(cap_close(old_sender.value).status);
    check(sys::send(old_sender.value, {.id = 2}, false).status == STATUS_INVALID_CAP);
    expect(sys::send(current_sender.value, {.id = 3}).status);
    expect(sys::receive(current.value2, message).status);
    check(message.id == 3);
    check(sys::receive(current.value2, message, false).status == STATUS_WOULD_BLOCK);
    expect(cap_close(old.value).status);
    expect(cap_close(old.value2).status);
    expect(cap_close(old_pool.value).status);
    expect(object_destroy(ended.value).status);
    expect(cap_close(ended.value).status);
    expect(cap_close(current_sender.value).status);
    destroy(current);
}

void stale_service(cap_t pool, cap_t cspace) noexcept {
    const auto handoff = channel_create(pool, 4, CHANNEL_MAX_WORDS, 1, 1);
    expect(handoff.status);
    const auto control = channel_mint(handoff.value2, cspace, 1,
        RIGHT_SEND | RIGHT_RECEIVE);
    expect(control.status);
    const proc::cap_src source[]{input(boot::Stdout, handoff.value,
        RIGHT_SEND | RIGHT_RECEIVE, 1)};
    auto next_cap = [&]() noexcept -> cap_owner {
        const auto begin = clock_now();
        expect(begin.status);
        for (;;) {
            sys::Message message{};
            cap_owner received;
            const auto result = sys::receive_cap(control.value, message, received);
            if (result.status == STATUS_WOULD_BLOCK) {
                const auto now = clock_now();
                expect(now.status);
                check(now.value - begin.value < 1'000'000'000);
                yield();
                continue;
            }
            expect(result.status);
            check(message.id == 1 && static_cast<bool>(received));
            return received;
        }
    };
    const proc::quota quota{.bytes = 1024 * 1024, .caps = 128,
        .kinds = proc::base_kinds | OBJ_BIT(ObjChannel)};
    auto launch = [&]() noexcept {
        return start(supervisor, program, "provider", {.caps = source, .resources = quota});
    };
    struct Transfer final { cap_owner cap; proc::task_id holder; };
    auto transfer = [&]() noexcept -> Transfer {
        auto exported = next_cap();
        const proc::cap_src caps[]{source[0], input(channel_test::Provider, exported.selector(),
            RIGHT_SEND | RIGHT_DUPLICATE)};
        const auto holder = start(supervisor, program, "export-holder", {.caps = caps, .resources = quota});
        return {next_cap(), holder};
    };

    auto first = launch();
    auto old = transfer();
    expect(sys::send(old.cap.selector(), {.id = 41}).status);
    sys::Message message{};
    expect(sys::receive(control.value, message).status);
    check(message.id == 4);
    expect(sys::send(control.value, {.id = 3}).status);
    check(wait(supervisor, first) == STATUS_INTERNAL);
    check(stop(supervisor, old.holder) == STATUS_CANCELED);

    auto second = launch();
    auto current = transfer();
    check(sys::send(old.cap.selector(), {.id = 42}, false).status == STATUS_BUSY);
    expect(sys::send(current.cap.selector(), {.id = 41}).status);
    expect(sys::receive(control.value, message).status);
    check(message.id == 4);
    expect(sys::send(control.value, {.id = 3}).status);
    check(wait(supervisor, second) == STATUS_INTERNAL);
    check(stop(supervisor, current.holder) == STATUS_CANCELED);
    expect(cap_close(control.value).status);
    destroy(handoff);
}

}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    const auto info = sys::bootstrap(address, size);
    supervisor.open(info);
    open(program, info);
    const auto pool = sys::capability(info, BOOT_POOL);
    configured_capacity(pool, sys::capability(info, BOOT_CSPACE));
    abandoned_reply(pool, info.selector(BOOT_CSPACE), info.selector(BOOT_EVENTS));
    stale_channel(pool, sys::capability(info, BOOT_CSPACE));
    stale_service(pool, sys::capability(info, BOOT_CSPACE));
    Clock clock;
    expect(clock.open());
    for (unsigned round = 0; round != 9; ++round) {
        const auto data = channel_create(pool, 1, CHANNEL_MAX_WORDS, 0, 1);
        const auto ready = channel_create(pool, 4, CHANNEL_MAX_WORDS, 0, 1);
        const auto ended = notification_create(pool, 1);
        expect(data.status); expect(ready.status); expect(ended.status);
        const auto sender = channel_mint(data.value,
            sys::capability(info, BOOT_CSPACE), 99, RIGHT_SEND);
        expect(sender.status);
        expect(sys::send(sender.value, {.id = 100}).status);
        expect(cap_close(sender.value).status);
        const proc::cap_src sources[] = {input(boot::Stdout, data.value, RIGHT_SEND, 99), input(boot::Stderr, ready.value, RIGHT_SEND, 1)};
        proc::task_id tasks[3];
        for (unsigned i = 0; i != 3; ++i) {
            boot::args arguments;
            const char number = '0' + i;
            check(arguments.append("writer", 6) && arguments.append(&number, 1));
            tasks[i] = start(supervisor, program, "writer", {.args = &arguments, .caps = sources,
                .resources = {.bytes = 1024 * 1024, .caps = 128,
                    .kinds = proc::base_kinds | OBJ_BIT(ObjChannel)}, .events = ended.value});
        }
        unsigned entered{};
        for (unsigned i = 0; i != 3; ++i) {
            sys::Message message;
            expect(sys::receive(ready.value2, message).status);
            check(message.id < 3 && !(entered & (1U << message.id)));
            entered |= 1U << message.id;
        }
        // The data queue is still full. A single-slot implementation makes
        // later senders exit BUSY here instead of retaining all three waits.
        const auto deadline = clock.after_ms(30);
        check(static_cast<bool>(deadline));
        check(notification_wait(ended.value, *deadline).status == STATUS_TIMED_OUT);
        const unsigned mode = round % 3;
        if (mode == 1) check(stop(supervisor, tasks[1]) == STATUS_CANCELED);
        if (mode == 2) expect(channel_close(data.value2).status);
        else {
            sys::Message message;
            expect(sys::receive(data.value2, message).status);
            check(message.id == 100);
            unsigned received{};
            for (unsigned i = 0; i != (mode == 1 ? 2U : 3U); ++i) {
                expect(sys::receive(data.value2, message).status);
                check(message.id < 3 && !(received & (1U << message.id)));
                received |= 1U << message.id;
            }
            check(received == (mode == 1 ? 5U : 7U));
        }
        for (unsigned i = 0; i != 3; ++i) {
            if (mode == 1 && i == 1) continue;
            check(wait(supervisor, tasks[i]) == (mode == 2 ? STATUS_PEER_CLOSED : STATUS_OK));
        }
        destroy(data); destroy(ready);
        expect(object_destroy(ended.value).status);
        expect(cap_close(ended.value).status);
    }
    exit();
}
