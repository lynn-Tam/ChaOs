#include <sys/handle.hpp>
#include <servers/runtime/service.hpp>
#include <servers/uart/port.hpp>
#include <uapi/io.h>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    auto mapping = MappedMemory::map(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        cap::OwnedCap{{service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY), 0}},
        0x30010000, 4096, MYOS_VM_READ | MYOS_VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    uart::Printer printer{uart::Writer{port}};
    (void)printer.print<"[device-inventory] count={}\n">(info.device_count());
    if (info.device_count() != 2) exit(MYOS_STATUS_NOT_FOUND);
    for (size_t i = 0; i < 2; ++i) {
        const auto cap = info.device(i);
        const auto query = device_info(cap);
        (void)printer.print<"[device-inventory] device={} query={}\n">(i, query.status);
        service::require(query.status);
        myos_device_info device{};
        service::copy(&device, reinterpret_cast<const void*>(service::IpcAddress), sizeof(device));
        bool bar{};
        for (auto size : device.bar_sizes) bar |= size != 0;
        (void)printer.print<"[device-inventory] requester={:#x} id={:#x} bar={}\n">(
            device.requester, device.configuration[0], bar);
        if (device.version != MYOS_DEVICE_INFO_VERSION
            || device.requester != (i + 1) << 3
            || device.configuration[0] != 0x1042'1af4
            || !bar) exit(MYOS_STATUS_INTERNAL);
    }
    port.write("[device-inventory] two distinct PCI Device capabilities\n");
    exit();
}
