#include <sys/channel.hpp>
#include <sys/fw.hpp>
#include <test/user/task.hpp>
#include <servers/uart/port.hpp>

namespace { proc::package program; proc::tasks<1> supervisor; }

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    auto fw = Firmware::open(info, 0x41000000);
    if (!fw) exit(fw.error());
    auto mapping = mapping::map(sys::capability(info, BOOT_VSPACE),
        cap_owner{fw->console(ObjMem)},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    supervisor.open(info);
    open(program, info);
    const proc::cap_src caps[]{
        {.role = BOOT_DOMAIN, .cap = info.selector(BOOT_DOMAIN), .limit = word_t{RIGHT_CONTROL | RIGHT_DUPLICATE}},
        {.role = BOOT_BUNDLE, .cap = info.selector(BOOT_BUNDLE), .limit = word_t{RIGHT_MAP | RIGHT_DELEGATE | RIGHT_DUPLICATE | RIGHT_INSPECT}}};
    const auto task = start(supervisor, program, "channel-test", {.caps = caps,
        .resources = {.bytes = 32 * 1024 * 1024, .caps = 2048,
            .kinds = proc::base_kinds | OBJ_BIT(ObjChannel), .slots = 512, .pages = 68,
            .pool_perms = RIGHT_CREATE | RIGHT_SPLIT}});
    const auto status = wait(supervisor, task);
    if (status != STATUS_OK) {
        uart::Printer printer{uart::Writer{port}};
        (void)printer.print<"[channel] failed status={}\n">(status);
        exit(status);
    }
    port.write("[channel] service generations, stale cap denied, blocked senders, stop and reuse ok\n");
    exit();
}
