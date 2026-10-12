#include <util.hpp>
#include <servers/pci.hpp>
#include <servers/virtio_blk.hpp>
#include <servers/uart.hpp>

int main() {

    const auto& info = rt_startup();
    auto pci = pci_bus::open(info, 0x3100'0000);
    if (!pci)
        sys::exit(pci.error());
    if (const auto rc = pci->configure(0x1042'1af4); rc != STATUS_OK)
        sys::exit(rc);
    const auto vm = info.vspace;
    auto vm_area = vm_area::map(vm, cap_owner{pci->firmware.console(ObjMem)}, 0x30010000, 4096,
                                VM_READ | VM_WRITE);
    if (!vm_area)
        sys::exit(vm_area.error());
    uart_port port{vm_area->address};
    port.reset();
    if (pci->count(0x1042'1af4) != 2)
        sys::exit(STATUS_NOT_FOUND);
    std::array<pci_binding, 2> devices;
    for (size_t i = 0; i < devices.size(); ++i) {
        auto found = pci->find(0x1042'1af4, i);
        if (!found)
            sys::exit(found.error());
        devices[i] = std::move(*found);
    }
    if (devices[0].rid == devices[1].rid)
        sys::exit(STATUS_BAD_ARGS);
    const CapView outside{CAP_ATTENUATION_VERSION_CURRENT,
                          ObjHost,
                          CAP_ATTENUATION_SIZE,
                          RIGHT_CONNECT,
                          {devices[1].rid, 1}};
    cap_encode(outside, *reinterpret_cast<uint8_t (*)[CAP_ATTENUATION_SIZE]>(pci->desc.address));
    if (sys::cap_typed_delegate(devices[0].cap.selector(), 0, pci->desc.memory.selector(), 0)
            .status != STATUS_DENIED)
        sys::exit(STATUS_INTERNAL);
    const auto pool = info.pool;
    {
        const auto* firmware = &fw_root(info);
        if (!firmware || firmware->firmware_bytes < 40)
            sys::exit(STATUS_BAD_ARGS);
        const auto copy =
            sys::cap_duplicate(firmware->firmware, info.cspace, RIGHT_MAP);
        if (copy.status != STATUS_OK) sys::exit(copy.status);
        const size_t offset = firmware->firmware_offset;
        const size_t bytes = (offset + firmware->firmware_bytes + 4095) & ~size_t{4095};
        auto view = vm_area::map(vm, cap_owner{copy.value}, 0x32000000, bytes, VM_READ);
        if (!view)
            sys::exit(view.error());
        const auto* header = reinterpret_cast<volatile const uint8_t*>(view->address + offset);
        if (header[0] != 0xd0 || header[1] != 0x0d || header[2] != 0xfe || header[3] != 0xed)
            sys::exit(STATUS_BAD_ARGS);
    }
    const auto events = sys::notification_create(pool, rt_event_badge);
    if (events.status != STATUS_OK) sys::exit(events.status);
    cap_owner wake{events.value};
    {
        const auto mem = sys::memory_create(pool, 4096, VM_READ | VM_WRITE);
        const auto created = sys::io_space_create(pool);
        if (mem.status != STATUS_OK) sys::exit(mem.status);
        if (created.status != STATUS_OK) sys::exit(created.status);
        cap_owner arena{mem.value}, space{created.value};
        if (sys::io_space_bind(created.value, pci->host, mem.value, 0, 1, 0x100000).status !=
            STATUS_BAD_RIGHTS)
            sys::exit(STATUS_INTERNAL);
        if (const auto rc = sys::io_space_bind(created.value, devices[0].cap.selector(), mem.value,
                                               0, 1, 0x100000)
                                .status;
            rc != STATUS_OK)
            sys::exit(rc);
        for (;;) {
            const auto state = sys::io_space_state(created.value);
            if (state.status != STATUS_OK) sys::exit(state.status);
            if (state.value == IO_SPACE_ACTIVE)
                break;
            if (state.value != IO_SPACE_OPENING)
                sys::exit(STATUS_BACKING_FAILED);
            sys::yield();
        }
        const auto reg = sys::io_space_reg(created.value, IO_PCI_CFG);
        if (reg.status != STATUS_OK) sys::exit(reg.status);
        cap_owner cfg{reg.value};
        const auto slice = sys::vm_slice(vm, 0x62001000, 4096, VM_READ | VM_WRITE,
                                         RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
        if (slice.status != STATUS_OK) sys::exit(slice.status);
        cap_owner writable{slice.value};
        if (reg.value2 != 4096 ||
            sys::vm_map(slice.value, reg.value, 0x62001000, 4096, 0, VM_READ | VM_WRITE).status !=
                STATUS_BAD_RIGHTS)
            sys::exit(STATUS_BAD_RIGHTS);
        if (const auto rc = sys::vm_clear(slice.value).status; rc != STATUS_OK)
            sys::exit(rc);
        auto view = vm_area::map(vm, std::move(cfg), 0x62000000, 4096, VM_READ);
        if (!view)
            sys::exit(view.error());
        if (*reinterpret_cast<volatile const uint32_t*>(view->address) != 0x1042'1af4)
            sys::exit(STATUS_NOT_FOUND);
        if (const auto rc = sys::cap_revoke(devices[0].cap.selector(), true).status; rc != STATUS_OK)
            sys::exit(rc);
        for (;;) {
            const auto state = sys::io_space_state(created.value);
            if (state.status != STATUS_OK) sys::exit(state.status);
            if (state.value == IO_SPACE_CLOSED)
                break;
            if (state.value != IO_SPACE_ACTIVE && state.value != IO_SPACE_CLOSING)
                sys::exit(STATUS_BACKING_FAILED);
            sys::yield();
        }
        if (sys::vm_map(view->region.selector(), reg.value, 0x62000000, 4096, 0, VM_READ).status ==
            STATUS_OK)
            sys::exit(STATUS_BAD_RIGHTS);
    }
    auto renewed = pci->find(0x1042'1af4, 0);
    if (!renewed)
        sys::exit(renewed.error());
    devices[0] = std::move(*renewed);
    for (size_t i = 0; i < devices.size(); ++i) {
        blk_dev device;
        if (const auto rc =
                device.open(pool, vm, devices[i].cap.selector(), wake.selector(), rt_event_badge);
            rc != STATUS_OK)
            sys::exit(rc);
        if (const auto rc = device.close(); rc != STATUS_OK)
            sys::exit(rc);
    }
    port.write("[device-inventory] two requester grants, dynamic binding and cfg revocation\n");
    sys::exit();
}
