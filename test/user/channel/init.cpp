#include <util.hpp>
#include <ipc.hpp>
#include <servers/fw.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <protocols/msg.hpp>

 test_image program; test_tasks supervisor;

int main() {

    const auto& info = rt_startup();
    auto fw = fw_view::open(info, 0x41000000);
    if (!fw) sys::exit(fw.error());
    const auto storage = sys::account_create(info.alloc(), 1024 * 1024, 64,
                          OBJ_BIT(ObjMem) | OBJ_BIT(ObjIrq) | OBJ_BIT(ObjIrqCtl));
    if (storage.status != STATUS_OK) sys::exit(storage.status);
    cap_owner owner{storage.value};
    const auto scope = sys::scope_create(info.alloc());
    if (scope.status != STATUS_OK) sys::exit(scope.status);
    cap_owner lifetime{scope.value};
    auto child = info;
    child.pool = storage.value;
    child.scope = scope.value;
    auto irq = fw->irq(child);
    if (!irq) sys::exit(irq.error());
    auto console = fw_console::open(child, fw->tree, irq->selector());
    if (!console) sys::exit(console.error());
    const auto plic = fw->tree.find("sifive,plic-1.0.0");
    const auto range = plic ? fw->tree.reg(*plic) : std::nullopt;
    if (!range || sys::memory_create_physical(info.alloc(), fw_root(info).physical,
                         range->address, 4096, VM_READ).status != STATUS_BUSY)
        sys::exit(STATUS_INTERNAL);
    const auto copied = sys::cap_duplicate(console->memory.selector(), info.cspace, RIGHT_MAP);
    if (copied.status != STATUS_OK) sys::exit(copied.status);
    auto mapping = vm_area::map(info.vspace, cap_owner{copied.value}, 0, 4096, VM_READ | VM_WRITE);
    if (!mapping) sys::exit(mapping.error());
    uart_port port{mapping->address + console->offset};
    port.reset();
    supervisor.open(info.alloc(), fw_root(info).domain, info.cpus);
    open(program, info);
    ch_pair metrics;
    if (const auto rc = metrics.open(info.alloc(), 3, 0); rc != STATUS_OK) sys::exit(rc);
    const pr_cap caps[]{
        input(ct_perf, metrics.ends[0].selector(), RIGHT_SEND, 1),
        {.contract = rt_domain, .cap = fw_root(info).domain, .limit = word_t{RIGHT_CREATE | RIGHT_DUPLICATE}},
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
    msg_frame result;
    if (const auto rc = msg_recv(metrics.ends[1].selector(), result, false).status; rc != STATUS_OK) sys::exit(rc);
    std::array<word_t, 2> percentiles;
    if (result.size != sizeof(percentiles)) sys::exit(STATUS_BAD_ARGS);
    ::memcpy(percentiles.data(), result.data, sizeof(percentiles));
    (void)libk::fmt::format_to<"[channel] payload pressure: streams=8 messages=8192 ticks={} batch_p50_max={} batch_p99_max={}\n">(
        port, result.value, percentiles[0], percentiles[1]);
    if (const auto rc = msg_recv(metrics.ends[1].selector(), result, false).status; rc != STATUS_OK) sys::exit(rc);
    if (result.operation != 1 || result.size != sizeof(word_t)) sys::exit(STATUS_BAD_ARGS);
    word_t contended_p99; ::memcpy(&contended_p99, result.data, sizeof(contended_p99));
    (void)libk::fmt::format_to<"[channel] active consumers=9 messages=128 ack_p50={} ack_p99={}\n">(
        port, result.value, contended_p99);
    if (const auto rc = msg_recv(metrics.ends[1].selector(), result, false).status; rc != STATUS_OK) sys::exit(rc);
    if (result.operation != 2 || result.size != sizeof(word_t)) sys::exit(STATUS_BAD_ARGS);
    word_t rpc_p99; ::memcpy(&rpc_p99, result.data, sizeof(rpc_p99));
    (void)libk::fmt::format_to<"[channel] RPC calls=128 p50={} p99={}\n">(port, result.value, rpc_p99);
    port.write("[channel] service generations, stale cap denied, blocked senders, stop and reuse ok\n");
    // Retire the actual controller and all its grants, including its private
    // kernel mapping, before admitting a new controller on the same range.
    if (const auto rc = mapping->close(); rc != STATUS_OK) sys::exit(rc);
    console = fw_console{};
    irq = cap_owner{};
    if (const auto rc = sys::scope_close(lifetime.selector()).status; rc != STATUS_OK) sys::exit(rc);
    if (const auto rc = lifetime.close(); rc != STATUS_OK) sys::exit(rc);
    if (const auto rc = sys::account_close(owner.selector()).status; rc != STATUS_OK) sys::exit(rc);
    if (const auto rc = sys::wait_one(owner.selector(), STATE_DRAINED).status; rc != STATUS_OK) sys::exit(rc);
    if (const auto rc = sys::object_destroy(owner.selector()).status; rc != STATUS_OK) sys::exit(rc);
    if (const auto rc = owner.close(); rc != STATUS_OK) sys::exit(rc);
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
