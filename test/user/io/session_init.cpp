#include <sys/protocol.hpp>
#include <sys/pci.hpp>
#include <sys/channel.hpp>
#include <sys/handle.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>

namespace { proc::package program; proc::tasks<2> supervisor; }

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    auto pci = sys::pci::Bus::open(info, 0x3100'0000);
    if (!pci) exit(pci.error());
    sys::require(pci->configure(0x1042'1af4));
    auto selected = pci->find(0x1042'1af4, 0);
    if (!selected) sys::exit(selected.error());
    auto mapping = mapping::map(sys::capability(info, BOOT_VSPACE),
        cap_owner{pci->firmware.console(ObjMem)},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    open(program, info);
    supervisor.open(info);
    const auto pair = channel_create(sys::capability(info, BOOT_POOL),
        1, CHANNEL_MAX_WORDS, 4, 2);
    sys::require(pair.status);
    cap_owner first{pair.value}, second{pair.value2};
    // Exercise the root execution's registered IPC page before clients start.
    // A user mapping alone would leave CHANNEL_TRY_RECV returning BAD_ARGS.
    const auto probe = channel_mint(first.selector(),
        sys::capability(info, BOOT_CSPACE), 1, RIGHT_RECEIVE);
    sys::require(probe.status);
    cap_owner probe_owner{probe.value};
    sys::Message empty{};
    if (sys::receive(probe_owner.selector(), empty, false).status != STATUS_WOULD_BLOCK)
        exit(STATUS_INTERNAL);
    probe_owner = {};
    const proc::cap_src server_caps[]{host(selected->cap.selector(), selected->rid),
        input(boot::Block, pair.value2, RIGHT_RECEIVE, 1)};
    const proc::cap_src client_caps[]{input(boot::Block, pair.value, RIGHT_SEND, 1)};
    const auto server = start(supervisor, program, "block", {.caps = server_caps, .resources = io_quota});
    const auto client = start(supervisor, program, "io-client", {.caps = client_caps, .resources = io_quota});
    status_t status{};
    for (;;) {
        auto observed = supervisor.observe(server);
        sys::require(observed.status);
        if (observed.value != 0) {
            status = wait(supervisor, server);
            (void)stop(supervisor, client);
            uart::Printer printer{uart::Writer{port}};
            (void)printer.print<"[io-session] server failed status={}\n">(status);
            exit(STATUS_INTERNAL);
        }
        observed = supervisor.observe(client);
        sys::require(observed.status);
        if (observed.value != 0) break;
        yield();
    }
    status = wait(supervisor, client);
    (void)stop(supervisor, server);
    if (status != STATUS_OK) {
        uart::Printer printer{uart::Writer{port}};
        (void)printer.print<"[io-session] client failed status={}\n">(status);
        exit(status);
    }
    port.write("[io-session] ok: isolated pages, 32 outstanding, batched completion, cancel, close\n");
    exit();
}
