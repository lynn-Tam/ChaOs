#include <util.hpp>
#include <utility>
#include <ch.hpp>
#include <proto.hpp>

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
    // The fixture injects EIO at the file's first data sector, after mounting.
    // Ordinary reads and page-in traverse the same real block service.
    const auto read = fs_read(filesystem, file, [](uint64_t, const uint8_t*, size_t) {});
    if (read != STATUS_BACKING_FAILED)
        sys::exit(STATUS_INTERNAL);
    auto backing = fs_backing(filesystem, file);
    if (!backing)
        sys::exit(backing.error());
    auto vm_area = vm_area::map(info.vspace, std::move(backing->memory), 0x75000000,
                                (file.size + 4095) & ~size_t{4095}, VM_READ);
    if (!vm_area)
        sys::exit(vm_area.error());
    if (const auto rc = fs_close(filesystem, file); rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = filesystem.close(); rc != STATUS_OK)
        sys::exit(rc);
    // Closing both handles must preserve the independent content authority.
    // Its first fault must report the backend failure, never fabricate zeros.
    const auto byte = *reinterpret_cast<const volatile uint8_t*>(vm_area->address);
    (void) byte;
    sys::exit(STATUS_INTERNAL);
}
