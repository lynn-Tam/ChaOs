#include <protocols/msg.hpp>
#include <protocols/stream.hpp>
#include <utility>
#include <source_location>
#include <test/user/channel/export_protocol.hpp>
#include <util.hpp>
#include <test/user/task.hpp>

static test_image program;
using Supervisor = test_tasks;
Supervisor supervisor;
static void expect(status_t status, std::source_location at = std::source_location::current()) noexcept {
    // The fixture's exit status identifies the failed source line and status.
    if (status != STATUS_OK)
        sys::exit(-static_cast<status_t>(at.line() * 100) + status);
}
static void check(bool value, std::source_location at = std::source_location::current()) noexcept {
    expect(value ? STATUS_OK : STATUS_INTERNAL, at);
}
static void destroy(sys::SysResult pair) noexcept {
    expect(sys::object_destroy(pair.value).status);
    expect(sys::cap_close(pair.value).status);
    expect(sys::cap_close(pair.value2).status);
}
static void configured_capacity(CreateAuth pool, cap_t cspace) noexcept {
    constexpr unsigned depth = 33, bindings = 8;
    const auto pair = sys::channel_create(pool, depth, CHANNEL_MAX_WORDS, 0);
    expect(pair.status);
    const auto sender = sys::channel_mint(pair.value, cspace, 1, RIGHT_SEND);
    expect(sender.status);
    check(sys::source_info(sender.value, SOURCE_READ).status == STATUS_DENIED);
    check(sys::source_info(sender.value, SOURCE_WRITE).status == STATUS_OK);
    cap_owner events[bindings];
    word_t relations[bindings];
    for (unsigned i = 0; i != bindings; ++i) {
        const auto event = sys::port_create(pool, 1, 1);
        expect(event.status);
        events[i] = cap_owner{event.value};
        const auto bound = sys::port_watch(event.value, pair.value2, SOURCE_READY, 0, i, WATCH_CHANGED, SOURCE_READ);
        expect(bound.status);
        relations[i] = bound.value;
    }
    for (unsigned round = 0; round != 3; ++round) {
        word_t ready_version{};
        for (unsigned i = 0; i != depth; ++i) {
            expect(msg_send(sender.value, {.value = i}, false).status);
            const auto ready = sys::source_info(pair.value2, SOURCE_READ);
            expect(ready.status);
            if (!i) ready_version = ready.value2;
            check(ready.value == SOURCE_READY && ready.value2 == ready_version);
        }
        check(msg_send(sender.value, {.value = depth}, false).status == STATUS_WOULD_BLOCK);
        for (auto& event : events)
            expect(sys::port_take(event.selector()).status);
        msg_frame message;
        for (unsigned i = 0; i != depth; ++i) {
            expect(msg_recv(pair.value2, message, false).status);
            check(message.value == i);
        }
        check(msg_recv(pair.value2, message, false).status == STATUS_WOULD_BLOCK);
        const auto seen = sys::source_info(pair.value2, SOURCE_READ);
        expect(seen.status);
        for (unsigned i = 0; i != bindings; ++i) {
            expect(sys::port_arm(events[i].selector(), relations[i], 0).status);
            expect(sys::port_take(events[i].selector()).status);
            expect(sys::port_arm(events[i].selector(), relations[i], seen.value2).status);
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

static void parallel_payloads(const rt_env& env) {
    std::array<ch_pair, 4> channels;
    std::array<test_id, 8> children;
    ch_pair report;
    expect(report.open(env.alloc(), 8, 0));
    const auto opened = sys::notification_create(env.alloc(), 1);
    expect(opened.status);
    cap_owner gate{opened.value};
    const std::string_view args[]{"writer", "flood"};
    for (size_t i = 0; i < channels.size(); ++i) {
        expect(channels[i].open(env.alloc(), 4, 0));
        for (size_t side = 0; side < 2; ++side) {
            const pr_cap caps[]{input(rt_stdout, channels[i].ends[side].selector(), RIGHT_SEND | RIGHT_RECEIVE, 1),
                input(rt_stderr, report.ends[0].selector(), RIGHT_SEND, 1),
                input(ct_notice, gate.selector(), RIGHT_RECEIVE)};
            children[i * 2 + side] = start(supervisor, program, "writer", {.argv = args, .caps = caps,
                .resources = {.caps = 128}});
        }
    }
    msg_frame result;
    for (size_t i = 0; i < children.size(); ++i) expect(msg_recv(report.ends[1].selector(), result).status);
    const auto begin = sys::clock_now().value;
    expect(sys::notification_signal(gate.selector()).status);
    word_t p50_max{}, p99_max{};
    for (size_t i = 0; i < children.size(); ++i) {
        expect(msg_recv(report.ends[1].selector(), result).status);
        check(result.size == sizeof(word_t));
        word_t p99; ::memcpy(&p99, result.data, sizeof(p99));
        p50_max = std::max(p50_max, result.value); p99_max = std::max(p99_max, p99);
    }
    const auto elapsed = sys::clock_now().value - begin;
    for (const auto child : children) expect(wait(supervisor, child));
    // Each sample is a batch of four sends and four receives, not one syscall.
    const std::array<word_t, 2> percentiles{p50_max, p99_max};
    result = {.value = elapsed, .size = sizeof(percentiles)};
    ::memcpy(result.data, percentiles.data(), sizeof(percentiles));
    expect(msg_send(env.selector(ct_perf), result).status);
    expect(sys::object_destroy(gate.selector()).status);
}

static void competing_consumers(const rt_env& env) {
    ch_pair queue, report;
    expect(queue.open(env.alloc(), 1, 0)); expect(report.open(env.alloc(), 9, 0));
    std::array<test_id, 9> children;
    const std::string_view args[]{"worker", "compete"};
    for (auto& child : children) {
        const pr_cap caps[]{input(rt_stdout, queue.ends[1].selector(), RIGHT_RECEIVE),
            input(rt_stderr, report.ends[0].selector(), RIGHT_SEND, 1)};
        child = start(supervisor, program, "writer", {.argv = args, .caps = caps, .resources = {.caps = 128}});
    }
    msg_frame response;
    for (auto child : children) { (void)child; expect(msg_recv(report.ends[1].selector(), response).status); check(response.value == UINT64_MAX); }
    const auto sender = sys::channel_mint(queue.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
    expect(sender.status); cap_owner endpoint{sender.value};
    std::array<word_t, 128> latency;
    for (size_t round = 0; round < latency.size(); ++round) {
        const auto deadline = rt_after_ms(2'000); check(bool(deadline));
        for (;;) {
            bool waiting = true;
            for (auto child : children) {
                auto info = supervisor.info(child); check(bool(info));
                waiting &= bool(info->flags & THREAD_WAITING);
                check(!(info->flags & THREAD_STOPPED));
            }
            if (waiting) break;
            check(sys::clock_now().value < *deadline); sys::yield();
        }
        const auto begin = sys::clock_now().value;
        expect(msg_send(endpoint.selector(), {.value = round}, false).status);
        expect(msg_recv(report.ends[1].selector(), response).status);
        latency[round] = sys::clock_now().value - begin;
        check(response.value == round);
        check(msg_recv(report.ends[1].selector(), response, false).status == STATUS_WOULD_BLOCK);
    }
    expect(sys::channel_close(queue.ends[0].selector()).status);
    for (auto child : children) expect(wait(supervisor, child));
    std::sort(latency.begin(), latency.end());
    msg_frame result{.operation = 1, .value = latency[latency.size() / 2], .size = sizeof(word_t)};
    const auto p99 = latency[(latency.size() * 99 + 99) / 100 - 1];
    ::memcpy(result.data, &p99, sizeof(p99));
    expect(msg_send(env.selector(ct_perf), result).status);
}

static void native_replies(const rt_env& env) {
    ch_pair request, response, handoff;
    expect(request.open(env.alloc(), 1, 2)); expect(response.open(env.alloc(), 1, 2));
    expect(handoff.open(env.alloc(), 1, 1));
    const auto minted = sys::channel_mint(request.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
    expect(minted.status); cap_owner sender{minted.value};
    auto call = [&] { return msg_send(sender.selector(), {}, false, {}, response.ends[1].selector()); };
    auto receive = [&] {
        msg_packet packet;
        expect(msg_recv(request.ends[1].selector(), packet.message, false, packet.capabilities, &packet.count).status);
        check(packet.count == 1); return std::move(packet.capabilities[0]);
    };
    check(msg_send(sender.selector(), {}, false, {}, request.ends[0].selector()).status == STATUS_BAD_ARGS);
    const CapXfer invalid{0, RIGHT_SEND, CAP_COPY, 0};
    check(msg_send(sender.selector(), {}, false, {&invalid, 1}, response.ends[1].selector()).status != STATUS_OK);
    msg_frame empty;
    check(msg_recv(response.ends[0].selector(), empty, false).status == STATUS_WOULD_BLOCK);
    auto first = call(); expect(first.status); auto old = receive();
    check(sys::cap_duplicate(old.selector(), env.cspace, RIGHT_SEND).status == STATUS_BAD_RIGHTS);
    expect(sys::channel_cancel(response.ends[0].selector(), first.value, true).status);
    auto second = call(); expect(second.status); check(second.value != first.value);
    auto current = receive();
    check(msg_send(old.selector(), {}, false).status == STATUS_CANCELED);
    expect(old.close());
    const auto route = sys::channel_mint(handoff.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
    expect(route.status); cap_owner moving{route.value};
    const CapXfer transfer{current.selector(), 0, CAP_MOVE, 0};
    expect(msg_send(moving.selector(), {}, false, {&transfer, 1}).status); (void)current.release();
    msg_packet forwarded;
    expect(msg_recv(handoff.ends[1].selector(), forwarded.message, false, forwarded.capabilities, &forwarded.count).status);
    check(forwarded.count == 1);
    const auto notice = sys::notification_create(env.alloc(), 1); expect(notice.status); cap_owner event{notice.value};
    const CapXfer business{event.selector(), RIGHT_RECEIVE, CAP_COPY, 0};
    expect(msg_send(forwarded.capabilities[0].selector(), {.value = 42}, false, {&business, 1}).status);
    check(msg_send(forwarded.capabilities[0].selector(), {.value = 99}, false).status != STATUS_OK);
    msg_frame frame;
    const auto insufficient = msg_recv(response.ends[0].selector(), frame, false);
    check(insufficient.status == STATUS_NO_MEMORY && insufficient.value == 1);
    msg_packet received;
    expect(msg_recv(response.ends[0].selector(), received.message, false, received.capabilities, &received.count).status);
    check(received.count == 1 && received.message.value == 42);
    check(msg_recv(response.ends[0].selector(), frame, false).status == STATUS_WOULD_BLOCK);
    auto third = call(); expect(third.status); auto abandoned = receive();
    expect(abandoned.close());
    check(msg_recv(response.ends[0].selector(), frame, false).status == STATUS_PEER_FAULT);
    expect(call().status); expect(request.close());
    check(msg_recv(response.ends[0].selector(), frame).status == STATUS_PEER_FAULT);
    expect(sys::object_destroy(event.selector()).status);
}

// An exported endpoint queued in an abandoned reply has no future holder.
// Dropping that escrow must notify its provider, even without explicit Close.
static void abandoned_reply(CreateAuth pool, cap_t cspace, cap_t events) noexcept {
    ch_pair provider, reply;
    expect(provider.open(pool, 1, 0));
    expect(reply.open(pool, 1, 1));
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
        expect(ev_wait(events, *deadline).status);
    }
}

static void stale_channel(CreateAuth pool, cap_t cspace) noexcept {
    const auto old_pool =
        sys::account_create(pool, 256 * 1024, 64, pr_base_kinds | OBJ_BIT(ObjChannel));
    expect(old_pool.status);
    const auto scope = sys::scope_create(pool);
    expect(scope.status);
    const auto old = sys::channel_create({old_pool.value, scope.value}, 1, CHANNEL_MAX_WORDS, 0);
    expect(old.status);
    const auto old_sender = sys::channel_mint(old.value, cspace, 1, RIGHT_SEND);
    expect(old_sender.status);
    expect(msg_send(old_sender.value, {.value = 1}).status);
    msg_frame message;
    expect(msg_recv(old.value2, message).status);
    check(message.value == 1);
    expect(sys::scope_close(scope.value).status);
    expect(sys::cap_close(scope.value).status);
    expect(sys::account_close(old_pool.value).status);
    expect(sys::wait_one(old_pool.value, STATE_DRAINED).status);
    expect(sys::object_destroy(old_pool.value).status);

    const auto current = sys::channel_create(pool, 1, CHANNEL_MAX_WORDS, 0);
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
    expect(sys::cap_close(current_sender.value).status);
    destroy(current);
}

static void stale_service(CreateAuth pool, cap_t cspace) noexcept {
    const auto handoff = sys::channel_create(pool, 4, CHANNEL_MAX_WORDS, 1);
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

// Abandon terminates the invocation, while its reply transport stays reusable.
static void rpc_pressure(const rt_env& env) {
    ch_pair requests;
    expect(requests.open(env.alloc(), 1, 1));
    const auto minted = sys::channel_mint(requests.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
    expect(minted.status); cap_owner sender{minted.value};
    const pr_cap caps[]{input(rt_stdout, requests.ends[1].selector(), RIGHT_RECEIVE)};
    const std::string_view args[]{"provider", "echo"};
    auto child = start(supervisor, program, "provider", {.argv = args, .caps = caps, .resources = {.caps = 128}});
    rpc_client client;
    expect(client.open(sender.selector(), env.alloc()));
    std::array<word_t, 128> latency;
    for (size_t i = 0; i < latency.size(); ++i) {
        msg_frame request{.value = i};
        const auto began = sys::clock_now().value;
        expect(client.exchange(request));
        latency[i] = sys::clock_now().value - began;
        check(request.value == i);
    }
    expect(wait(supervisor, child));
    std::sort(latency.begin(), latency.end());
    msg_frame metrics{.operation = 2, .value = latency[latency.size() / 2], .size = sizeof(word_t)};
    const auto p99 = latency[(latency.size() * 99 + 99) / 100 - 1];
    ::memcpy(metrics.data, &p99, sizeof(p99));
    expect(msg_send(env.selector(ct_perf), metrics).status);
}

static void rpc_abandon(const rt_env& env) {
    ch_pair request;
    expect(request.open(env.alloc(), 1, 1));
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
    expect(rpc.open(sender.selector(), env.alloc()));
    msg_frame expired{.value = 74, .deadline = sys::clock_now().value};
    check(rpc.exchange(expired) == STATUS_TIMED_OUT);
    msg_frame message{.value = 73};
    check(rpc.exchange(message) == STATUS_PEER_FAULT);
    expect(wait(supervisor, child));
}

static void rebill_memory(const rt_env& env) {
    const auto first = sys::account_create(env.alloc(), 128 * 1024, 32, OBJ_BIT(ObjMem) | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjScope));
    const auto second = sys::account_create(env.alloc(), 128 * 1024, 32, OBJ_BIT(ObjMem) | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjScope));
    expect(first.status); expect(second.status);
    cap_owner a{first.value}, b{second.value};
    const auto from = sys::scope_create(env.alloc()), to = sys::scope_create(env.alloc());
    expect(from.status); expect(to.status);
    cap_owner source{from.value}, destination{to.value};
    auto members = [&](cap_t scope) {
        expect(sys::scope_members(scope).status);
        ScopeMembers page; ::memcpy(&page, reinterpret_cast<const void*>(env.ipc), sizeof(page));
        return page;
    };
    const auto memory = sys::memory_create({a.selector(), source.selector()}, 8192, VM_READ | VM_WRITE);
    expect(memory.status);
    auto area = vm_area::map(env.vspace, cap_owner{memory.value}, 0, 8192, VM_READ | VM_WRITE);
    check(bool(area));
    auto* bytes = reinterpret_cast<volatile uint8_t*>(area->address);
    bytes[0] = 0x5a;
    auto balance = [&](cap_t account) {
        expect(sys::account_info(account).status);
        AccountInfo info; ::memcpy(&info, reinterpret_cast<const void*>(env.ipc), sizeof(info));
        return info.memory_available;
    };
    auto identity = [&](cap_t object) {
        expect(sys::object_info(object).status);
        ObjectInfo info; ::memcpy(&info, reinterpret_cast<const void*>(env.ipc), sizeof(info));
        return info;
    };
    const auto original = identity(area->memory.selector());
    check(original.billing_epoch == 1 && members(source.selector()).count == 1
        && members(destination.selector()).count == 0);
    const auto before_a = balance(a.selector()), before_b = balance(b.selector());
    const auto empty = sys::account_create(env.alloc(), 0, 0, OBJ_BIT(ObjMem));
    expect(empty.status);
    cap_owner denied{empty.value};
    check(sys::object_rebill(area->memory.selector(), a.selector(), denied.selector(), 1,
        source.selector(), destination.selector(), 1).status == STATUS_DENIED);
    check(identity(area->memory.selector()).billing_epoch == 1 && members(source.selector()).count == 1
        && members(destination.selector()).count == 0 && balance(a.selector()) == before_a);
    expect(denied.close());
    const auto moved = sys::object_rebill(area->memory.selector(), a.selector(), b.selector(), 1,
        source.selector(), destination.selector(), 1);
    expect(moved.status); check(moved.value == 2 && moved.value2 == 2 && bytes[0] == 0x5a);
    const auto membership = members(destination.selector());
    check(members(source.selector()).count == 0 && membership.count == 1
        && membership.records[0].object_id == original.object_id && membership.records[0].epoch == moved.value2);
    expect(sys::scope_close(source.selector()).status);
    expect(source.close());
    const auto current = identity(area->memory.selector());
    check(current.object_id == original.object_id && current.billing_epoch == moved.value);
    const auto after_a = balance(a.selector()), after_b = balance(b.selector());
    check(after_a > before_a && before_b - after_b == after_a - before_a);
    check(sys::object_rebill(area->memory.selector(), a.selector(), b.selector(), 1).status == STATUS_STALE);
    bytes[4096] = 0xa5;
    check(bytes[0] == 0x5a && balance(a.selector()) == after_a && balance(b.selector()) < after_b);
    const auto created = sys::cspace_create({a.selector(), env.scope}, 8, 3);
    expect(created.status);
    cap_owner space{created.value};
    const auto notice = sys::notification_create(env.alloc(), 1);
    expect(notice.status);
    cap_owner signal{notice.value};
    const auto copied = sys::cap_duplicate(signal.selector(), space.selector(), RIGHT_SIGNAL);
    expect(copied.status);
    const auto old_balance = balance(a.selector()), new_balance = balance(b.selector());
    expect(sys::object_rebill(space.selector(), a.selector(), b.selector(), 1).status);
    check(balance(a.selector()) > old_balance && balance(b.selector()) < new_balance);
    expect(sys::cap_close(copied.value, space.selector()).status);
    auto cap_balance = [&](cap_t account) {
        expect(sys::account_info(account).status);
        AccountInfo info; ::memcpy(&info, reinterpret_cast<const void*>(env.ipc), sizeof(info));
        return info.caps_available;
    };
    const auto caps_a = cap_balance(a.selector()), caps_b = cap_balance(b.selector());
    const auto again = sys::cap_duplicate(signal.selector(), space.selector(), RIGHT_SIGNAL);
    expect(again.status);
    check(cap_balance(a.selector()) == caps_a && cap_balance(b.selector()) + 1 == caps_b);

    const auto scope = sys::scope_create({a.selector(), env.scope});
    expect(scope.status);
    cap_owner owner{scope.value};
    const auto child = sys::account_create({env.pool, owner.selector()}, 128 * 1024, 32, OBJ_BIT(ObjScope));
    expect(child.status);
    cap_owner cycle{child.value};
    const auto parent_balance = balance(a.selector()), child_balance = balance(cycle.selector());
    check(sys::object_rebill(owner.selector(), a.selector(), cycle.selector(), 1).status == STATUS_DENIED);
    check(balance(a.selector()) == parent_balance && balance(cycle.selector()) == child_balance);
    expect(sys::scope_close(owner.selector()).status);
    expect(owner.close()); expect(cycle.close());
    expect(sys::object_destroy(space.selector()).status);
    expect(space.close());
    expect(area->close());

    // Grant nodes remain independently charged to A until the memory's old
    // capability lineage drains; Rebill transfers only its allocation owner.
    for (auto* account : {&a, &b}) {
        expect(sys::account_close(account->selector()).status);
        expect(sys::wait_one(account->selector(), STATE_DRAINED).status);
        expect(sys::object_destroy(account->selector()).status);
        expect(account->close());
    }
}

static void wait_authority(const rt_env& env) {
    const auto notice = sys::notification_create(env.alloc(), 1);
    expect(notice.status);
    cap_owner source{notice.value};
    const auto delegated = sys::cap_delegate(source.selector(), 0, RIGHT_RECEIVE | RIGHT_DUPLICATE | RIGHT_REVOKE);
    expect(delegated.status);
    cap_owner branch{delegated.value};
    ch_pair ready;
    expect(ready.open(env.alloc(), 1, 0));
    const pr_cap caps[]{input(ct_notice, branch.selector(), RIGHT_RECEIVE),
                        input(rt_stderr, ready.ends[0].selector(), RIGHT_SEND, 1)};
    const std::string_view args[]{"writer", "await"};
    const auto child = start(supervisor, program, "writer", {.argv = args, .caps = caps});
    msg_frame message;
    expect(msg_recv(ready.ends[1].selector(), message).status);
    const auto deadline = rt_after_ms(1000).value();
    for (;;) {
        const auto state = supervisor.info(child);
        check(bool(state));
        if (state->flags & THREAD_WAITING) break;
        expect(rt_admit(deadline));
        sys::yield();
    }
    expect(sys::cap_revoke(branch.selector()).status);
    check(wait(supervisor, child) == STATUS_CLOSED);
    expect(sys::notification_signal(source.selector()).status);
    expect(sys::notification_take(source.selector()).status);
    expect(sys::object_destroy(source.selector()).status);
}

static void port_events(const rt_env& env) {
    const auto created = sys::port_create(env.alloc(), 3, 1);
    expect(created.status);
    cap_owner port{created.value};
    std::array<cap_owner, 2> notices;
    for (auto& notice : notices) {
        const auto result = sys::notification_create(env.alloc(), 1);
        expect(result.status); notice = cap_owner{result.value};
    }
    const auto delegated = sys::cap_delegate(notices[1].selector(), 0, RIGHT_RECEIVE | RIGHT_REVOKE);
    expect(delegated.status);
    cap_owner branch{delegated.value};
    const auto first = sys::port_watch(port.selector(), notices[0].selector(), ~word_t{}, 0, 11);
    const auto second = sys::port_watch(port.selector(), branch.selector(), ~word_t{}, 0, 22);
    expect(first.status); expect(second.status);
    for (auto& notice : notices) expect(sys::notification_signal(notice.selector()).status);
    // Independent Sources publish finite asynchronous hints; wait for both
    // to reach the one-cell Port before testing the actual overflow facts.
    expect(sys::wait_one(port.selector(), PORT_OVERFLOW, 0, rt_after_ms(1000).value()).status);
    const auto ready = sys::source_info(port.selector());
    expect(ready.status); check(ready.value == (PORT_READABLE | PORT_OVERFLOW));
    expect(sys::notification_signal(notices[0].selector()).status);
    expect(sys::port_arm(port.selector(), first.value).status);
    check(sys::source_info(port.selector()).value2 == ready.value2);
    expect(sys::port_records(port.selector()).status);
    auto records = *reinterpret_cast<const PortRecords*>(env.ipc);
    check(records.count == 2 && !records.more && records.events[0].source_id != records.events[1].source_id);
    check((records.events[0].flags & PORT_EVENT_PENDING) && (records.events[1].flags & PORT_EVENT_PENDING));
    expect(sys::port_take(port.selector()).status);
    check(reinterpret_cast<const PortEvent*>(env.ipc)->key == 11 || reinterpret_cast<const PortEvent*>(env.ipc)->key == 22);
    check(sys::port_resync(port.selector(), records.epoch).status == STATUS_STALE);
    expect(sys::port_records(port.selector()).status);
    records = *reinterpret_cast<const PortRecords*>(env.ipc);
    expect(sys::port_resync(port.selector(), records.epoch).status);
    check(sys::source_info(port.selector()).value == 0);
    for (auto& notice : notices) expect(sys::notification_take(notice.selector()).status);
    expect(sys::port_arm(port.selector(), first.value, sys::source_info(notices[0].selector()).value2).status);
    expect(sys::notification_signal(notices[0].selector()).status);
    expect(sys::object_destroy(notices[0].selector()).status);
    const auto deadline = rt_after_ms(1000);
    check(bool(deadline));
    for (;;) {
        expect(sys::wait_one(port.selector(), PORT_READABLE | PORT_OVERFLOW, 0, *deadline).status);
        expect(sys::port_take(port.selector()).status);
        const auto event = *reinterpret_cast<const PortEvent*>(env.ipc);
        check(event.key == 11);
        if (event.flags & PORT_EVENT_CLOSED) break;
    }
    check(sys::port_arm(port.selector(), first.value).status == STATUS_CLOSED);
    expect(sys::cap_revoke(branch.selector()).status);
    expect(sys::wait_one(port.selector(), PORT_READABLE, 0, *deadline).status);
    expect(sys::port_take(port.selector()).status);
    check(reinterpret_cast<const PortEvent*>(env.ipc)->key == 22 &&
        (reinterpret_cast<const PortEvent*>(env.ipc)->flags & PORT_EVENT_CLOSED));
    expect(sys::notification_signal(notices[1].selector()).status); // Original Grant and object remain live.
    expect(sys::port_unwatch(port.selector(), first.value).status);
    expect(sys::port_unwatch(port.selector(), second.value).status);
    expect(sys::object_destroy(port.selector()).status);
    expect(sys::object_destroy(notices[1].selector()).status);
}

int main() {
    const auto& info = rt_startup();
    supervisor.open(info.alloc(), info.selector(rt_domain), info.cpus);
    open(program, info);
    const auto pool = info.alloc();
    port_events(info);
    wait_authority(info);
    rebill_memory(info);
    rpc_abandon(info);
    configured_capacity(pool, info.cspace);
    native_replies(info);
    parallel_payloads(info);
    competing_consumers(info);
    rpc_pressure(info);
    abandoned_reply(pool, info.cspace, info.events);
    stale_channel(pool, info.cspace);
    stale_service(pool, info.cspace);

    for (unsigned round = 0; round != 9; ++round) {
        const auto data = sys::channel_create(pool, 1, CHANNEL_MAX_WORDS, 0);
        const auto ready = sys::channel_create(pool, 4, CHANNEL_MAX_WORDS, 0);
        const auto ended = sys::port_create(pool, 16, 8);
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
        for (;;) {
            // Execution-watch notifications are state hints, including Start.
            // Exit itself remains authoritative for premature sender failure.
            for (auto task : tasks) {
                const auto observed = supervisor.observe(task);
                expect(observed.status); check(!observed.value);
            }
            const auto blocked = ev_wait(ended.value, *deadline);
            if (blocked.status == STATUS_TIMED_OUT) break;
            expect(blocked.status);
        }
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
