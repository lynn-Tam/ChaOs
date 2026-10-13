#include <util.hpp>
#include <servers/pci.hpp>
#include <ipc.hpp>
#include <cap.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>

test_image program;
test_tasks supervisor;

int main() {

    const auto& info = rt_startup();
    auto pci = pci_bus::open(info, 0x3100'0000);
    if (!pci)
        sys::exit(pci.error());
    if (const auto rc = pci->configure(0x1042'1af4); rc != STATUS_OK)
        sys::exit(rc);
    auto selected = pci->find(0x1042'1af4, 0);
    if (!selected)
        sys::exit(selected.error());
    auto vm_area =
        vm_area::map(info.vspace, cap_owner{pci->firmware.console(ObjMem)},
                     0x30010000, 4096, VM_READ | VM_WRITE);
    if (!vm_area)
        sys::exit(vm_area.error());
    uart_port port{vm_area->address};
    port.reset();
    open(program, info);
    supervisor.open(info.pool, fw_root(info).domain, info.vspace, info.cpus);
    const pr_cap caps[]{host(selected->cap.selector(), selected->rid)};
    for (size_t generation = 0; generation < 3; ++generation) {
        const auto task =
            start(supervisor, program, "io-test", {.caps = caps, .resources = io_quota});
        const auto status = wait(supervisor, task);
        if (status != STATUS_OK) {
            (void) libk::fmt::format_to<"[io-user] worker failed status={}\n">(port, status);
            sys::exit(status);
        }
    }
    port.write("[io-user] ok: 32 outstanding, batch completion, task teardown, device reuse\n");
    sys::exit();
}
