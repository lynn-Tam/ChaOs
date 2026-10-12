#include <util.hpp>
#include <ch.hpp>
#include <servers/fw.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>

 pr_pkg program; pr_tasks supervisor;

int main() {

    const auto& info = rt_startup();
    auto fw = fw_view::open(info, 0x41000000);
    if (!fw) sys::exit(fw.error());
    auto vm_area = vm_area::map(info.vspace,
        cap_owner{fw->console(ObjMem)},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!vm_area) sys::exit(vm_area.error());
    uart_port port{vm_area->address};
    port.reset();
    supervisor.open(info.pool, fw_root(info).domain, info.vspace, info.cpus);
    open(program, info);
    const pr_cap caps[]{
        {.contract = rt_domain, .cap = fw_root(info).domain, .limit = word_t{RIGHT_CONTROL | RIGHT_DUPLICATE}},
        {.contract = rt_image, .bytes = fw_root(info).bundle_bytes, .cap = fw_root(info).bundle, .limit = word_t{RIGHT_MAP | RIGHT_DELEGATE | RIGHT_DUPLICATE | RIGHT_INSPECT}}};
    const auto task = start(supervisor, program, "channel-test", {.caps = caps,
        .resources = {.bytes = 32 * 1024 * 1024, .caps = 2048,
            .kinds = pr_base_kinds | OBJ_BIT(ObjChannel), .slots = 512, .pages = 68,
            .pool_perms = RIGHT_CREATE | RIGHT_SPLIT}});
    const auto status = wait(supervisor, task);
    if (status != STATUS_OK) {
        (void)libk::fmt::format_to<"[channel] failed status={}\n">(port, status);
        sys::exit(status);
    }
    port.write("[channel] service generations, stale cap denied, blocked senders, stop and reuse ok\n");
    sys::exit();
}
