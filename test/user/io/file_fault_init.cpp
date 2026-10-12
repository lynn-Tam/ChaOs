#include <util.hpp>
#include <proto.hpp>
#include <servers/pci.hpp>
#include <utility>
#include <ch.hpp>
#include <cap.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>
#include "file_fault.hpp"

static pr_pkg program;
using Supervisor = pr_tasks;
Supervisor supervisor;
static void check(bool condition) noexcept {
    if (!condition)
        sys::exit(STATUS_INTERNAL);
}
static auto input(uart_port port) -> uint8_t {
    uint8_t value{};
    // Fixture-only host barrier; ordinary services keep their normal IRQ path.
    while (!port.try_get(value))
        sys::yield();
    return value;
}

int main() {
    const auto& info = rt_startup();
    auto pci = pci_bus::open(info, 0x3100'0000);
    if (!pci)
        sys::exit(pci.error());
    if (const auto rc = pci->configure(0x1042'1af4); rc != STATUS_OK)
        sys::exit(rc);
    auto selected = pci->find(0x1042'1af4, 0);
    if (!selected)
        sys::exit(selected.error());
    auto mapping =
        vm_area::map(info.vspace, cap_owner{pci->firmware.console(ObjMem)},
                     0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping)
        sys::exit(mapping.error());
    uart_port port{mapping->address};
    port.reset();
    supervisor.open(info.pool, fw_root(info).domain, info.vspace, info.cpus);
    open(program, info);
    const auto pool = info.pool;
    cap_owner endpoints[4];
    for (size_t i = 0; i != 2; ++i) {
        const auto pair = sys::channel_create(pool, i == 0 ? 1 : 8, CHANNEL_MAX_WORDS, 4, 2);
        if (pair.status != STATUS_OK) sys::exit(pair.status);
        endpoints[2 * i] = cap_owner{pair.value};
        endpoints[2 * i + 1] = cap_owner{pair.value2};
    }
    const pr_cap block_caps[]{host(selected->cap.selector(), selected->rid),
                              input(blk_service, endpoints[1].selector(), RIGHT_RECEIVE, 1)};
    const pr_cap files_caps[]{input(blk_service, endpoints[0].selector(), RIGHT_SEND, 1),
                              input(fs_service, endpoints[3].selector(), RIGHT_RECEIVE, 1)};
    pr_id tasks[4];
    for (unsigned i = 0; i != 2; ++i) {
        tasks[i] = start(supervisor, program, i == 0 ? "block" : "files",
                         {.caps = i == 0 ? std::span<const pr_cap>{block_caps}
                                         : std::span<const pr_cap>{files_caps},
                          .resources = i == 0 ? io_quota : files_quota});
    }
    const auto ready = sys::notification_create(pool, 1);
    if (ready.status != STATUS_OK) sys::exit(ready.status);
    cap_owner go[2];
    for (unsigned i = 0; i != 2; ++i) {
        const auto signal = sys::notification_create(pool, 1);
        if (signal.status != STATUS_OK) sys::exit(signal.status);
        go[i] = cap_owner{signal.value};
        const pr_cap caps[]{input(fs_service, endpoints[2].selector(), RIGHT_SEND, 1),
                            input(ft_ready, ready.value, RIGHT_SIGNAL),
                            input(ft_go, signal.value, RIGHT_RECEIVE)};
        tasks[2 + i] =
            start(supervisor, program, "file-client", {.caps = caps, .resources = io_quota});
        if (const auto rc = sys::notification_wait(ready.value).status; rc != STATUS_OK)
            sys::exit(rc);
    }
    port.write("[file-fault] mapped, awaiting host\n");
    check(input(port) == 'g');
    for (auto& signal : go)
        if (const auto rc = sys::notification_signal(signal.selector()).status; rc != STATUS_OK)
            sys::exit(rc);
    const auto mode = input(port);
    check(mode == 'r' || mode == 'k');
    if (mode == 'k') {
        port.write("[file-fault] stopping Files with page-in pending\n");
        const auto status = stop(supervisor, tasks[1]);
        (void) libk::fmt::format_to<"[file-fault] Files stop status={}\n">(port, status);
        check(status == STATUS_CANCELED);
    }
    for (unsigned i = 2; i != 4; ++i) {
        const auto status = wait(supervisor, tasks[i]);
        (void) libk::fmt::format_to<"[file-fault] client {} status={}\n">(port, i, status);
        check(status == (mode == 'k' ? STATUS_PEER_FAULT : STATUS_OK));
    }
    const auto block = supervisor.observe(tasks[0]);
    (void) libk::fmt::format_to<"[file-fault] Block observe status={} value={} result={}\n">(port,
        block.status, block.value, block.value2);
    check(block.status == STATUS_OK && block.value == 0);
    if (mode == 'r')
        check(stop(supervisor, tasks[1]) == STATUS_CANCELED);
    const auto block_stop = stop(supervisor, tasks[0]);
    (void) libk::fmt::format_to<"[file-fault] Block stop status={}\n">(port, block_stop);
    check(block_stop == STATUS_CANCELED);
    port.write(mode == 'k' ? "[file-fault] Files death released both faults, Block survived\n"
                           : "[file-fault] shared request supplied both mappings\n");
    sys::exit();
}
