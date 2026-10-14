#include <protocols/msg.hpp>
#include <test/user/task.hpp>
#include <exec.hpp>
#include <protocols/file.hpp>
#include <protocols/block.hpp>
#include <protocols/service.hpp>
#include <protocols/stream.hpp>
#include <util.hpp>
#include <vector>

int main() {
    const auto& info = rt_startup();
    const auto check = [](bool ok) {
        if (!ok)
            sys::exit(STATUS_INTERNAL);
    };

    const auto notification = sys::notification_create(info.pool, 1);
    if (notification.status != STATUS_OK)
        sys::exit(notification.status);
    cap_owner events{notification.value};
    rpc_client directory;
    if (const auto rc =
            directory.open(info.selector(sv_service), info.pool, info.cspace, info.events);
        rc != STATUS_OK)
        sys::exit(rc);
    auto endpoint = sv_lookup(directory, vfs_exec);
    if (!endpoint)
        sys::exit(endpoint.error());
    const auto ungranted = sv_lookup(directory, blk_service);
    check(!ungranted && ungranted.error() == STATUS_NOT_FOUND);
    auto wrong = vfs_exec;
    ++wrong.major;
    const auto mismatch = sv_lookup(directory, wrong);
    check(!mismatch && mismatch.error() == STATUS_NOT_FOUND);
    rpc_client files;
    if (const auto rc = files.open(endpoint->selector(), info.pool, info.cspace, events.selector());
        rc != STATUS_OK)
        sys::exit(rc);
    auto mem = fl_map(files, "/boot/sleep.elf", VM_READ | VM_EXECUTE);
    check(mem.has_value());
    test_image image;
    if (const auto rc =
            image.open(info.vspace, mem->memory.selector(), mem->size, VM_READ | VM_EXECUTE | VM_MAP_PRIVATE);
        rc != STATUS_OK)
        sys::exit(rc);
    test_tasks tasks;
    tasks.open(info.pool, info.selector(rt_domain), info.vspace, info.cpus);
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
            if (result.status != STATUS_WOULD_BLOCK && !pr_retryable(result.status))
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
    str_writer{info.selector(con_output)}.write(
        "[process] direct spawn, retention, deadlines and reuse ok\n");
    sys::exit();
}
