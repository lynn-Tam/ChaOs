#include <util.hpp>
#include <proc.hpp>
#include <ch.hpp>
#include <proto.hpp>

int main() {

    const auto& info = rt_startup();
    io_client files;
    uint64_t value{};
    if (const auto rc = files.connect(info.selector(fs_service), info.pool,
                                      info.cspace, info.events,
                                      info.vspace, 0x70000000, value);
        rc != STATUS_OK)
        sys::exit(rc);
    fs_file file{};
    constexpr char path[] = "README.TXT";
    if (const auto rc = fs_open(files, path, sizeof(path) - 1, file); rc != STATUS_OK)
        sys::exit(rc);
    size_t read{};
    if (const auto rc = fs_read(
            files, file, [&](uint64_t, const uint8_t*, size_t bytes) noexcept { read += bytes; });
        rc != STATUS_OK)
        sys::exit(rc);
    if (read != file.size || read == 0)
        sys::exit(STATUS_INTERNAL);
    if (const auto rc = fs_close(files, file); rc != STATUS_OK)
        sys::exit(rc);
    // Leave a real descendant running when this launcher faults. The external
    // parent must retire the entire group; this process cannot run destructors.
    if (const auto rc = fs_open(files, "SLEEP.PKG", 9, file); rc != STATUS_OK)
        sys::exit(rc);
    auto image = fs_backing(files, file, VM_READ | VM_EXECUTE);
    if (!image)
        sys::exit(image.error());
    if (const auto rc = fs_close(files, file); rc != STATUS_OK)
        sys::exit(rc);
    pr_pkg pkg;
    if (const auto rc = pkg.open(info.vspace, image->memory.selector(),
                                 {0, pr_window::round_size(image->size)}, image->size,
                                 VM_READ | VM_EXECUTE | VM_MAP_PRIVATE);
        rc != STATUS_OK)
        sys::exit(rc);
    pr_tasks tasks;
    tasks.open(info.pool, info.selector(rt_domain), info.vspace, info.cpus);
    const std::string_view args[]{"sleep", "10000"};
    const auto child = tasks.spawn(pkg, {},
                                   {.argv = args,
                                    .resources = {.bytes = 1024 * 1024, .caps = 64},
                                    .events = info.events});
    if (child.status != STATUS_OK)
        sys::exit(child.status);
    if (const auto rc = files.close(); rc != STATUS_OK)
        sys::exit(rc);
    str_writer console{info.selector(con_output)};
    console.write("[fault-shell] file read before failure\n");
    // This test ELF takes a real user fault. Production services contain no
    // fault trigger or scenario branch.
    (void) *reinterpret_cast<volatile const uint64_t*>(0xe100);
    sys::exit(STATUS_INTERNAL);
}
