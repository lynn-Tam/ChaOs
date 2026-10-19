#include <libk/checked_arithmetic.hpp>
#include <test/user/task.hpp>
#include <util.hpp>
#include <exec.hpp>
#include <protocols/msg.hpp>
#include <protocols/file.hpp>
#include <protocols/service.hpp>
#include <protocols/stream.hpp>

int main() {

    const auto& info = rt_startup();
    rpc_client directory;
    if (const auto rc =
            directory.open(info.selector(sv_service), info.alloc());
        rc != STATUS_OK)
        sys::exit(rc);
    auto endpoint = sv_lookup(directory, vfs_exec);
    if (!endpoint)
        sys::exit(endpoint.error());
    rpc_client files;
    if (const auto rc = files.open(endpoint->selector(), info.alloc());
        rc != STATUS_OK)
        sys::exit(rc);
    auto note = fl_map(files, "/boot/README.TXT");
    if (!note || !note->size)
        sys::exit(STATUS_INTERNAL);
    auto text = vm_area::map(info.vspace, std::move(note->memory), 0,
                             libk::checked_align_up(note->size, 4096).value_or(0), VM_READ);
    if (!text)
        sys::exit(text.error());
    const auto* bytes = reinterpret_cast<volatile const uint8_t*>(text->address);
    for (size_t i = 0; i < note->size; ++i)
        (void) bytes[i];
    auto source = fl_map(files, "/boot/sleep.elf", VM_READ | VM_EXECUTE);
    if (!source)
        sys::exit(source.error());
    test_image image;
    if (const auto rc = image.open(info.vspace, source->memory.selector(), source->size);
        rc != STATUS_OK)
        sys::exit(rc);
    test_tasks tasks;
    tasks.open(info.alloc(), info.selector(rt_domain), info.cpus);
    const std::string_view args[]{"sleep", "10000"};
    const auto child = tasks.spawn(
        image, {},
        {.argv = args, .resources = {.bytes = 1024 * 1024, .caps = 64}, .events = info.events});
    if (child.status != STATUS_OK)
        sys::exit(child.status);
    str_writer console{info.selector(con_output)};
    console.write("[fault-shell] file read before failure\n");
    // This test ELF takes a real user fault. Production services contain no
    // fault trigger or scenario branch.
    (void) *reinterpret_cast<volatile const uint64_t*>(0xe100);
    sys::exit(STATUS_INTERNAL);
}
