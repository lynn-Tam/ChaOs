#include <util.hpp>
#include <protocols/block.hpp>
#include <protocols/stream.hpp>
#include <servers/pci.hpp>
#include <ipc.hpp>
#include <cap.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>

test_image program;
test_tasks supervisor;

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
    open(program, info);
    supervisor.open(info.pool, fw_root(info).domain, info.vspace, info.cpus);
    const auto pair = sys::channel_create(info.pool, 1, CHANNEL_MAX_WORDS, 4, 2);
    if (pair.status != STATUS_OK) sys::exit(pair.status);
    cap_owner first{pair.value}, second{pair.value2};
    // Exercise the root execution's registered IPC page before clients start.
    // A user mapping alone would leave CHANNEL_TRY_RECV returning BAD_ARGS.
    const auto probe =
        sys::channel_mint(first.selector(), info.cspace, 1, RIGHT_RECEIVE);
    if (probe.status != STATUS_OK) sys::exit(probe.status);
    cap_owner probe_owner{probe.value};
    ch_msg empty{};
    if (ch_recv(probe_owner.selector(), empty, false).status != STATUS_WOULD_BLOCK)
        sys::exit(STATUS_INTERNAL);
    probe_owner = {};
    const auto storage = sys::resource_create_child(info.pool, 4 * 1024 * 1024, 0, OBJ_BIT(ObjMem));
    if (storage.status != STATUS_OK) sys::exit(storage.status);
    cap_owner memory_pool{storage.value};
    const pr_cap server_caps[]{{.contract=io_memory, .cap=storage.value, .limit=word_t{RIGHT_CREATE}}, host(selected->cap.selector(), selected->rid),
                               input(blk_service, pair.value2, RIGHT_RECEIVE, 1)};
    ch_pair lifecycle;
    if (const auto rc = lifecycle.open(info.pool, 1, 0, 1); rc != STATUS_OK) sys::exit(rc);
    const auto parent = sys::channel_mint(lifecycle.ends[0].selector(), info.cspace, 1,
                                         RIGHT_SEND | RIGHT_RECEIVE);
    if (parent.status != STATUS_OK) sys::exit(parent.status);
    cap_owner parent_end{parent.value};
    ch_port lifetime{parent.value, info.events};
    const pr_cap client_caps[]{input(blk_service, pair.value, RIGHT_SEND, 1),
        input(rt_stdout, lifecycle.ends[1].selector(), RIGHT_SEND | RIGHT_RECEIVE, 1)};
    const auto server =
        start(supervisor, program, "block", {.caps = server_caps, .resources = io_quota});
    const auto client =
        start(supervisor, program, "io-client", {.caps = client_caps, .resources = io_quota});
    // Keep client mappings live while the entire provider execution drains.
    ch_msg ready;
    for (;;) {
        const auto received = lifetime.try_receive(ready).status;
        if (received == STATUS_OK) break;
        if (received != STATUS_WOULD_BLOCK) sys::exit(received);
        const auto failed = supervisor.observe(client);
        if (failed.status != STATUS_OK || failed.value) sys::exit(STATUS_INTERNAL);
        sys::yield();
    }
    if (ready.id != 100) sys::exit(STATUS_PEER_FAULT);
    if (const auto rc = stop(supervisor, server); rc != STATUS_OK) sys::exit(rc);
    if (const auto rc = lifetime.send({.id=101}).status; rc != STATUS_OK) sys::exit(rc);
    const auto status = wait(supervisor, client);
    if (status != STATUS_OK) {
        (void) libk::fmt::format_to<"[io-session] client failed status={}\n">(port, status);
        sys::exit(status);
    }
    for (;;) {
        const auto retired = sys::resource_close(memory_pool.selector()).status;
        if (retired == STATUS_OK) break;
        if (retired != STATUS_BUSY && retired != STATUS_RETRY) sys::exit(retired);
        sys::yield();
    }
    port.write(
        "[io-session] ok: isolated pages, 32 outstanding, batched completion, cancel, close\n");
    sys::exit();
}
