#include <user/lib/clock.hpp>
#include <user/lib/supervisor.hpp>

namespace {
using namespace myos;
deploy::Program program;
using Supervisor = deploy::Supervisor<3>;
Supervisor supervisor;
unsigned step{};
void require(myos_status_t status) noexcept {
    ++step;
    // Fixture exit code retains the failed check and original status.
    if (status != MYOS_STATUS_OK) exit(-static_cast<myos_status_t>(step * 100) + status);
}
void check(bool value) noexcept { require(value ? MYOS_STATUS_OK : MYOS_STATUS_INTERNAL); }
auto binding(const char* name, myos_cap_t channel) noexcept -> deploy::LaunchSource {
    return {name, {channel, 0}, {
        .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
        .kind = MYOS_OBJECT_KIND_CHANNEL, .size = MYOS_CAP_ATTENUATION_SIZE,
        .rights = MYOS_RIGHT_SEND | MYOS_RIGHT_DUPLICATE, .words = {0, 0, 0, 0}}};
}
void destroy(SysResult pair) noexcept {
    require(object_destroy(pair.value).status);
    require(cap_close(pair.value).status);
    require(cap_close(pair.value2).status);
}
void configured_capacity(myos_cap_t pool, myos_cap_t cspace) noexcept {
    constexpr unsigned depth = 33, bindings = 8;
    const auto pair = channel_create(pool, depth, MYOS_CHANNEL_MAX_WORDS, 0, bindings);
    require(pair.status);
    const auto sender = channel_mint(pair.value, cspace, 1, MYOS_RIGHT_SEND);
    require(sender.status);
    cap::OwnedCap events[bindings];
    myos_word_t relations[bindings];
    for (unsigned i = 0; i != bindings; ++i) {
        const auto event = notification_create(pool, 1);
        require(event.status);
        events[i] = cap::OwnedCap{{event.value, 0}};
        const auto bound = channel_bind(pair.value2, event.value, MYOS_CHANNEL_READABLE);
        require(bound.status);
        relations[i] = bound.value;
    }
    for (unsigned round = 0; round != 3; ++round) {
        for (unsigned i = 0; i != depth; ++i)
            require(service::send(sender.value, {.id = i}, false).status);
        check(service::send(sender.value, {.id = depth}, false).status == MYOS_STATUS_WOULD_BLOCK);
        for (auto& event : events) require(notification_take(event.selector()).status);
        service::Message message;
        for (unsigned i = 0; i != depth; ++i) {
            require(service::receive(pair.value2, message, false).status);
            check(message.id == i);
        }
        check(service::receive(pair.value2, message, false).status == MYOS_STATUS_WOULD_BLOCK);
        for (unsigned i = 0; i != bindings; ++i) {
            const auto arm = channel_arm(pair.value2, relations[i], 0);
            require(arm.status);
            require(notification_take(events[i].selector()).status);
            require(channel_arm(pair.value2, relations[i], arm.value).status);
        }
    }
    require(cap_close(sender.value).status);
    destroy(pair);
    for (auto& event : events) require(object_destroy(event.selector()).status);
}

}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = service::bootstrap(address, size);
    supervisor.open(info);
    require(supervisor.load(program, info));
    require(supervisor.add_boot_sources(info));
    const auto pool = service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    configured_capacity(pool, service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE));
    Clock clock;
    require(clock.open());
    for (unsigned round = 0; round != 9; ++round) {
        const auto data = channel_create(pool, 1, MYOS_CHANNEL_MAX_WORDS, 0, 1);
        const auto ready = channel_create(pool, 4, MYOS_CHANNEL_MAX_WORDS, 0, 1);
        const auto terminal = notification_create(pool, 1);
        require(data.status); require(ready.status); require(terminal.status);
        const auto sender = channel_mint(data.value,
            service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), 99, MYOS_RIGHT_SEND);
        require(sender.status);
        require(service::send(sender.value, {.id = 100}).status);
        require(cap_close(sender.value).status);
        const deploy::LaunchSource sources[] = {binding("data", data.value), binding("ready", ready.value)};
        Supervisor::Handle tasks[3];
        for (unsigned i = 0; i != 3; ++i) {
            bootstrap::Arguments arguments;
            const char number = '0' + i;
            check(arguments.append("writer", 6) && arguments.append(&number, 1));
            myos_status_t status{};
            auto task = supervisor.launch(program, Supervisor::name("writer"), status,
                {.arguments = &arguments, .terminal_events = terminal.value, .sources = sources});
            require(status); check(static_cast<bool>(task));
            tasks[i] = libk::move(*task);
        }
        unsigned entered{};
        for (unsigned i = 0; i != 3; ++i) {
            service::Message message;
            require(service::receive(ready.value2, message).status);
            check(message.id < 3 && !(entered & (1U << message.id)));
            entered |= 1U << message.id;
        }
        // The data queue is still full. A single-slot implementation makes
        // later senders exit BUSY here instead of retaining all three waits.
        const auto deadline = clock.after_ms(30);
        check(static_cast<bool>(deadline));
        check(notification_wait(terminal.value, *deadline).status == MYOS_STATUS_TIMED_OUT);
        const unsigned mode = round % 3;
        if (mode == 1) check(supervisor.stop(tasks[1]) == MYOS_STATUS_CANCELED);
        if (mode == 2) require(channel_close(data.value2).status);
        else {
            service::Message message;
            require(service::receive(data.value2, message).status);
            check(message.id == 100);
            unsigned received{};
            for (unsigned i = 0; i != (mode == 1 ? 2U : 3U); ++i) {
                require(service::receive(data.value2, message).status);
                check(message.id < 3 && !(received & (1U << message.id)));
                received |= 1U << message.id;
            }
            check(received == (mode == 1 ? 5U : 7U));
        }
        for (unsigned i = 0; i != 3; ++i) {
            if (mode == 1 && i == 1) continue;
            check(supervisor.wait(tasks[i]) == (mode == 2 ? MYOS_STATUS_PEER_CLOSED : MYOS_STATUS_OK));
        }
        destroy(data); destroy(ready);
        require(object_destroy(terminal.value).status);
        require(cap_close(terminal.value).status);
    }
    exit();
}
