#include "../task.hpp"
#include <proc.hpp>
#include <posix.hpp>
#include <unistd.h>
#include <thread.hpp>
#include <vector>

int main() {
    if (const auto status = px_init(); status != STATUS_OK) return status;
    const auto& info = _startup();
    const auto check = [](bool ok) {
        if (!ok)
            _thread_exit(STATUS_INTERNAL);
    };

    const auto notification = _port_create(info.alloc(), 8, 4);
    if (notification.status != STATUS_OK)
        _thread_exit(notification.status);
    _cap_owner events{notification.value};
    ubus_conn directory, files;
    auto status = directory.bind(info.bootstrap);
    ubus_msg query, response; query.data.put("name", "exec");
    if (status == STATUS_OK) status = directory.invoke("Get", std::move(query), response);
    if (status == STATUS_OK) status = files.bind(response.capability(response.data["connector"]));
    if (status != STATUS_OK) _thread_exit(status);
    query = {}; query.data.put("path", "/boot/sleep.elf").put("perms", word_t{VM_READ | VM_EXECUTE});
    check(files.invoke("Map", std::move(query), response) == STATUS_OK);
    const auto buffer = response.data["buffer"].as<ubus_buffer>(); check(bool(buffer));
    auto memory = response.take(response.data["buffer"]);
    test_image image;
    if (const auto rc =
            image.open(info.vspace, memory.selector(), buffer->size, VM_READ | VM_EXECUTE | VM_MAP_PRIVATE);
        rc != STATUS_OK)
        _thread_exit(rc);
    test_tasks tasks;
    tasks.open(info.alloc(), info.domain, info.cpus);
    const auto spawn = [&](const char* ms) {
        const std::string_view args[]{"sleep", ms};
        const auto child = tasks.spawn(
            image, {},
            {.argv = args, .resources = {.bytes = 1024 * 1024, .caps = 64}, .events = &_events()});
        if (child.status != STATUS_OK)
            _thread_exit(child.status);
        return child.id;
    };
    const auto wait = [&](test_id id, uint64_t deadline = 0) {
        for (;;) {
            const auto result = tasks.collect(id);
            if (result.status == STATUS_OK)
                return static_cast<status_t>(result.value);
            if (result.status != STATUS_WOULD_BLOCK && !(result.status == STATUS_BUSY || result.status == STATUS_RETRY))
                return result.status;
            const auto now = _clock_now();
            if (now.status != STATUS_OK)
                _thread_exit(now.status);
            if (deadline && now.value >= deadline)
                return STATUS_TIMED_OUT;
            _yield();
        }
    };
    const auto first = spawn("10000");
    const auto deadline = _after(std::chrono::milliseconds{1});
    check(deadline.has_value());
    check(wait(first, *deadline) == STATUS_TIMED_OUT);
    if (const auto rc = tasks.request_stop(first); rc != STATUS_OK)
        _thread_exit(rc);
    check(wait(first) == STATUS_CANCELED);
    check(wait(first) == STATUS_INVALID_CAP);
    // More live/unconsumed records than the removed four-slot proxy admitted.
    std::vector<test_id> children;
    for (unsigned i = 0; i < 6; ++i)
        children.push_back(spawn("1"));
    for (auto id : children)
        check(wait(id) == STATUS_OK);
    const auto reused = spawn("10000");
    for (auto id : children) {
        check(id != reused);
        check(wait(id) == STATUS_INVALID_CAP);
    }
    if (const auto rc = tasks.request_stop(reused); rc != STATUS_OK)
        _thread_exit(rc);
    check(wait(reused) == STATUS_CANCELED);
    for (unsigned i = 0; i < 6; ++i) {
        const auto id = spawn("10000");
        if (const auto rc = tasks.request_stop(id); rc != STATUS_OK)
            _thread_exit(rc);
        check(wait(id) == STATUS_CANCELED);
    }
    constexpr std::string_view message{"[process] direct spawn, retention, deadlines and reuse ok\n"};
    check(write(STDOUT_FILENO, message.data(), message.size()) == int64_t(message.size()));
    return STATUS_OK;
}
