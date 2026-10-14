#include <util.hpp>
#include <ipc.hpp>
#include <servers/fw.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>

 test_image program; test_tasks supervisor;

int main() {

    const auto& info = rt_startup();
    auto fw = fw_view::open(info, 0x41000000);
    if (!fw) sys::exit(fw.error());
    const auto storage = sys::resource_create_child(info.pool, 1024 * 1024, 64,
                          OBJ_BIT(ObjMem) | OBJ_BIT(ObjIrq) | OBJ_BIT(ObjIrqCtl));
    if (storage.status != STATUS_OK) sys::exit(storage.status);
    cap_owner owner{storage.value};
    auto child = info;
    child.pool = storage.value;
    auto irq = fw->irq(child);
    if (!irq) sys::exit(irq.error());
    auto console = fw_console::open(child, fw->tree, irq->selector());
    if (!console) sys::exit(console.error());
    const auto plic = fw->tree.find("sifive,plic-1.0.0");
    const auto range = plic ? fw->tree.reg(*plic) : std::nullopt;
    if (!range || sys::memory_create_physical(info.pool, fw_root(info).physical,
                         range->address, 4096, VM_READ).status != STATUS_BUSY)
        sys::exit(STATUS_INTERNAL);
    const auto copied = sys::cap_duplicate(console->memory.selector(), info.cspace, RIGHT_MAP);
    if (copied.status != STATUS_OK) sys::exit(copied.status);
    auto mapping = vm_area::map(info.vspace, cap_owner{copied.value}, 0, 4096, VM_READ | VM_WRITE);
    if (!mapping) sys::exit(mapping.error());
    uart_port port{mapping->address + console->offset};
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
    // Retire the actual controller and all its grants, including its private
    // kernel mapping, before admitting a new controller on the same range.
    if (const auto rc = mapping->close(); rc != STATUS_OK) sys::exit(rc);
    console = fw_console{};
    irq = cap_owner{};
    for (;;) {
        const auto rc = sys::resource_close(owner.selector()).status;
        if (rc == STATUS_OK) break;
        if (rc != STATUS_BUSY && rc != STATUS_RETRY) sys::exit(rc);
        sys::yield();
    }
    auto next_irq = fw->irq(info);
    if (!next_irq) sys::exit(next_irq.error());
    auto successor = fw_console::open(info, fw->tree, next_irq->selector());
    if (!successor) sys::exit(successor.error());
    const auto mem = sys::cap_duplicate(successor->memory.selector(), info.cspace, RIGHT_MAP);
    if (mem.status != STATUS_OK) sys::exit(mem.status);
    auto next = vm_area::map(info.vspace, cap_owner{mem.value}, 0, 4096, VM_READ | VM_WRITE);
    if (!next) sys::exit(next.error());
    uart_port{next->address + successor->offset}.write("[irq] controller retired and recreated\n");
    sys::exit();
}
