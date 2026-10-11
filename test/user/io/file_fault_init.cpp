#include <sys/protocol.hpp>
#include <sys/pci.hpp>
#include <utility>
#include <sys/channel.hpp>
#include <sys/handle.hpp>
#include <test/user/task.hpp>
#include <servers/uart/port.hpp>
#include "file_fault.hpp"

namespace {
using namespace sys;
proc::package program;
using Supervisor = proc::tasks<4>;
Supervisor supervisor;
void check(bool condition) noexcept { if (!condition) exit(STATUS_INTERNAL); }
auto input(uart::Port port) -> uint8_t {
    uint8_t value{};
    // Fixture-only host barrier; ordinary services keep their normal IRQ path.
    while (!port.try_get(value)) yield();
    return value;
}
}
extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    const auto info = sys::bootstrap(address, size);
    auto pci = sys::pci::Bus::open(info, 0x3100'0000);
    if (!pci) exit(pci.error());
    sys::require(pci->configure(0x1042'1af4));
    auto selected = pci->find(0x1042'1af4, 0);
    if (!selected) sys::exit(selected.error());
    auto mapping = mapping::map(sys::capability(info, BOOT_VSPACE),
        cap_owner{pci->firmware.console(ObjMem)},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address}; port.reset();
    uart::Printer printer{uart::Writer{port}};
    supervisor.open(info);
    open(program, info);
    const auto pool = sys::capability(info, BOOT_POOL);
    cap_owner endpoints[4];
    for (size_t i = 0; i != 2; ++i) {
        const auto pair = channel_create(pool, i == 0 ? 1 : 8, CHANNEL_MAX_WORDS, 4, 2);
        sys::require(pair.status);
        endpoints[2 * i] = cap_owner{pair.value};
        endpoints[2 * i + 1] = cap_owner{pair.value2};
    }
    const proc::cap_src block_caps[]{host(selected->cap.selector(), selected->rid),
        input(boot::Block, endpoints[1].selector(), RIGHT_RECEIVE, 1)};
    const proc::cap_src files_caps[]{input(boot::Block, endpoints[0].selector(), RIGHT_SEND, 1),
        input(boot::Files, endpoints[3].selector(), RIGHT_RECEIVE, 1)};
    proc::task_id tasks[4];
    for (unsigned i = 0; i != 2; ++i) {
        tasks[i] = start(supervisor, program, i == 0 ? "block" : "files", {
            .caps = i == 0 ? std::span<const proc::cap_src>{block_caps} : std::span<const proc::cap_src>{files_caps},
            .resources = i == 0 ? io_quota : files_quota});
    }
    const auto ready = notification_create(pool, 1);
    sys::require(ready.status);
    cap_owner go[2];
    for (unsigned i = 0; i != 2; ++i) {
        const auto signal = notification_create(pool, 1);
        sys::require(signal.status); go[i] = cap_owner{signal.value};
        const proc::cap_src caps[]{input(boot::Files, endpoints[2].selector(), RIGHT_SEND, 1),
            input(file_fault_test::Ready, ready.value, RIGHT_SIGNAL),
            input(file_fault_test::Go, signal.value, RIGHT_RECEIVE)};
        tasks[2 + i] = start(supervisor, program, "file-client", {.caps = caps, .resources = io_quota});
        sys::require(notification_wait(ready.value).status);
    }
    port.write("[file-fault] mapped, awaiting host\n");
    check(input(port) == 'g');
    for (auto& signal : go) sys::require(notification_signal(signal.selector()).status);
    const auto mode = input(port);
    check(mode == 'r' || mode == 'k');
    if (mode == 'k') {
        port.write("[file-fault] stopping Files with page-in pending\n");
        const auto status = stop(supervisor, tasks[1]);
        (void)printer.print<"[file-fault] Files stop status={}\n">(status);
        check(status == STATUS_CANCELED);
    }
    for (unsigned i = 2; i != 4; ++i) {
        const auto status = wait(supervisor, tasks[i]);
        (void)printer.print<"[file-fault] client {} status={}\n">(i, status);
        check(status == (mode == 'k' ? STATUS_PEER_FAULT : STATUS_OK));
    }
    const auto block = supervisor.observe(tasks[0]);
    (void)printer.print<"[file-fault] Block observe status={} value={} result={}\n">(block.status, block.value, block.value2);
    check(block.status == STATUS_OK && block.value == 0);
    if (mode == 'r') check(stop(supervisor, tasks[1]) == STATUS_CANCELED);
    const auto block_stop = stop(supervisor, tasks[0]);
    (void)printer.print<"[file-fault] Block stop status={}\n">(block_stop);
    check(block_stop == STATUS_CANCELED);
    port.write(mode == 'k' ? "[file-fault] Files death released both faults, Block survived\n"
                           : "[file-fault] shared request supplied both mappings\n");
    exit();
}
