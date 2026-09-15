#include <user/lib/mapped_memory.hpp>
#include <user/lib/supervisor.hpp>
#include <user/lib/uart.hpp>

namespace { myos::deploy::Program program; myos::deploy::Supervisor<1> supervisor; }

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    auto mapping = MappedMemory::map(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        cap::OwnedCap{{service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY), 0}},
        0x30010000, 4096, MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_DEVICE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping.value().address};
    port.reset();
    service::require(supervisor.load(program, info));
    supervisor.open(info);
    service::require(supervisor.add_boot_sources(info));
    service::require(supervisor.add("block.device", service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE),
        MYOS_OBJECT_KIND_DEVICE, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONNECT));
    for (size_t generation = 0; generation < 3; ++generation) {
        myos_status_t status{};
        auto task = supervisor.launch(program, "io-test", status);
        if (!task) {
            port.write("[io-user] launch failed\n");
            exit(status);
        }
        status = supervisor.wait(*task);
        if (status != MYOS_STATUS_OK) {
            uart::Printer printer{uart::Writer{port}};
            (void)printer.print<"[io-user] worker failed status={}\n">(status);
            exit(status);
        }
    }
    port.write("[io-user] ok: 32 outstanding, batch completion, task teardown, device reuse\n");
    exit();
}
