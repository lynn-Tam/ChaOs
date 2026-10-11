#include <sys/pci.hpp>
#include <servers/block/device.hpp>
#include <servers/uart/port.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    auto pci = sys::pci::Bus::open(info, 0x3100'0000);
    if (!pci) exit(pci.error());
    sys::require(pci->configure(0x1042'1af4));
    const auto vm = sys::capability(info, BOOT_VSPACE);
    auto mapping = mapping::map(vm,
        cap_owner{pci->firmware.console(ObjMem)},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    if (pci->count(0x1042'1af4) != 2) exit(STATUS_NOT_FOUND);
    std::array<sys::pci::Binding, 2> devices;
    for (size_t i = 0; i < devices.size(); ++i) {
        auto found = pci->find(0x1042'1af4, i);
        if (!found) exit(found.error());
        devices[i] = std::move(*found);
    }
    if (devices[0].rid == devices[1].rid) exit(STATUS_BAD_ARGS);
    const CapView outside{CAP_ATTENUATION_VERSION_CURRENT, ObjHost,
        CAP_ATTENUATION_SIZE, RIGHT_CONNECT, {devices[1].rid, 1}};
    encode_cap(outside, *reinterpret_cast<uint8_t (*)[CAP_ATTENUATION_SIZE]>(pci->desc.address));
    if (cap_typed_delegate(devices[0].cap.selector(), 0, pci->desc.memory.selector(), 0).status != STATUS_DENIED)
        exit(STATUS_INTERNAL);
    const auto pool = sys::capability(info, BOOT_POOL);
    {
        const auto* firmware = info.record(BOOT_FIRMWARE);
        if (!firmware || firmware->bytes < 40) exit(STATUS_BAD_ARGS);
        const auto copy = cap_duplicate(firmware->handle, info.selector(BOOT_CSPACE), RIGHT_MAP);
        sys::require(copy.status);
        const size_t offset = firmware->phys % 4096;
        const size_t bytes = (offset + firmware->bytes + 4095) & ~size_t{4095};
        auto view = mapping::map(vm, cap_owner{copy.value},
            0x32000000, bytes, VM_READ);
        if (!view) exit(view.error());
        const auto* header = reinterpret_cast<volatile const uint8_t*>(view->address + offset);
        if (header[0] != 0xd0 || header[1] != 0x0d || header[2] != 0xfe || header[3] != 0xed)
            exit(STATUS_BAD_ARGS);
    }
    const auto events = notification_create(pool, boot::event_badge);
    sys::require(events.status);
    cap_owner wake{events.value};
    {
        const auto mem = memory_create(pool, 4096, VM_READ | VM_WRITE);
        const auto created = sys::io_space_create(pool);
        sys::require(mem.status);
        sys::require(created.status);
        cap_owner arena{mem.value}, space{created.value};
        if (sys::io_space_bind(created.value, pci->host, mem.value, 0, 1, 0x100000).status != STATUS_BAD_RIGHTS)
            exit(STATUS_INTERNAL);
        sys::require(sys::io_space_bind(created.value, devices[0].cap.selector(), mem.value, 0, 1, 0x100000).status);
        for (;;) {
            const auto state = sys::io_space_state(created.value);
            sys::require(state.status);
            if (state.value == IO_SPACE_ACTIVE) break;
            if (state.value != IO_SPACE_OPENING) exit(STATUS_BACKING_FAILED);
            yield();
        }
        const auto reg = sys::io_space_reg(created.value, IO_PCI_CFG);
        sys::require(reg.status);
        cap_owner cfg{reg.value};
        const auto slice = vm_slice(vm, 0x62001000, 4096, VM_READ | VM_WRITE,
            RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
        sys::require(slice.status);
        cap_owner writable{slice.value};
        if (reg.value2 != 4096 ||
            vm_map(slice.value, reg.value, 0x62001000, 4096, 0, VM_READ | VM_WRITE).status != STATUS_BAD_RIGHTS)
            exit(STATUS_BAD_RIGHTS);
        sys::require(vm_clear(slice.value).status);
        auto view = mapping::map(vm, std::move(cfg), 0x62000000, 4096, VM_READ);
        if (!view) exit(view.error());
        if (*reinterpret_cast<volatile const uint32_t*>(view->address) != 0x1042'1af4)
            exit(STATUS_NOT_FOUND);
        sys::require(cap_revoke(devices[0].cap.selector(), true).status);
        for (;;) {
            const auto state = sys::io_space_state(created.value);
            sys::require(state.status);
            if (state.value == IO_SPACE_CLOSED) break;
            if (state.value != IO_SPACE_ACTIVE && state.value != IO_SPACE_CLOSING)
                exit(STATUS_BACKING_FAILED);
            yield();
        }
        if (vm_map(view->region.selector(), reg.value, 0x62000000, 4096, 0, VM_READ).status == STATUS_OK)
            exit(STATUS_BAD_RIGHTS);
    }
    auto renewed = pci->find(0x1042'1af4, 0);
    if (!renewed) exit(renewed.error());
    devices[0] = std::move(*renewed);
    for (size_t i = 0; i < devices.size(); ++i) {
        block::Device device;
        sys::require(device.open(pool, vm, devices[i].cap.selector(), wake.selector(), boot::event_badge));
        sys::require(device.close());
    }
    port.write("[device-inventory] two requester grants, dynamic binding and cfg revocation\n");
    exit();
}
