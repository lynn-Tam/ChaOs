#include <protocols/msg.hpp>
#include <protocols/stream.hpp>
#include <utility>
#include <test/user/channel/export_protocol.hpp>
#include <util.hpp>
#include <test/user/task.hpp>

static test_image program;
using Supervisor = test_tasks;
Supervisor supervisor;
unsigned step{};
static void expect(status_t status) noexcept {
    ++step;
    // Fixture exit code retains the failed check and original status.
    if (status != STATUS_OK)
        sys::exit(-static_cast<status_t>(step * 100) + status);
}
static void check(bool value) noexcept {
    expect(value ? STATUS_OK : STATUS_INTERNAL);
}
static void destroy(sys::SysResult pair) noexcept {
    expect(sys::object_destroy(pair.value).status);
    expect(sys::cap_close(pair.value).status);
    expect(sys::cap_close(pair.value2).status);
}
static void configured_capacity(cap_t pool, cap_t cspace) noexcept {
    constexpr unsigned depth = 33, bindings = 8;
    const auto pair = sys::channel_create(pool, depth, CHANNEL_MAX_WORDS, 0, bindings);
    expect(pair.status);
    const auto sender = sys::channel_mint(pair.value, cspace, 1, RIGHT_SEND);
    expect(sender.status);
    cap_owner events[bindings];
    word_t relations[bindings];
    for (unsigned i = 0; i != bindings; ++i) {
        const auto event = sys::notification_create(pool, 1);
        expect(event.status);
        events[i] = cap_owner{event.value};
        const auto bound = sys::channel_bind(pair.value2, event.value, CHANNEL_READABLE);
        expect(bound.status);
        relations[i] = bound.value;
    }
    for (unsigned round = 0; round != 3; ++round) {
        for (unsigned i = 0; i != depth; ++i)
            expect(msg_send(sender.value, {.value = i}, false).status);
        check(msg_send(sender.value, {.value = depth}, false).status == STATUS_WOULD_BLOCK);
        for (auto& event : events)
            expect(sys::notification_take(event.selector()).status);
        msg_frame message;
        for (unsigned i = 0; i != depth; ++i) {
            expect(msg_recv(pair.value2, message, false).status);
            check(message.value == i);
        }
        check(msg_recv(pair.value2, message, false).status == STATUS_WOULD_BLOCK);
        for (unsigned i = 0; i != bindings; ++i) {
            const auto arm = sys::channel_arm(pair.value2, relations[i], 0);
            expect(arm.status);
            expect(sys::notification_take(events[i].selector()).status);
            expect(sys::channel_arm(pair.value2, relations[i], arm.value).status);
        }
    }
    const std::array<word_t, 3> words{17, 23, 41};
    expect(ch_send(sender.value, words, false, {}, 0x123).status);
    ch_packet raw;
    expect(ch_recv(pair.value2, raw, false).status);
    check(raw.word_count == words.size() && raw.count == 0 && raw.badge == 1 && raw.tag == 0x123);
    check(std::equal(words.begin(), words.end(), raw.words.begin()));
    expect(ch_send(sender.value, std::span{words}.first(1), false).status);
    msg_frame malformed;
    check(msg_recv(pair.value2, malformed, false).status == STATUS_BAD_ARGS);
    expect(sys::cap_close(sender.value).status);
    destroy(pair);
    for (auto& event : events)
        expect(sys::object_destroy(event.selector()).status);
}

// An exported endpoint queued in an abandoned reply has no future holder.
// Dropping that escrow must notify its provider, even without explicit Close.
static void abandoned_reply(cap_t pool, cap_t cspace, cap_t events) noexcept {
    ch_pair provider, reply;
    expect(provider.open(pool, 1, 0, 1));
    expect(reply.open(pool, 1, 1, 1));
    const auto exported =
        sys::channel_mint(provider.ends[0].selector(), cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
    expect(exported.status);
    cap_owner endpoint{exported.value};
    expect(provider.ends[0].close());
    const auto minted = sys::channel_mint(reply.ends[1].selector(), cspace, 1, RIGHT_SEND);
    expect(minted.status);
    cap_owner sender{minted.value};
    const CapXfer cap{endpoint.selector(), RIGHT_SEND, CAP_COPY, 0};
    expect(msg_send(sender.selector(), {}, false, {&cap, 1}).status);
    expect(endpoint.close());
    msg_rx peer{provider.ends[1].selector(), events};
    expect(reply.close());

    const auto deadline = rt_after_ms(1'000);
    check(deadline.has_value());
    for (;;) {
        msg_frame msg;
        const auto r = peer.try_receive(msg);
        if (r.status == STATUS_PEER_CLOSED)
            break;
        check(r.status == STATUS_WOULD_BLOCK);
        expect(peer.arm().status);
        expect(sys::notification_wait(events, *deadline).status);
    }
}

static void stale_channel(cap_t pool, cap_t cspace) noexcept {
    const auto old_pool =
        sys::resource_create_child(pool, 256 * 1024, 64, pr_base_kinds | OBJ_BIT(ObjChannel));
    expect(old_pool.status);
    const auto ended = sys::notification_create(pool, 1);
    expect(ended.status);
    const auto old = sys::channel_create(old_pool.value, 1, CHANNEL_MAX_WORDS, 0, 1);
    expect(old.status);
    const auto old_sender = sys::channel_mint(old.value, cspace, 1, RIGHT_SEND);
    expect(old_sender.status);
    expect(msg_send(old_sender.value, {.value = 1}).status);
    msg_frame message;
    expect(msg_recv(old.value2, message).status);
    check(message.value == 1);
    expect(sys::resource_close_async(old_pool.value, ended.value, 1).status);
    expect(sys::notification_wait(ended.value).status);

    const auto current = sys::channel_create(pool, 1, CHANNEL_MAX_WORDS, 0, 1);
    expect(current.status);
    const auto current_sender = sys::channel_mint(current.value, cspace, 1, RIGHT_SEND);
    expect(current_sender.status);
    check(msg_send(old_sender.value, {.value = 2}, false).status == STATUS_BUSY);
    expect(sys::cap_close(old_sender.value).status);
    check(msg_send(old_sender.value, {.value = 2}, false).status == STATUS_INVALID_CAP);
    expect(msg_send(current_sender.value, {.value = 3}).status);
    expect(msg_recv(current.value2, message).status);
    check(message.value == 3);
    check(msg_recv(current.value2, message, false).status == STATUS_WOULD_BLOCK);
    expect(sys::cap_close(old.value).status);
    expect(sys::cap_close(old.value2).status);
    expect(sys::cap_close(old_pool.value).status);
    expect(sys::object_destroy(ended.value).status);
    expect(sys::cap_close(ended.value).status);
    expect(sys::cap_close(current_sender.value).status);
    destroy(current);
}

static void stale_service(cap_t pool, cap_t cspace) noexcept {
    const auto handoff = sys::channel_create(pool, 4, CHANNEL_MAX_WORDS, 1, 1);
    expect(handoff.status);
    const auto control = sys::channel_mint(handoff.value2, cspace, 1, RIGHT_SEND | RIGHT_RECEIVE);
    expect(control.status);
    const pr_cap source[]{input(rt_stdout, handoff.value, RIGHT_SEND | RIGHT_RECEIVE, 1)};
    auto next_cap = [&]() noexcept -> cap_owner {
        const auto begin = sys::clock_now();
        expect(begin.status);
        for (;;) {
            msg_frame message{};
            cap_owner received;
            const auto result = msg_recv(control.value, message, false, {&received, 1});
            if (result.status == STATUS_WOULD_BLOCK) {
                const auto now = sys::clock_now();
                expect(now.status);
                check(now.value - begin.value < 1'000'000'000);
                sys::yield();
                continue;
            }
            expect(result.status);
            check(message.value == 1 && static_cast<bool>(received));
            return received;
        }
    };
    const pr_quota quota{
        .bytes = 1024 * 1024, .caps = 128, .kinds = pr_base_kinds | OBJ_BIT(ObjChannel)};
    auto launch = [&]() noexcept {
        return start(supervisor, program, "provider", {.caps = source, .resources = quota});
    };
    struct Transfer final {
        cap_owner cap;
        test_id holder;
    };
    auto transfer = [&]() noexcept -> Transfer {
        auto exported = next_cap();
        const pr_cap caps[]{source[0],
                            input(ct_provider, exported.selector(), RIGHT_SEND | RIGHT_DUPLICATE)};
        const auto holder =
            start(supervisor, program, "export-holder", {.caps = caps, .resources = quota});
        return {next_cap(), holder};
    };

    auto first = launch();
    auto old = transfer();
    expect(msg_send(old.cap.selector(), {.value = 41}).status);
    msg_frame message{};
    expect(msg_recv(control.value, message).status);
    check(message.value == 4);
    expect(msg_send(control.value, {.value = 3}).status);
    check(wait(supervisor, first) == STATUS_INTERNAL);
    check(stop(supervisor, old.holder) == STATUS_CANCELED);

    auto second = launch();
    auto current = transfer();
    check(msg_send(old.cap.selector(), {.value = 42}, false).status == STATUS_BUSY);
    expect(msg_send(current.cap.selector(), {.value = 41}).status);
    expect(msg_recv(control.value, message).status);
    check(message.value == 4);
    expect(msg_send(control.value, {.value = 3}).status);
    check(wait(supervisor, second) == STATUS_INTERNAL);
    check(stop(supervisor, current.holder) == STATUS_CANCELED);
    expect(sys::cap_close(control.value).status);
    destroy(handoff);
}

// The provider discards its sole reply capability. The caller must observe
// closure without a synthetic timeout or a retained sender root of its own.
static void rpc_abandon(const rt_env& env) {
    ch_pair request;
    expect(request.open(env.pool, 1, 1, 1));
    const auto minted = sys::channel_mint(request.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
    expect(minted.status);
    cap_owner sender{minted.value};
    const pr_cap caps[]{input(rt_stdout, request.ends[1].selector(), RIGHT_RECEIVE, 1)};
    const std::string_view args[]{"provider", "abandon"};
    const auto child = start(supervisor, program, "provider",
                             {.argv = args,
                              .caps = caps,
                              .resources = {.bytes = 1024 * 1024,
                                            .caps = 128,
                                            .kinds = pr_base_kinds | OBJ_BIT(ObjChannel)}});
    rpc_client rpc;
    expect(rpc.open(sender.selector(), env.pool, env.cspace, env.events));
    msg_frame message{.value = 73};
    check(rpc.exchange(message) == STATUS_PEER_CLOSED);
    expect(wait(supervisor, child));
}

int main() {
    const auto& info = rt_startup();
    supervisor.open(info.pool, info.selector(rt_domain), info.vspace, info.cpus);
    open(program, info);
    const auto pool = info.pool;
    rpc_abandon(info);
    configured_capacity(pool, info.cspace);
    abandoned_reply(pool, info.cspace, info.events);
    stale_channel(pool, info.cspace);
    stale_service(pool, info.cspace);

    for (unsigned round = 0; round != 9; ++round) {
        const auto data = sys::channel_create(pool, 1, CHANNEL_MAX_WORDS, 0, 1);
        const auto ready = sys::channel_create(pool, 4, CHANNEL_MAX_WORDS, 0, 1);
        const auto ended = sys::notification_create(pool, 1);
        expect(data.status);
        expect(ready.status);
        expect(ended.status);
        const auto sender = sys::channel_mint(data.value, info.cspace, 99, RIGHT_SEND);
        expect(sender.status);
        expect(msg_send(sender.value, {.value = 100}).status);
        expect(sys::cap_close(sender.value).status);
        const pr_cap sources[] = {input(rt_stdout, data.value, RIGHT_SEND, 99),
                                  input(rt_stderr, ready.value, RIGHT_SEND, 1)};
        test_id tasks[3];
        for (unsigned i = 0; i != 3; ++i) {
            const char number[]{static_cast<char>('0' + i), 0};
            const std::string_view arguments[]{"writer", number};
            tasks[i] = start(supervisor, program, "writer",
                             {.argv = arguments,
                              .caps = sources,
                              .resources = {.bytes = 1024 * 1024,
                                            .caps = 128,
                                            .kinds = pr_base_kinds | OBJ_BIT(ObjChannel)},
                              .events = ended.value});
        }
        unsigned entered{};
        for (unsigned i = 0; i != 3; ++i) {
            msg_frame message;
            expect(msg_recv(ready.value2, message).status);
            check(message.value < 3 && !(entered & (1U << message.value)));
            entered |= 1U << message.value;
        }
        // The data queue is still full. A single-slot implementation makes
        // later senders exit BUSY here instead of retaining all three waits.
        const auto deadline = rt_after_ms(30);
        check(static_cast<bool>(deadline));
        check(sys::notification_wait(ended.value, *deadline).status == STATUS_TIMED_OUT);
        const unsigned mode = round % 3;
        if (mode == 1)
            check(stop(supervisor, tasks[1]) == STATUS_CANCELED);
        if (mode == 2)
            expect(sys::channel_close(data.value2).status);
        else {
            msg_frame message;
            expect(msg_recv(data.value2, message).status);
            check(message.value == 100);
            unsigned received{};
            for (unsigned i = 0; i != (mode == 1 ? 2U : 3U); ++i) {
                expect(msg_recv(data.value2, message).status);
                check(message.value < 3 && !(received & (1U << message.value)));
                received |= 1U << message.value;
            }
            check(received == (mode == 1 ? 5U : 7U));
        }
        for (unsigned i = 0; i != 3; ++i) {
            if (mode == 1 && i == 1)
                continue;
            check(wait(supervisor, tasks[i]) == (mode == 2 ? STATUS_PEER_CLOSED : STATUS_OK));
        }
        destroy(data);
        destroy(ready);
        expect(sys::object_destroy(ended.value).status);
        expect(sys::cap_close(ended.value).status);
    }
    sys::exit();
}
