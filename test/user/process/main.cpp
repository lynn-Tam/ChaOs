#include <test/user/task.hpp>
#include <proc.hpp>
#include <lib/fs/fs.hpp>
#include <lib/stream/stream.hpp>
#include <util.hpp>
#include <vector>

int main() {
    const auto& info = rt_startup();
    const auto check = [](bool ok) {
        if (!ok)
            sys::exit(STATUS_INTERNAL);
    };

    const auto notification = sys::port_create(info.alloc(), 8, 4);
    if (notification.status != STATUS_OK)
        sys::exit(notification.status);
    cap_owner events{notification.value};
    ubus_conn directory, files;
    auto status = directory.bind(info.selector(ubus_directory));
    if (status == STATUS_OK) status = files.connect(directory, "exec", "myos.files", 1, 1);
    if (status != STATUS_OK) sys::exit(status);
    auto mem = fs_map(files, "/boot/sleep.elf", VM_READ | VM_EXECUTE);
    check(mem.has_value());
    test_image image;
    if (const auto rc =
            image.open(info.vspace, mem->memory.selector(), mem->size, VM_READ | VM_EXECUTE | VM_MAP_PRIVATE);
        rc != STATUS_OK)
        sys::exit(rc);
    test_tasks tasks;
    tasks.open(info.alloc(), info.domain, info.cpus);
    const auto spawn = [&](const char* ms) {
        const std::string_view args[]{"sleep", ms};
        const auto child = tasks.spawn(
            image, {},
            {.argv = args, .resources = {.bytes = 1024 * 1024, .caps = 64}, .events = info.events});
        if (child.status != STATUS_OK)
            sys::exit(child.status);
        return child.id;
    };
    const auto wait = [&](test_id id, uint64_t deadline = 0) {
        for (;;) {
            const auto result = tasks.collect(id);
            if (result.status == STATUS_OK)
                return static_cast<status_t>(result.value);
            if (result.status != STATUS_WOULD_BLOCK && !proc_retryable(result.status))
                return result.status;
            const auto now = sys::clock_now();
            if (now.status != STATUS_OK)
                sys::exit(now.status);
            if (deadline && now.value >= deadline)
                return STATUS_TIMED_OUT;
            sys::yield();
        }
    };
    const auto first = spawn("10000");
    const auto deadline = rt_after_ms(1);
    check(deadline.has_value());
    check(wait(first, *deadline) == STATUS_TIMED_OUT);
    if (const auto rc = tasks.request_stop(first); rc != STATUS_OK)
        sys::exit(rc);
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
        sys::exit(rc);
    check(wait(reused) == STATUS_CANCELED);
    for (unsigned i = 0; i < 6; ++i) {
        const auto id = spawn("10000");
        if (const auto rc = tasks.request_stop(id); rc != STATUS_OK)
            sys::exit(rc);
        check(wait(id) == STATUS_CANCELED);
    }
    str_writer{info.selector(rt_stdout)}.write(
        "[process] direct spawn, retention, deadlines and reuse ok\n");
    sys::exit();
}
