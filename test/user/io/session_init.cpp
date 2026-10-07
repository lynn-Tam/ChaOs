#include <servers/runtime/service.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/uart/port.hpp>

namespace { deploy::program program; deploy::tasks<2> supervisor; }

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    auto mapping = MappedMemory::map(service::capability(info, BOOT_VSPACE),
        cap::OwnedCap{{service::capability(info, boot::UartMem), 0}},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    service::require(supervisor.load(program, info));
    supervisor.open(info);
    service::require(supervisor.add_boot_sources(info));
    service::require(supervisor.add("block.device", service::initial_device(info),
        OBJECT_KIND_DEVICE, RIGHT_DUPLICATE | RIGHT_CONNECT));
    const auto pair = channel_create(service::capability(info, BOOT_POOL),
        1, CHANNEL_MAX_WORDS, 4, 2);
    service::require(pair.status);
    cap::OwnedCap first{{pair.value, 0}}, second{{pair.value2, 0}};
    // Exercise the root execution's registered IPC page before clients start.
    // A user mapping alone would leave CHANNEL_TRY_RECV returning BAD_ARGS.
    const auto probe = channel_mint(first.selector(),
        service::capability(info, BOOT_CSPACE), 1, RIGHT_RECEIVE);
    service::require(probe.status);
    cap::OwnedCap probe_owner{{probe.value, 0}};
    service::Message empty{};
    if (service::receive(probe_owner.selector(), empty, false).status != STATUS_WOULD_BLOCK)
        exit(STATUS_INTERNAL);
    probe_owner = {};
    constexpr auto rights = RIGHT_SEND | RIGHT_RECEIVE | RIGHT_DUPLICATE;
    service::require(supervisor.add("block.client", pair.value, OBJECT_KIND_CHANNEL, rights, 0));
    service::require(supervisor.add("block.server", pair.value2, OBJECT_KIND_CHANNEL, rights, 1));
    status_t status{};
    auto server = supervisor.launch(program, "block", status);
    if (!server) {
        uart::Printer printer{uart::Writer{port}};
        (void)printer.print<"[io-session] server launch failed status={}\n">(status);
        exit(status);
    }
    auto client = supervisor.launch(program, "io-client", status);
    if (!client) {
        uart::Printer printer{uart::Writer{port}};
        (void)printer.print<"[io-session] client launch failed status={}\n">(status);
        exit(status);
    }
    for (;;) {
        auto observed = supervisor.observe(*server);
        service::require(observed.status);
        if (observed.value != 0) {
            status = supervisor.wait(*server);
            (void)supervisor.stop(*client);
            uart::Printer printer{uart::Writer{port}};
            (void)printer.print<"[io-session] server failed status={}\n">(status);
            exit(STATUS_INTERNAL);
        }
        observed = supervisor.observe(*client);
        service::require(observed.status);
        if (observed.value != 0) break;
        yield();
    }
    status = supervisor.wait(*client);
    (void)supervisor.stop(*server);
    if (status != STATUS_OK) {
        uart::Printer printer{uart::Writer{port}};
        (void)printer.print<"[io-session] client failed status={}\n">(status);
        exit(status);
    }
    port.write("[io-session] ok: isolated pages, 32 outstanding, batched completion, cancel, close\n");
    exit();
}
