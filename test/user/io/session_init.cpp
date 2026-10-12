#include <util.hpp>
#include <proto.hpp>
#include <servers/pci.hpp>
#include <ch.hpp>
#include <cap.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>

pr_pkg program;
pr_tasks supervisor;

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
    const pr_cap server_caps[]{host(selected->cap.selector(), selected->rid),
                               input(blk_service, pair.value2, RIGHT_RECEIVE, 1)};
    const pr_cap client_caps[]{input(blk_service, pair.value, RIGHT_SEND, 1)};
    const auto server =
        start(supervisor, program, "block", {.caps = server_caps, .resources = io_quota});
    const auto client =
        start(supervisor, program, "io-client", {.caps = client_caps, .resources = io_quota});
    status_t status{};
    for (;;) {
        auto observed = supervisor.observe(server);
        if (observed.status != STATUS_OK) sys::exit(observed.status);
        if (observed.value != 0) {
            status = wait(supervisor, server);
            (void) stop(supervisor, client);
            (void) libk::fmt::format_to<"[io-session] server failed status={}\n">(port, status);
            sys::exit(STATUS_INTERNAL);
        }
        observed = supervisor.observe(client);
        if (observed.status != STATUS_OK) sys::exit(observed.status);
        if (observed.value != 0)
            break;
        sys::yield();
    }
    status = wait(supervisor, client);
    (void) stop(supervisor, server);
    if (status != STATUS_OK) {
        (void) libk::fmt::format_to<"[io-session] client failed status={}\n">(port, status);
        sys::exit(status);
    }
    port.write(
        "[io-session] ok: isolated pages, 32 outstanding, batched completion, cancel, close\n");
    sys::exit();
}
