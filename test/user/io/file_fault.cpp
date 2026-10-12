#include <util.hpp>
#include <utility>
#include <ch.hpp>
#include <proto.hpp>
#include "file_fault.hpp"

io_client filesystem;
int main() {

    const auto& info = rt_startup();
    uint64_t value{};
    if (const auto rc = filesystem.connect(info.selector(fs_service), info.pool,
                                           info.cspace, info.events,
                                           info.vspace, 0x70000000, value);
        rc != STATUS_OK)
        sys::exit(rc);
    fs_file file;
    if (const auto rc = fs_open(filesystem, "DATA.BIN", 8, file); rc != STATUS_OK)
        sys::exit(rc);
    auto backing = fs_backing(filesystem, file);
    if (!backing)
        sys::exit(backing.error());
    auto vm_area = vm_area::map(info.vspace, std::move(backing->memory), 0x75000000,
                                4096, VM_READ);
    if (!vm_area)
        sys::exit(vm_area.error());
    if (const auto rc = fs_close(filesystem, file); rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = filesystem.close(); rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::notification_signal(info.selector(ft_ready)).status; rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::notification_wait(info.selector(ft_go)).status; rc != STATUS_OK)
        sys::exit(rc);
    const auto* bytes = reinterpret_cast<const volatile uint8_t*>(vm_area->address);
    for (size_t i = 0; i != 4096; ++i)
        if (bytes[i] != i % 251)
            sys::exit(STATUS_INTERNAL);
    if (const auto rc = vm_area->close(); rc != STATUS_OK)
        sys::exit(rc);
    sys::exit();
}
