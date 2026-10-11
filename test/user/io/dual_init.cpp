#include <sys/pci.hpp>
#include <optional>
#include <sys/channel.hpp>
#include <sys/handle.hpp>
#include <test/user/task.hpp>
#include <servers/uart/port.hpp>

namespace {
proc::package program;
proc::tasks<2> supervisor;


}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    auto pci = sys::pci::Bus::open(info, 0x3100'0000);
    if (!pci) exit(pci.error());
    sys::require(pci->configure(0x1042'1af4));
    if (pci->count(0x1042'1af4) != 2) exit(STATUS_NOT_FOUND);
    auto mapping = mapping::map(sys::capability(info, BOOT_VSPACE),
        cap_owner{pci->firmware.console(ObjMem)},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    open(program, info);
    supervisor.open(info);
    proc::task_id tasks[2];
    std::array<sys::pci::Binding, 2> devices;
    for (size_t i = 0; i < 2; ++i) {
        auto found = pci->find(0x1042'1af4, i);
        if (!found) exit(found.error());
        devices[i] = std::move(*found);
        const proc::cap_src caps[]{host(devices[i].cap.selector(), devices[i].rid)};
        tasks[i] = start(supervisor, program, "io-test", {.caps = caps, .resources = io_quota});
    }
    for (size_t i = 0; i < 2; ++i) {
        const auto status = wait(supervisor, tasks[i]);
        if (status != STATUS_OK) {
            uart::Printer printer{uart::Writer{port}};
            (void)printer.print<"[io-dual] worker={} status={}\n">(i, status);
            exit(status);
        }
    }
    port.write("[io-dual] two devices completed independent DMA and teardown\n");
    exit();
}
