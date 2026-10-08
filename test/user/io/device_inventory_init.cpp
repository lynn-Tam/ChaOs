#include <sys/pci.hpp>
#include <servers/block/device.hpp>
#include <servers/uart/port.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    auto pci = sys::pci::Bus::open(info, 0x3100'0000);
    if (!pci) exit(pci.error());
    service::require(pci->configure(0x1042'1af4));
    const auto vm = service::capability(info, BOOT_VSPACE);
    auto mapping = MappedMemory::map(vm,
        cap::OwnedCap{{service::capability(info, boot::UartMem), 0}},
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
    const CapView outside{CAP_ATTENUATION_VERSION_CURRENT, OBJECT_KIND_IO_HOST,
        CAP_ATTENUATION_SIZE, RIGHT_CONNECT, {devices[1].rid, 1}};
    cap::encode(outside, *reinterpret_cast<uint8_t (*)[CAP_ATTENUATION_SIZE]>(pci->desc.address));
    if (cap_typed_delegate(devices[0].cap.selector(), 0, pci->desc.memory.selector(), 0).status != STATUS_DENIED)
        exit(STATUS_INTERNAL);
    const auto pool = service::capability(info, BOOT_POOL);
    {
        const auto* firmware = info.find(sys::pci::Firmware);
        if (!firmware || firmware->bytes < 40) exit(STATUS_BAD_ARGS);
        const auto copy = cap_duplicate(firmware->handle, info.selector(BOOT_CSPACE), RIGHT_MAP);
        service::require(copy.status);
        const size_t offset = firmware->phys % 4096;
        const size_t bytes = (offset + firmware->bytes + 4095) & ~size_t{4095};
        auto view = MappedMemory::map(vm, cap::OwnedCap{{copy.value, 0}},
            0x32000000, bytes, VM_READ);
        if (!view) exit(view.error());
        const auto* header = reinterpret_cast<volatile const uint8_t*>(view->address + offset);
        if (header[0] != 0xd0 || header[1] != 0x0d || header[2] != 0xfe || header[3] != 0xed)
            exit(STATUS_BAD_ARGS);
    }
    const auto events = notification_create(pool, service::EventsBadge);
    service::require(events.status);
    cap::OwnedCap wake{{events.value, 0}};
    {
        const auto mem = memory_create(pool, 4096, VM_READ | VM_WRITE);
        const auto created = io_space_create(pool);
        service::require(mem.status);
        service::require(created.status);
        cap::OwnedCap arena{{mem.value, 0}}, space{{created.value, 0}};
        if (io_space_bind(created.value, info.selector(sys::pci::Host), mem.value, 0, 1, 0x100000).status != STATUS_BAD_RIGHTS)
            exit(STATUS_INTERNAL);
        service::require(io_space_bind(created.value, devices[0].cap.selector(), mem.value, 0, 1, 0x100000).status);
        for (;;) {
            const auto state = io_space_state(created.value);
            service::require(state.status);
            if (state.value == IO_SPACE_ACTIVE) break;
            if (state.value != IO_SPACE_OPENING) exit(STATUS_BACKING_FAILED);
            yield();
        }
        const auto reg = io_space_reg(created.value, IO_PCI_CFG);
        service::require(reg.status);
        cap::OwnedCap cfg{{reg.value, 0}};
        const auto slice = vm_slice(vm, 0x62001000, 4096, VM_READ | VM_WRITE,
            RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
        service::require(slice.status);
        cap::OwnedCap writable{{slice.value, 0}};
        if (reg.value2 != 4096 ||
            vm_map(slice.value, reg.value, 0x62001000, 4096, 0, VM_READ | VM_WRITE).status != STATUS_BAD_RIGHTS)
            exit(STATUS_BAD_RIGHTS);
        service::require(vm_clear(slice.value).status);
        auto view = MappedMemory::map(vm, std::move(cfg), 0x62000000, 4096, VM_READ);
        if (!view) exit(view.error());
        if (*reinterpret_cast<volatile const uint32_t*>(view->address) != 0x1042'1af4)
            exit(STATUS_NOT_FOUND);
        service::require(cap_revoke(devices[0].cap.selector(), true).status);
        for (;;) {
            const auto state = io_space_state(created.value);
            service::require(state.status);
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
        service::require(device.open(pool, vm, devices[i].cap.selector(), wake.selector()));
        service::require(device.close());
    }
    port.write("[device-inventory] two requester grants, dynamic binding and cfg revocation\n");
    exit();
}
