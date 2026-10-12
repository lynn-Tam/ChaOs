#include <util.hpp>
#include <servers/pci.hpp>
#include <optional>
#include <ch.hpp>
#include <cap.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>

static pr_pkg program;
static pr_tasks supervisor;

int main() {

    const auto& info = rt_startup();
    auto pci = pci_bus::open(info, 0x3100'0000);
    if (!pci)
        sys::exit(pci.error());
    if (const auto rc = pci->configure(0x1042'1af4); rc != STATUS_OK)
        sys::exit(rc);
    if (pci->count(0x1042'1af4) != 2)
        sys::exit(STATUS_NOT_FOUND);
    auto mapping =
        vm_area::map(info.vspace, cap_owner{pci->firmware.console(ObjMem)},
                     0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping)
        sys::exit(mapping.error());
    uart_port port{mapping->address};
    port.reset();
    open(program, info);
    supervisor.open(info.pool, fw_root(info).domain, info.vspace, info.cpus);
    pr_id tasks[2];
    std::array<pci_binding, 2> devices;
    for (size_t i = 0; i < 2; ++i) {
        auto found = pci->find(0x1042'1af4, i);
        if (!found)
            sys::exit(found.error());
        devices[i] = std::move(*found);
        const pr_cap caps[]{host(devices[i].cap.selector(), devices[i].rid)};
        tasks[i] = start(supervisor, program, "io-test", {.caps = caps, .resources = io_quota});
    }
    for (size_t i = 0; i < 2; ++i) {
        const auto status = wait(supervisor, tasks[i]);
        if (status != STATUS_OK) {
            (void) libk::fmt::format_to<"[io-dual] worker={} status={}\n">(port, i, status);
            sys::exit(status);
        }
    }
    port.write("[io-dual] two devices completed independent DMA and teardown\n");
    sys::exit();
}
