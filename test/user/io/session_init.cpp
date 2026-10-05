#include <servers/runtime/service.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/uart/port.hpp>

namespace { deploy::program program; deploy::tasks<2> supervisor; }

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    auto mapping = MappedMemory::map(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        cap::OwnedCap{{service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY), 0}},
        0x30010000, 4096, MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_DEVICE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    service::require(supervisor.load(program, info));
    supervisor.open(info);
    service::require(supervisor.add_boot_sources(info));
    service::require(supervisor.add("block.device", service::initial_device(info),
        MYOS_OBJECT_KIND_DEVICE, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONNECT));
    const auto pair = channel_create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
        1, MYOS_CHANNEL_MAX_WORDS, 4, 2);
    service::require(pair.status);
    cap::OwnedCap first{{pair.value, 0}}, second{{pair.value2, 0}};
    // Exercise the root execution's registered IPC page before clients start.
    // A user mapping alone would leave CHANNEL_TRY_RECV returning BAD_ARGS.
    const auto probe = channel_mint(first.selector(),
        service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), 1, MYOS_RIGHT_RECEIVE);
    service::require(probe.status);
    cap::OwnedCap probe_owner{{probe.value, 0}};
    service::Message empty{};
    if (service::receive(probe_owner.selector(), empty, false).status != MYOS_STATUS_WOULD_BLOCK)
        exit(MYOS_STATUS_INTERNAL);
    probe_owner = {};
    constexpr auto rights = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_DUPLICATE;
    service::require(supervisor.add("block.client", pair.value, MYOS_OBJECT_KIND_CHANNEL, rights, 0));
    service::require(supervisor.add("block.server", pair.value2, MYOS_OBJECT_KIND_CHANNEL, rights, 1));
    myos_status_t status{};
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
            exit(MYOS_STATUS_INTERNAL);
        }
        observed = supervisor.observe(*client);
        service::require(observed.status);
        if (observed.value != 0) break;
        yield();
    }
    status = supervisor.wait(*client);
    (void)supervisor.stop(*server);
    if (status != MYOS_STATUS_OK) {
        uart::Printer printer{uart::Writer{port}};
        (void)printer.print<"[io-session] client failed status={}\n">(status);
        exit(status);
    }
    port.write("[io-session] ok: isolated pages, 32 outstanding, batched completion, cancel, close\n");
    exit();
}
