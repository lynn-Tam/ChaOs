#include "task.hpp"
#include <util.hpp>
#include "../../apps/uart/uart.hpp"
#include "../../apps/devices/fw.hpp"
#include <libk/fmt.hpp>
#include <source_location>
#include <libk/parse.hpp>

static uart_port* output;
static void expect(status_t status, std::source_location at = std::source_location::current()) {
    if (status == STATUS_OK) return;
    if (output) (void)libk::fmt::format_to<"[channel] failed {} status={}\n">(*output, at.line(), status);
    sys::exit(status);
}
static void check(bool value, std::source_location at = std::source_location::current()) {
    if (!value) expect(STATUS_INTERNAL, at);
}
static auto send(cap_t cap, word_t value, bool block = true, std::span<const CapXfer> caps = {}, cap_t reply = 0) -> sys::SysResult {
    return ch_send(cap, std::span{&value, size_t{1}}, block, caps, 0, reply);
}
static void capacity(const rt_env& env) {
    for (const auto depth : {1U, 4U, 33U}) {
        ch_pair channel;
        expect(channel.open(env.alloc(), depth, 0));
        const auto minted = sys::channel_mint(channel.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
        expect(minted.status); cap_owner sender{minted.value};
        check(sys::source_info(sender.selector(), SOURCE_READ).status == STATUS_DENIED);
        for (unsigned round = 0; round < 3; ++round) {
            for (unsigned i = 0; i < depth; ++i) expect(send(sender.selector(), i, false).status);
            check(send(sender.selector(), depth, false).status == STATUS_WOULD_BLOCK);
            for (unsigned i = 0; i < depth; ++i) {
                ch_packet packet; expect(ch_recv(channel.ends[1].selector(), packet, false).status);
                check(packet.word_count == 1 && packet.words[0] == i && packet.badge == 1 && !packet.called);
            }
            ch_packet packet;
            check(ch_recv(channel.ends[1].selector(), packet, false).status == STATUS_WOULD_BLOCK);
        }
    }
}
static void initialization(const rt_env& env) {
    const auto made = sys::port_create(env.alloc(), 257, 1);
    check(made.status == STATUS_PENDING);
    cap_owner port{made.value};
    expect(op_wait(port.selector()).status);
    check(sys::source_info(port.selector()).value == 0);
    expect(sys::object_destroy(port.selector()).status);
    const auto abandoned = sys::port_create(env.alloc(), 257, 1);
    check(abandoned.status == STATUS_PENDING); cap_owner partial_port{abandoned.value};
    expect(sys::object_destroy(partial_port.selector()).status);
    const auto server = sys::pager_create(env.alloc());
    expect(server.status); cap_owner pager{server.value};
    const auto memory = sys::memory_create_pager(env.alloc(), 1024 * 4096, VM_READ | VM_WRITE, pager.selector(), 0, 1024);
    check(memory.status == STATUS_PENDING);
    cap_owner backing{memory.value};
    expect(op_wait(backing.selector()).status);
    expect(sys::object_destroy(backing.selector()).status);
    const auto canceled = sys::memory_create_pager(env.alloc(), 1024 * 4096, VM_READ | VM_WRITE, pager.selector(), 0, 1024);
    check(canceled.status == STATUS_PENDING); cap_owner partial_mem{canceled.value};
    expect(sys::object_destroy(partial_mem.selector()).status);
    expect(sys::object_destroy(pager.selector()).status);
    // Destroying unpublished initialization cancels its remaining domain work.
    const auto partial = sys::channel_create(env.alloc(), 129, CHANNEL_MAX_WORDS, 0);
    check(partial.status == STATUS_PENDING);
    cap_owner a{partial.value}, b{partial.value2};
    expect(sys::object_destroy(a.selector()).status);
}
static void control_paths(const rt_env& env) {
    std::array<cap_owner, 96> scopes;
    cap_t parent = env.scope;
    for (auto& scope : scopes) {
        const auto created = sys::scope_create({env.quota, parent});
        expect(created.status); scope = cap_owner{created.value}; parent = scope.selector();
    }
    const auto created = sys::scope_create(env.alloc());
    expect(created.status); cap_owner moved{created.value};
    const auto adopted = sys::scope_adopt(moved.selector(), env.scope, parent, 1);
    expect(adopted.status);
    check(sys::scope_adopt(scopes[0].selector(), env.scope, moved.selector(), 1).status == STATUS_DENIED);
    const auto loan = sys::quota_create({env.quota, parent}, 65536, 64, OBJ_KINDS);
    expect(loan.status); cap_owner quota{loan.value};
    expect(sys::object_reassign(moved.selector(), env.quota, quota.selector(), 1).status);
    expect(sys::scope_close(scopes[0].selector()).status);
    expect(sys::wait_one(scopes[0].selector(), STATE_DRAINED).status);
}
static void replies(const rt_env& env) {
    ch_pair request, response, handoff;
    expect(request.open(env.alloc(), 1, 2)); expect(response.open(env.alloc(), 1, 2));
    expect(handoff.open(env.alloc(), 1, 1));
    const auto minted = sys::channel_mint(request.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
    expect(minted.status); cap_owner sender{minted.value};
    const auto receive = [&] {
        ch_packet packet; expect(ch_recv(request.ends[1].selector(), packet, false).status);
        check(packet.called && packet.count == 1); return std::move(packet.capabilities[0]);
    };
    check(send(sender.selector(), 0, false, {}, request.ends[0].selector()).status == STATUS_BAD_ARGS);
    auto first = send(sender.selector(), 1, false, {}, response.ends[1].selector());
    expect(first.status); auto old = receive();
    check(sys::cap_duplicate(old.selector(), env.cspace, RIGHT_SEND).status == STATUS_BAD_RIGHTS);
    expect(sys::channel_cancel(response.ends[0].selector(), first.value, true).status);
    auto second = send(sender.selector(), 2, false, {}, response.ends[1].selector());
    expect(second.status); check(first.value != second.value); auto current = receive();
    check(send(old.selector(), 3, false).status == STATUS_CANCELED);
    const auto route = sys::channel_mint(handoff.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
    expect(route.status); cap_owner forward{route.value};
    const CapXfer move{current.selector(), 0, CAP_MOVE, 0};
    expect(send(forward.selector(), 0, false, {&move, 1}).status); (void)current.release();
    ch_packet forwarded; expect(ch_recv(handoff.ends[1].selector(), forwarded, false).status);
    check(!forwarded.called && forwarded.count == 1);
    const auto notification = sys::notification_create(env.alloc(), 1);
    expect(notification.status); cap_owner event{notification.value};
    const CapXfer business{event.selector(), RIGHT_RECEIVE, CAP_COPY, 0};
    expect(send(forwarded.capabilities[0].selector(), 42, false, {&business, 1}).status);
    check(send(forwarded.capabilities[0].selector(), 99, false).status != STATUS_OK);
    ch_packet packet;
    const auto small = ch_recv(response.ends[0].selector(), packet, false, 0);
    check(small.status == STATUS_NO_MEMORY && small.value == 1);
    expect(ch_recv(response.ends[0].selector(), packet, false).status);
    check(packet.words[0] == 42 && packet.count == 1 && !packet.called);
    expect(send(sender.selector(), 3, false, {}, response.ends[1].selector()).status);
    auto abandoned = receive(); expect(abandoned.close());
    check(ch_recv(response.ends[0].selector(), packet, false).status == STATUS_PEER_FAULT);
    expect(sys::object_destroy(event.selector()).status);
}
static void reassign_memory(const rt_env& env) {
    const auto first = sys::quota_create(env.alloc(), 128 * 1024, 32, OBJ_BIT(ObjMem) | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjScope));
    const auto second = sys::quota_create(env.alloc(), 128 * 1024, 32, OBJ_BIT(ObjMem) | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjScope));
    expect(first.status); expect(second.status);
    cap_owner a{first.value}, b{second.value};
    const auto from = sys::scope_create(env.alloc()), to = sys::scope_create(env.alloc());
    expect(from.status); expect(to.status);
    cap_owner source{from.value}, destination{to.value};
    auto members = [&](cap_t scope) {
        expect(sys::scope_members(scope).status);
        ScopeMembers page; ::memcpy(&page, reinterpret_cast<const void*>(rt_current().ipc), sizeof(page));
        return page;
    };
    const auto memory = sys::memory_create({a.selector(), source.selector()}, 8192, VM_READ | VM_WRITE);
    expect(memory.status);
    auto area = vm_area::map(rt_vm(), cap_owner{memory.value}, 0, 8192, VM_READ | VM_WRITE);
    check(bool(area));
    auto* bytes = reinterpret_cast<volatile uint8_t*>(area->address);
    bytes[0] = 0x5a;
    check(sys::memory_read(area->memory.selector(), 4090, 0, 16).status == STATUS_BAD_ARGS);
    check(sys::memory_write(area->memory.selector(), 4090, 0, 16).status == STATUS_BAD_ARGS);
    auto balance = [&](cap_t quota) {
        expect(sys::quota_info(quota).status);
        QuotaInfo info; ::memcpy(&info, reinterpret_cast<const void*>(rt_current().ipc), sizeof(info));
        return info.memory_available;
    };
    auto identity = [&](cap_t object) {
        expect(sys::object_info(object).status);
        ObjectInfo info; ::memcpy(&info, reinterpret_cast<const void*>(rt_current().ipc), sizeof(info));
        return info;
    };
    const auto original = identity(area->memory.selector());
    check(original.quota_epoch == 1 && members(source.selector()).count == 1
        && members(destination.selector()).count == 0);
    const auto before_a = balance(a.selector()), before_b = balance(b.selector());
    const auto empty = sys::quota_create(env.alloc(), 0, 0, OBJ_BIT(ObjMem));
    expect(empty.status);
    cap_owner denied{empty.value};
    check(sys::object_reassign(area->memory.selector(), a.selector(), denied.selector(), 1,
        source.selector(), destination.selector(), 1).status == STATUS_DENIED);
    check(identity(area->memory.selector()).quota_epoch == 1 && members(source.selector()).count == 1
        && members(destination.selector()).count == 0 && balance(a.selector()) == before_a);
    expect(denied.close());
    const auto moved = sys::object_reassign(area->memory.selector(), a.selector(), b.selector(), 1,
        source.selector(), destination.selector(), 1);
    expect(moved.status); check(moved.value == 2 && moved.value2 == 2 && bytes[0] == 0x5a);
    const auto membership = members(destination.selector());
    check(members(source.selector()).count == 0 && membership.count == 1
        && membership.records[0].object_id == original.object_id && membership.records[0].epoch == moved.value2);
    expect(sys::scope_close(source.selector()).status);
    expect(source.close());
    const auto current = identity(area->memory.selector());
    check(current.object_id == original.object_id && current.quota_epoch == moved.value);
    const auto after_a = balance(a.selector()), after_b = balance(b.selector());
    check(after_a > before_a && before_b - after_b == after_a - before_a);
    check(sys::object_reassign(area->memory.selector(), a.selector(), b.selector(), 1).status == STATUS_STALE);
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
    expect(sys::object_reassign(space.selector(), a.selector(), b.selector(), 1).status);
    check(balance(a.selector()) > old_balance && balance(b.selector()) < new_balance);
    expect(sys::cap_close(copied.value, space.selector()).status);
    auto cap_balance = [&](cap_t quota) {
        expect(sys::quota_info(quota).status);
        QuotaInfo info; ::memcpy(&info, reinterpret_cast<const void*>(rt_current().ipc), sizeof(info));
        return info.caps_available;
    };
    const auto caps_a = cap_balance(a.selector()), caps_b = cap_balance(b.selector());
    const auto again = sys::cap_duplicate(signal.selector(), space.selector(), RIGHT_SIGNAL);
    expect(again.status);
    check(cap_balance(a.selector()) == caps_a && cap_balance(b.selector()) + 1 == caps_b);

    const auto scope = sys::scope_create({a.selector(), env.scope});
    expect(scope.status);
    cap_owner owner{scope.value};
    const auto child = sys::quota_create({env.quota, owner.selector()}, 128 * 1024, 32, OBJ_BIT(ObjScope));
    expect(child.status);
    cap_owner cycle{child.value};
    const auto parent_balance = balance(a.selector()), child_balance = balance(cycle.selector());
    check(sys::object_reassign(owner.selector(), a.selector(), cycle.selector(), 1).status == STATUS_DENIED);
    check(balance(a.selector()) == parent_balance && balance(cycle.selector()) == child_balance);
    expect(sys::scope_close(owner.selector()).status);
    expect(owner.close()); expect(cycle.close());
    expect(sys::object_destroy(space.selector()).status);
    expect(space.close());
    expect(area->close());

    // Grant nodes remain independently charged to A until the memory's old
    // capability lineage drains; Reassign transfers only its allocation owner.
    for (auto* quota : {&a, &b}) {
        expect(sys::quota_close(quota->selector()).status);
        expect(sys::wait_one(quota->selector(), STATE_DRAINED).status);
        expect(sys::object_destroy(quota->selector()).status);
        expect(quota->close());
    }
}

static void resize_quota(const rt_env& env) {
    const auto made = sys::quota_create(env.alloc(), 0, 0, OBJ_BIT(ObjMem), 256 * 1024, 64);
    expect(made.status); cap_owner quota{made.value};
    auto info = [&] {
        expect(sys::quota_info(quota.selector()).status);
        return *reinterpret_cast<const QuotaInfo*>(rt_current().ipc);
    };
    expect(sys::object_info(quota.selector()).status);
    const auto id = reinterpret_cast<const ObjectInfo*>(rt_current().ipc)->object_id;
    check(sys::quota_resize(quota.selector(), 128 * 1024, 32).status == STATUS_DENIED);
    expect(sys::quota_resize(quota.selector(), 128 * 1024, 32, env.quota).status);
    const auto mem = sys::memory_create({quota.selector(), env.scope}, 8192, VM_READ | VM_WRITE);
    expect(mem.status);
    auto area = vm_area::map(rt_vm(), cap_owner{mem.value}, 0, 8192, VM_READ | VM_WRITE);
    check(bool(area));
    auto* bytes = reinterpret_cast<volatile uint8_t*>(area->address);
    bytes[0] = 0x5a;
    const auto before = info();
    check(sys::quota_resize(quota.selector(), 0, 0).status == STATUS_BUSY);
    check(sys::quota_resize(quota.selector(), 512 * 1024, 32, env.quota).status == STATUS_DENIED);
    expect(sys::quota_resize(quota.selector(), 192 * 1024, 24, env.quota).status);
    const auto after = info();
    check(after.memory_available == before.memory_available + 64 * 1024
        && after.caps_available + 8 == before.caps_available && bytes[0] == 0x5a);
    bytes[4096] = 0xa5; // A new page still uses the same Quota after growth.
    expect(sys::object_info(quota.selector()).status);
    check(reinterpret_cast<const ObjectInfo*>(rt_current().ipc)->object_id == id);
    const auto current = info();
    const auto used = current.memory_limit - current.memory_available;
    expect(sys::quota_close(quota.selector()).status);
    expect(sys::quota_resize(quota.selector(), used, current.cap_limit - current.caps_available).status);
    check(info().state == OBJECT_CLOSING && bytes[0] == 0x5a && bytes[4096] == 0xa5);
    check(sys::quota_resize(quota.selector(), 192 * 1024, 32, env.quota).status == STATUS_CLOSED);
    expect(area->close());
    expect(sys::wait_one(quota.selector(), STATE_DRAINED).status);
    check(sys::quota_resize(quota.selector(), 0, 0).status == STATUS_CLOSED);
    expect(sys::object_destroy(quota.selector()).status);
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
    auto records = *reinterpret_cast<const PortRecords*>(rt_current().ipc);
    check(records.count == 2 && !records.more && records.events[0].source_id != records.events[1].source_id);
    check((records.events[0].flags & PORT_EVENT_PENDING) && (records.events[1].flags & PORT_EVENT_PENDING));
    expect(sys::port_take(port.selector()).status);
    check(reinterpret_cast<const PortEvent*>(rt_current().ipc)->key == 11 || reinterpret_cast<const PortEvent*>(rt_current().ipc)->key == 22);
    check(sys::port_resync(port.selector(), records.epoch).status == STATUS_STALE);
    expect(sys::port_records(port.selector()).status);
    records = *reinterpret_cast<const PortRecords*>(rt_current().ipc);
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
        const auto event = *reinterpret_cast<const PortEvent*>(rt_current().ipc);
        check(event.key == 11);
        if (event.flags & PORT_EVENT_CLOSED) break;
    }
    check(sys::port_arm(port.selector(), first.value).status == STATUS_CLOSED);
    expect(sys::cap_revoke(branch.selector()).status);
    expect(sys::wait_one(port.selector(), PORT_READABLE, 0, *deadline).status);
    expect(sys::port_take(port.selector()).status);
    check(reinterpret_cast<const PortEvent*>(rt_current().ipc)->key == 22 &&
        (reinterpret_cast<const PortEvent*>(rt_current().ipc)->flags & PORT_EVENT_CLOSED));
    expect(sys::notification_signal(notices[1].selector()).status); // Original Grant and object remain live.
    expect(sys::port_unwatch(port.selector(), first.value).status);
    expect(sys::port_unwatch(port.selector(), second.value).status);
    expect(sys::object_destroy(port.selector()).status);
    expect(sys::object_destroy(notices[1].selector()).status);
}

int main(int argc, char** argv) {
    const auto& env = rt_startup();
    if (argc > 1) {
        ubus_conn environment; expect(environment.bind(env.environment));
        ubus_msg setup; expect(environment.invoke("Receive", {}, setup));
        const auto destination = setup.capability(setup.data["message"]["data"]);
        const auto number = libk::parse<uint64_t>(argv[1]); check(number.has_value());
        for (word_t i = 0; i < 512; ++i) {
            std::array<word_t, CHANNEL_MAX_WORDS> words;
            for (size_t n = 0; n < words.size(); ++n) words[n] = (*number << 48) | (i << 16) | n;
            expect(ch_send(destination, words, true).status);
        }
        return STATUS_OK;
    }
    auto firmware = fw_view::open(env); check(bool(firmware));
    auto irq = firmware->irq(env); check(bool(irq));
    auto console = fw_console::open(env, firmware->tree, irq->selector()); check(bool(console));
    const auto copied = sys::cap_duplicate(console->memory.selector(), env.cspace, RIGHT_MAP); expect(copied.status);
    auto mapping = vm_area::map(rt_vm(), cap_owner{copied.value}, 0, 4096, VM_READ | VM_WRITE); check(bool(mapping));
    uart_port port{mapping->address + console->offset}; port.reset(); output = &port;
    capacity(env); initialization(env); control_paths(env); replies(env); reassign_memory(env); resize_quota(env); port_events(env);
    test_image image; open(image, env);
    test_tasks tasks; tasks.open(env.alloc(), rt_root().domain, env.cpus);
    ch_pair data; expect(data.open(env.alloc(), 32, 0));
    const auto sender = sys::channel_mint(data.ends[0].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
    expect(sender.status); cap_owner destination{sender.value};
    ubus_object environment{"fixture"};
    expect(environment.add("Receive", {}, {{"message", ubus_type::Record}}, [&](auto&, auto& response) {
        ubus_data message;
        const auto data_idx = response.attach(destination.selector(), RIGHT_SEND);
        if (!data_idx) return data_idx.error();
        message.cap("data", *data_idx);
        response.data.put("message", message); return STATUS_OK;
    }));
    expect(environment.open()); auto connector = environment.connector(); check(bool(connector));
    std::array<test_id, 4> children;
    for (size_t i = 0; i < children.size(); ++i) {
        const auto number = std::to_string(i); const std::array<std::string_view, 2> args{"producer", number};
        const auto child = tasks.spawn(image, "worker", {.argv=args, .environment=connector->selector(), .events=rt_events()});
        expect(child.status); children[i] = child.id;
    }
    ch_rx input; expect(input.open(data.ends[1].selector(), rt_events()));
    std::array<size_t, 4> seen{};
    for (size_t count = 0; count < children.size() * 512; ++count) {
        ch_packet packet;
        for (;;) {
            const auto status = input.receive(packet);
            if (status == STATUS_OK) break;
            check(status == STATUS_WOULD_BLOCK);
            const auto received = environment.poll();
            if (received != STATUS_WOULD_BLOCK) expect(received);
            else expect(ev_wait(rt_events()).status);
        }
        const auto producer = packet.words[0] >> 48, index = (packet.words[0] >> 16) & 0xffff'ffff;
        check(producer < seen.size() && index == seen[producer]++ && packet.word_count == CHANNEL_MAX_WORDS);
        for (size_t n = 0; n < packet.word_count; ++n) check(packet.words[n] == ((producer << 48) | (index << 16) | n));
    }
    for (const auto child : children) for (;;) {
        const auto result = tasks.collect(child);
        if (result.status == STATUS_OK) { expect(static_cast<status_t>(result.value)); break; }
        check(result.status == STATUS_WOULD_BLOCK || proc_retryable(result.status)); sys::yield();
    }
    port.write("[channel] native capacity, MCMP payloads, Reply cancellation, Port facts and reassigning ok\n");
    for (;;) sys::yield();
}
