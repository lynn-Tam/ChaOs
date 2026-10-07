#include <servers/runtime/service.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/uart/port.hpp>

namespace { deploy::program program; deploy::tasks<1> supervisor; }

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    auto mapping = MappedMemory::map(service::capability(info, BOOT_VSPACE),
        cap::OwnedCap{{service::capability(info, boot::UartMem), 0}},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    supervisor.open(info);
    service::require(supervisor.load(program, info));
    service::require(supervisor.add_boot_sources(info));
    status_t status{};
    auto task = supervisor.launch(program, "channel-test", status);
    if (task) status = supervisor.wait(*task);
    if (status != STATUS_OK) {
        uart::Printer printer{uart::Writer{port}};
        (void)printer.print<"[channel] failed status={}\n">(status);
        exit(status);
    }
    port.write("[channel] service generations, stale cap denied, blocked senders, stop and reuse ok\n");
    exit();
}
