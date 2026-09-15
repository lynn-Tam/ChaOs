#include <user/lib/stream.hpp>
#include <user/lib/clock.hpp>
#include <libk/fmt.hpp>

namespace {
using namespace myos;
myos_cap_t output;
Clock clock;
unsigned step{};
void check(bool condition) noexcept {
    ++step;
    if (condition) return;
    stream::Writer writer{output};
    (void)libk::fmt::format_to<"[process] failed step={}\n">(writer, step);
    exit(MYOS_STATUS_INTERNAL);
}
auto deadline(uint64_t ms) noexcept -> uint64_t {
    const auto value = clock.after_ms(ms);
    check(static_cast<bool>(value));
    return *value;
}
auto request(service::Process operation, uint64_t id = 0, uint64_t expires = 0) noexcept -> service::Message {
    service::Message result{.operation = static_cast<uint64_t>(operation), .id = id};
    if (expires != 0) { result.size = sizeof(expires); service::copy(result.data, &expires, sizeof(expires)); }
    return result;
}
auto receive(service::Connection& channel, service::Process operation, myos_status_t status) noexcept -> service::Message {
    service::Message reply;
    check(channel.receive(reply).status == MYOS_STATUS_OK);
    check(reply.operation == static_cast<uint64_t>(operation) && reply.status == status);
    return reply;
}
auto exchange(service::Connection& channel, service::Message message, myos_status_t status) noexcept -> service::Message {
    check(channel.send(message).status == MYOS_STATUS_OK);
    return receive(channel, static_cast<service::Process>(message.operation), status);
}
auto spawn(service::Connection& channel, const char* ms, myos_status_t status = MYOS_STATUS_OK) noexcept -> uint64_t {
    auto message = request(service::Process::Spawn);
    bootstrap::Arguments args;
    check(args.append("sleep") && args.append(ms));
    message.size = args.encode(message.data, sizeof(message.data));
    return exchange(channel, message, status).id;
}
void stop(service::Connection& channel, uint64_t id) noexcept {
    check(exchange(channel, request(service::Process::Stop, id), MYOS_STATUS_CANCELED).id == id);
    exchange(channel, request(service::Process::Wait, id), MYOS_STATUS_INVALID_CAP);
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    output = service::capability(info, bootstrap::imports::ConsoleOutput);
    check(clock.open() == MYOS_STATUS_OK);
    service::Connection channel{service::capability(info, bootstrap::imports::Process),
        service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION)};

    const auto first = spawn(channel, "10000");
    check(channel.send(request(service::Process::Wait, first)).status == MYOS_STATUS_OK);
    const auto second = spawn(channel, "10000"); // Must progress while the first Wait is pending.
    check(first != second);
    check(channel.send(request(service::Process::CancelWait, first)).status == MYOS_STATUS_OK);
    check(receive(channel, service::Process::Wait, MYOS_STATUS_CANCELED).id == first);
    receive(channel, service::Process::CancelWait, MYOS_STATUS_OK);
    exchange(channel, request(service::Process::Wait, first, deadline(1)), MYOS_STATUS_TIMED_OUT);
    stop(channel, second);
    check(channel.send(request(service::Process::Wait, first)).status == MYOS_STATUS_OK);
    check(channel.send(request(service::Process::Stop, first)).status == MYOS_STATUS_OK);
    receive(channel, service::Process::Stop, MYOS_STATUS_OK);
    receive(channel, service::Process::Wait, MYOS_STATUS_CANCELED);
    exchange(channel, request(service::Process::Wait, first), MYOS_STATUS_INVALID_CAP);

    uint64_t retained[4]{};
    for (auto& id : retained) id = spawn(channel, "1");
    spawn(channel, "1", MYOS_STATUS_BUSY); // Even completed, unconsumed results retain admission slots.
    for (auto id : retained) exchange(channel, request(service::Process::Wait, id), MYOS_STATUS_OK);
    const auto reused = spawn(channel, "10000");
    for (auto id : retained) {
        check(id != reused);
        exchange(channel, request(service::Process::Wait, id), MYOS_STATUS_INVALID_CAP);
    }
    stop(channel, reused);

    // Exercise the actual timeout/completion arbitration repeatedly. A timeout
    // removes only the waiter; completion remains consumable exactly once.
    for (unsigned i = 0; i != 16; ++i) {
        const auto id = spawn(channel, "1");
        check(channel.send(request(service::Process::Wait, id, deadline(1))).status == MYOS_STATUS_OK);
        service::Message reply;
        check(channel.receive(reply).status == MYOS_STATUS_OK && reply.id == id
            && reply.operation == static_cast<uint64_t>(service::Process::Wait));
        check(reply.status == MYOS_STATUS_OK || reply.status == MYOS_STATUS_TIMED_OUT);
        if (reply.status == MYOS_STATUS_TIMED_OUT)
            exchange(channel, request(service::Process::Wait, id), MYOS_STATUS_OK);
        exchange(channel, request(service::Process::Wait, id), MYOS_STATUS_INVALID_CAP);
        check(channel.try_receive(reply).status == MYOS_STATUS_WOULD_BLOCK);
    }

    // Pending badges win before deadline admission; a signal after timeout is
    // preserved for the next wait instead of rewriting the prior result.
    const auto notification = notification_create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL), 8);
    check(notification.status == MYOS_STATUS_OK);
    for (unsigned i = 0; i != 16; ++i) {
        check(notification_wait(notification.value, deadline(1)).status == MYOS_STATUS_TIMED_OUT);
        check(notification_signal(notification.value).status == MYOS_STATUS_OK);
        const auto signaled = notification_wait(notification.value, 1);
        check(signaled.status == MYOS_STATUS_OK && signaled.value == 8);
        check(notification_wait(notification.value, 1).status == MYOS_STATUS_TIMED_OUT);
    }
    check(object_destroy(notification.value).status == MYOS_STATUS_OK);
    check(cap_close(notification.value).status == MYOS_STATUS_OK);
    stream::Writer{output}.write("[process] async wait, cancel, retention, deadlines and reuse ok\n");
    exit();
}
