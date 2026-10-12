#include <proc.hpp>
#include <proto.hpp>
#include <util.hpp>
#include <vector>
#include <servers/fw.hpp>

int main() {
    const auto& info = rt_startup();
    const auto check = [](bool ok) {
        if (!ok)
            sys::exit(STATUS_INTERNAL);
    };
    rt_clock clock;
    if (const auto rc = clock.open(); rc != STATUS_OK)
        sys::exit(rc);
    const auto notification = sys::notification_create(info.pool, 1);
    if (notification.status != STATUS_OK) sys::exit(notification.status);
    cap_owner events{notification.value};
    io_client files;
    uint64_t value{};
    if (const auto rc = files.connect(info.selector(fs_service), info.pool,
                                      info.cspace, events.selector(),
                                      info.vspace, 0, value);
        rc != STATUS_OK)
        sys::exit(rc);
    fs_file file;
    if (const auto rc = fs_open(files, "SLEEP.PKG", 9, file); rc != STATUS_OK)
        sys::exit(rc);
    auto mem = fs_backing(files, file, VM_READ | VM_EXECUTE);
    if (const auto rc = fs_close(files, file); rc != STATUS_OK)
        sys::exit(rc);
    check(mem.has_value());
    pr_pkg pkg;
    if (const auto rc = pkg.open(info.vspace, mem->memory.selector(),
                                 {0, pr_window::round_size(mem->size)}, mem->size,
                                 VM_READ | VM_EXECUTE | VM_MAP_PRIVATE);
        rc != STATUS_OK)
        sys::exit(rc);
    pr_tasks tasks;
    tasks.open(info.pool, fw_root(info).domain, info.vspace, info.cpus);
    const auto spawn = [&](const char* ms) {
        const std::string_view args[]{"sleep", ms};
        const auto child = tasks.spawn(pkg, {},
                                       {.argv = args,
                                        .resources = {.bytes = 1024 * 1024, .caps = 64},
                                        .events = info.events});
        if (child.status != STATUS_OK) sys::exit(child.status);
        return child.id;
    };
    const auto wait = [&](pr_id id, uint64_t deadline = 0) {
        for (;;) {
            const auto result = tasks.collect(id);
            if (result.status == STATUS_OK)
                return static_cast<status_t>(result.value);
            if (result.status != STATUS_WOULD_BLOCK && !pr_retryable(result.status))
                return result.status;
            const auto now = sys::clock_now();
            if (now.status != STATUS_OK) sys::exit(now.status);
            if (deadline && now.value >= deadline)
                return STATUS_TIMED_OUT;
            sys::yield();
        }
    };
    const auto first = spawn("10000");
    const auto deadline = clock.after_ms(1);
    check(deadline.has_value());
    check(wait(first, *deadline) == STATUS_TIMED_OUT);
    if (const auto rc = tasks.request_stop(first); rc != STATUS_OK)
        sys::exit(rc);
    check(wait(first) == STATUS_CANCELED);
    check(wait(first) == STATUS_INVALID_CAP);
    // More live/unconsumed records than the removed four-slot proxy admitted.
    std::vector<pr_id> children;
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
    str_writer{info.selector(con_output)}.write(
        "[process] direct spawn, retention, deadlines and reuse ok\n");
    sys::exit();
}
