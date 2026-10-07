#include <sys/handle.hpp>
#include <servers/runtime/service.hpp>
#include <servers/uart/port.hpp>
#include <uapi/io.h>

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    auto mapping = MappedMemory::map(service::capability(info, BOOT_VSPACE),
        cap::OwnedCap{{service::capability(info, boot::UartMem), 0}},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    uart::Printer printer{uart::Writer{port}};
    (void)printer.print<"[device-inventory] count={}\n">(info.device_count());
    if (info.device_count() != 2) exit(STATUS_NOT_FOUND);
    for (size_t i = 0; i < 2; ++i) {
        const auto cap = info.device(i);
        const auto query = device_info(cap);
        (void)printer.print<"[device-inventory] device={} query={}\n">(i, query.status);
        service::require(query.status);
        DeviceDesc device{};
        service::copy(&device, reinterpret_cast<const void*>(service::IpcAddress), sizeof(device));
        bool bar{};
        for (auto size : device.bar_sizes) bar |= size != 0;
        (void)printer.print<"[device-inventory] requester={:#x} id={:#x} bar={}\n">(
            device.requester, device.configuration[0], bar);
        if (device.version != DEVICE_INFO_VERSION
            || device.requester != (i + 1) << 3
            || device.configuration[0] != 0x1042'1af4
            || !bar) exit(STATUS_INTERNAL);
    }
    port.write("[device-inventory] two distinct PCI Device capabilities\n");
    exit();
}
