#include <servers/runtime/service.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/uart/port.hpp>

namespace { deploy::program program; deploy::tasks<1> supervisor; }

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    auto mapping = MappedMemory::map(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        cap::OwnedCap{{service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY), 0}},
        0x30010000, 4096, MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_DEVICE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    supervisor.open(info);
    service::require(supervisor.load(program, info));
    service::require(supervisor.add_boot_sources(info));
    myos_status_t status{};
    auto task = supervisor.launch(program, "channel-test", status);
    if (task) status = supervisor.wait(*task);
    if (status != MYOS_STATUS_OK) {
        uart::Printer printer{uart::Writer{port}};
        (void)printer.print<"[channel] failed status={}\n">(status);
        exit(status);
    }
    port.write("[channel] service generations, stale cap denied, blocked senders, stop and reuse ok\n");
    exit();
}
