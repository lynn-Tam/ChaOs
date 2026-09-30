#include <user/abi/time.hpp>
#include <servers/deploy/launch.hpp>

namespace {
using namespace myos;
deploy::program program;
using Supervisor = deploy::tasks<4>;
Supervisor supervisor;
unsigned step{};
void require(myos_status_t status) noexcept {
    ++step;
    // Fixture exit code retains the failed check and original status.
    if (status != MYOS_STATUS_OK) exit(-static_cast<myos_status_t>(step * 100) + status);
}
void check(bool value) noexcept { require(value ? MYOS_STATUS_OK : MYOS_STATUS_INTERNAL); }
auto binding(const char* name, myos_cap_t channel) noexcept -> deploy::source {
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

void stale_channel(myos_cap_t pool, myos_cap_t cspace) noexcept {
    const auto old_pool = resource_create_child(pool, 256 * 1024, 64,
        MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL);
    require(old_pool.status);
    const auto terminal = notification_create(pool, 1);
    require(terminal.status);
    const auto old = channel_create(old_pool.value, 1, MYOS_CHANNEL_MAX_WORDS, 0, 1);
    require(old.status);
    const auto old_sender = channel_mint(old.value, cspace, 1, MYOS_RIGHT_SEND);
    require(old_sender.status);
    require(service::send(old_sender.value, {.id = 1}).status);
    service::Message message;
    require(service::receive(old.value2, message).status);
    check(message.id == 1);
    require(resource_close_async(old_pool.value, terminal.value, 1).status);
    require(notification_wait(terminal.value).status);

    const auto current = channel_create(pool, 1, MYOS_CHANNEL_MAX_WORDS, 0, 1);
    require(current.status);
    const auto current_sender = channel_mint(current.value, cspace, 1, MYOS_RIGHT_SEND);
    require(current_sender.status);
    check(service::send(old_sender.value, {.id = 2}, false).status == MYOS_STATUS_BUSY);
    require(cap_close(old_sender.value).status);
    check(service::send(old_sender.value, {.id = 2}, false).status == MYOS_STATUS_INVALID_CAP);
    require(service::send(current_sender.value, {.id = 3}).status);
    require(service::receive(current.value2, message).status);
    check(message.id == 3);
    check(service::receive(current.value2, message, false).status == MYOS_STATUS_WOULD_BLOCK);
    require(cap_close(old.value).status);
    require(cap_close(old.value2).status);
    require(cap_close(old_pool.value).status);
    require(object_destroy(terminal.value).status);
    require(cap_close(terminal.value).status);
    require(cap_close(current_sender.value).status);
    destroy(current);
}

void stale_service(myos_cap_t pool, myos_cap_t cspace) noexcept {
    const auto handoff = channel_create(pool, 4, MYOS_CHANNEL_MAX_WORDS, 1, 1);
    require(handoff.status);
    const auto control = channel_mint(handoff.value2, cspace, 1,
        MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE);
    require(control.status);
    const deploy::source source[] = {{"handoff", {handoff.value, 0}, {
        .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
        .kind = MYOS_OBJECT_KIND_CHANNEL, .size = MYOS_CAP_ATTENUATION_SIZE,
        .rights = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_DUPLICATE,
        .words = {0, 0, 0, 0}}}};
    auto next_cap = [&]() noexcept -> cap::OwnedCap {
        const auto begin = clock_now();
        require(begin.status);
        for (;;) {
            service::Message message{};
            cap::OwnedCap received;
            const auto result = service::receive_cap(control.value, message, received);
            if (result.status == MYOS_STATUS_WOULD_BLOCK) {
                const auto now = clock_now();
                require(now.status);
                check(now.value - begin.value < 1'000'000'000);
                yield();
                continue;
            }
            require(result.status);
            check(message.id == 1 && static_cast<bool>(received));
            return received;
        }
    };
    auto launch = [&]() noexcept -> Supervisor::handle {
        myos_status_t status{};
        auto task = supervisor.launch(program, Supervisor::name("provider"), status,
            {.sources = source});
        require(status);
        check(static_cast<bool>(task));
        return libk::move(*task);
    };
    struct Transfer final { cap::OwnedCap cap; Supervisor::handle holder; };
    auto transfer = [&](Supervisor::handle& provider) noexcept -> Transfer {
        const Supervisor::handle* providers[]{&provider};
        myos_status_t status{};
        auto holder = supervisor.launch(program, Supervisor::name("export-holder"), status,
            {.sources = source}, {providers, 1});
        require(status);
        check(static_cast<bool>(holder));
        auto received = next_cap();
        return {libk::move(received), libk::move(*holder)};
    };

    auto first = launch();
    auto old = transfer(first);
    require(service::send(old.cap.selector(), {.id = 41}).status);
    service::Message message{};
    require(service::receive(control.value, message).status);
    check(message.id == 4);
    require(service::send(control.value, {.id = 3}).status);
    check(supervisor.wait(first) == MYOS_STATUS_INTERNAL);
    check(supervisor.stop(old.holder) == MYOS_STATUS_CANCELED);

    auto second = launch();
    auto current = transfer(second);
    check(service::send(old.cap.selector(), {.id = 42}, false).status == MYOS_STATUS_BUSY);
    require(service::send(current.cap.selector(), {.id = 41}).status);
    require(service::receive(control.value, message).status);
    check(message.id == 4);
    require(service::send(control.value, {.id = 3}).status);
    check(supervisor.wait(second) == MYOS_STATUS_INTERNAL);
    check(supervisor.stop(current.holder) == MYOS_STATUS_CANCELED);
    require(cap_close(control.value).status);
    destroy(handoff);
}

}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = service::bootstrap(address, size);
    supervisor.open(info);
    require(supervisor.load(program, info));
    require(supervisor.add_boot_sources(info));
    const auto pool = service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    configured_capacity(pool, service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE));
    stale_channel(pool, service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE));
    stale_service(pool, service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE));
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
        const deploy::source sources[] = {binding("data", data.value), binding("ready", ready.value)};
        Supervisor::handle tasks[3];
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
