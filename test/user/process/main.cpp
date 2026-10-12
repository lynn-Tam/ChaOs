#include <string.h>
#include <sys/protocol.hpp>
#include <sys/channel.hpp>
#include <sys/clock.hpp>
#include <libk/fmt.hpp>

namespace {
using namespace sys;
cap_t output;
Clock clock;
unsigned step{};
void check(bool condition) noexcept {
    ++step;
    if (condition) return;
    stream::Writer writer{output};
    (void)libk::fmt::format_to<"[process] failed step={}\n">(writer, step);
    exit(STATUS_INTERNAL);
}
auto deadline(uint64_t ms) noexcept -> uint64_t {
    const auto value = clock.after_ms(ms);
    check(static_cast<bool>(value));
    return *value;
}
auto request(proc::op operation, uint64_t id = 0, uint64_t expires = 0) noexcept -> sys::Message {
    sys::Message result{.operation = static_cast<uint64_t>(operation), .id = id};
    if (expires != 0) { result.size = sizeof(expires); ::memcpy(result.data, &expires, sizeof(expires)); }
    return result;
}
auto receive(sys::Port& channel, proc::op operation, status_t status) noexcept -> sys::Message {
    sys::Message reply;
    check(channel.receive(reply).status == STATUS_OK);
    if (reply.operation != static_cast<uint64_t>(operation) || reply.status != status) {
        stream::Writer writer{output};
        (void)libk::fmt::format_to<"[process] reply op={} status={} id={}, expected op={} status={}\n">(
            writer, reply.operation, reply.status, reply.id, static_cast<uint64_t>(operation), status);
    }
    check(reply.operation == static_cast<uint64_t>(operation) && reply.status == status);
    return reply;
}
auto exchange(sys::Port& channel, sys::Message message, status_t status) noexcept -> sys::Message {
    check(channel.send(message).status == STATUS_OK);
    return receive(channel, static_cast<proc::op>(message.operation), status);
}
auto spawn(sys::Port& channel, const char* ms, status_t status = STATUS_OK) noexcept -> uint64_t {
    auto message = request(proc::op::Spawn);
    boot::args args;
    check(args.append("sleep", 5) && args.append(ms, ::strlen(ms)));
    message.size = args.size();
    ::memcpy(message.data, args.data(), message.size);
    return exchange(channel, message, status).id;
}
void stop(sys::Port& channel, uint64_t id) noexcept {
    check(exchange(channel, request(proc::op::Stop, id), STATUS_CANCELED).id == id);
    exchange(channel, request(proc::op::Wait, id), STATUS_INVALID_CAP);
}
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    output = sys::capability(info, boot::ConsoleOutput);
    check(clock.open() == STATUS_OK);
    sys::Port channel{sys::capability(info, boot::Process),
        sys::capability(info, BOOT_EVENTS)};

    const auto first = spawn(channel, "10000");
    check(channel.send(request(proc::op::Wait, first)).status == STATUS_OK);
    const auto second = spawn(channel, "10000"); // Must progress while the first Wait is pending.
    check(first != second);
    check(channel.send(request(proc::op::CancelWait, first)).status == STATUS_OK);
    check(receive(channel, proc::op::Wait, STATUS_CANCELED).id == first);
    receive(channel, proc::op::CancelWait, STATUS_OK);
    exchange(channel, request(proc::op::Wait, first, deadline(1)), STATUS_TIMED_OUT);
    stop(channel, second);
    check(channel.send(request(proc::op::Wait, first)).status == STATUS_OK);
    check(channel.send(request(proc::op::Stop, first)).status == STATUS_OK);
    receive(channel, proc::op::Stop, STATUS_OK);
    receive(channel, proc::op::Wait, STATUS_CANCELED);
    exchange(channel, request(proc::op::Wait, first), STATUS_INVALID_CAP);

    uint64_t retained[4]{};
    for (auto& id : retained) id = spawn(channel, "1");
    spawn(channel, "1", STATUS_BUSY); // Even completed, unconsumed results retain admission slots.
    for (auto id : retained) exchange(channel, request(proc::op::Wait, id), STATUS_OK);
    const auto reused = spawn(channel, "10000");
    for (auto id : retained) {
        check(id != reused);
        exchange(channel, request(proc::op::Wait, id), STATUS_INVALID_CAP);
    }
    stop(channel, reused);

    // Stop while a child owns a deadline wait, then reuse its allocation.
    for (unsigned i = 0; i != 16; ++i) stop(channel, spawn(channel, "10000"));

    // Exercise the actual timeout/completion arbitration repeatedly. A timeout
    // removes only the waiter; completion remains consumable exactly once.
    for (unsigned i = 0; i != 16; ++i) {
        const auto id = spawn(channel, "1");
        check(channel.send(request(proc::op::Wait, id, deadline(1))).status == STATUS_OK);
        sys::Message reply;
        check(channel.receive(reply).status == STATUS_OK && reply.id == id
            && reply.operation == static_cast<uint64_t>(proc::op::Wait));
        check(reply.status == STATUS_OK || reply.status == STATUS_TIMED_OUT);
        if (reply.status == STATUS_TIMED_OUT)
            exchange(channel, request(proc::op::Wait, id), STATUS_OK);
        exchange(channel, request(proc::op::Wait, id), STATUS_INVALID_CAP);
        check(channel.try_receive(reply).status == STATUS_WOULD_BLOCK);
    }

    // Pending badges win before deadline admission; a signal after timeout is
    // preserved for the next wait instead of rewriting the prior result.
    const auto notification = notification_create(sys::capability(info, BOOT_POOL), 8);
    check(notification.status == STATUS_OK);
    for (unsigned i = 0; i != 16; ++i) {
        check(notification_wait(notification.value, deadline(1)).status == STATUS_TIMED_OUT);
        check(notification_signal(notification.value).status == STATUS_OK);
        const auto signaled = notification_wait(notification.value, 1);
        check(signaled.status == STATUS_OK && signaled.value == 8);
        check(notification_wait(notification.value, 1).status == STATUS_TIMED_OUT);
    }
    check(object_destroy(notification.value).status == STATUS_OK);
    check(cap_close(notification.value).status == STATUS_OK);
    stream::Writer{output}.write("[process] async wait, cancel, retention, deadlines and reuse ok\n");
    exit();
}
