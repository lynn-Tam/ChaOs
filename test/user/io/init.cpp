#include <sys/pci.hpp>
#include <sys/channel.hpp>
#include <sys/handle.hpp>
#include <test/user/task.hpp>
#include <servers/uart/port.hpp>

namespace { proc::package program; proc::tasks<1> supervisor; }

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
    const proc::cap_src caps[]{host(selected->cap.selector(), selected->rid)};
    for (size_t generation = 0; generation < 3; ++generation) {
        const auto task = start(supervisor, program, "io-test", {.caps = caps, .resources = io_quota});
        const auto status = wait(supervisor, task);
        if (status != STATUS_OK) {
            uart::Printer printer{uart::Writer{port}};
            (void)printer.print<"[io-user] worker failed status={}\n">(status);
            exit(status);
        }
    }
    port.write("[io-user] ok: 32 outstanding, batch completion, task teardown, device reuse\n");
    exit();
}
