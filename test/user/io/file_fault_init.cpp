#include <sys/pci.hpp>
#include <utility>
#include <servers/runtime/service.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/uart/port.hpp>
#include "file_fault.hpp"

namespace {
using namespace sys;
deploy::program program;
using Supervisor = deploy::tasks<4>;
Supervisor supervisor;
void check(bool condition) noexcept { if (!condition) exit(STATUS_INTERNAL); }
auto event_source(const char* name, cap_t cap, word_t rights) -> deploy::source {
    return {name, {cap, 0}, {.version = CAP_ATTENUATION_VERSION_CURRENT,
        .kind = OBJECT_KIND_NOTIFICATION, .size = CAP_ATTENUATION_SIZE,
        .rights = rights, .words = {}}};
}
auto input(uart::Port port) -> uint8_t {
    uint8_t value{};
    // Fixture-only host barrier; ordinary services keep their normal IRQ path.
    while (!port.try_get(value)) yield();
    return value;
}
}
extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    const auto info = service::bootstrap(address, size);
    auto pci = sys::pci::Bus::open(info, 0x3100'0000);
    if (!pci) exit(pci.error());
    service::require(pci->configure(0x1042'1af4));
    auto selected = pci->find(0x1042'1af4, 0);
    if (!selected) sys::exit(selected.error());
    auto mapping = MappedMemory::map(service::capability(info, BOOT_VSPACE),
        cap::OwnedCap{{service::capability(info, boot::UartMem), 0}},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address}; port.reset();
    uart::Printer printer{uart::Writer{port}};
    supervisor.open(info);
    service::require(supervisor.load(program, info));
    service::require(supervisor.add_boot_sources(info));
    service::require(supervisor.add("block.host", selected->cap.selector(),
        OBJECT_KIND_IO_HOST, RIGHT_DUPLICATE | RIGHT_CONNECT, selected->rid, 1));
    const auto pool = service::capability(info, BOOT_POOL);
    constexpr const char* names[][2] = {{"block.client", "block.server"}, {"files.client", "files.server"}};
    cap::OwnedCap endpoints[4];
    for (size_t i = 0; i != 2; ++i) {
        const auto pair = channel_create(pool, i == 0 ? 1 : 8, CHANNEL_MAX_WORDS, 4, 2);
        service::require(pair.status);
        endpoints[2 * i] = cap::OwnedCap{{pair.value, 0}};
        endpoints[2 * i + 1] = cap::OwnedCap{{pair.value2, 0}};
        const auto rights = RIGHT_SEND | RIGHT_RECEIVE | RIGHT_DUPLICATE;
        service::require(supervisor.add(names[i][0], pair.value, OBJECT_KIND_CHANNEL, rights, 0));
        service::require(supervisor.add(names[i][1], pair.value2, OBJECT_KIND_CHANNEL, rights, 1));
    }
    Supervisor::handle tasks[4];
    for (unsigned i = 0; i != 2; ++i) {
        status_t status{};
        auto task = supervisor.launch(program, i == 0 ? "block" : "files", status);
        if (!task) {
            (void)printer.print<"[file-fault] service {} launch failed status={}\n">(i, status);
            exit(status);
        }
        tasks[i] = std::move(*task);
    }
    const auto ready = notification_create(pool, 1);
    service::require(ready.status);
    cap::OwnedCap go[2];
    for (unsigned i = 0; i != 2; ++i) {
        const auto signal = notification_create(pool, 1);
        service::require(signal.status); go[i] = cap::OwnedCap{{signal.value, 0}};
        const deploy::source sources[] = {
            event_source("test.ready", ready.value, RIGHT_SIGNAL),
            event_source("test.go", signal.value, RIGHT_RECEIVE)};
        status_t status{};
        auto task = supervisor.launch(program, Supervisor::name("file-client"), status, {.sources = sources});
        if (!task) {
            (void)printer.print<"[file-fault] client {} launch failed status={}\n">(i, status);
            exit(status);
        }
        tasks[2 + i] = std::move(*task);
        service::require(notification_wait(ready.value).status);
    }
    port.write("[file-fault] mapped, awaiting host\n");
    check(input(port) == 'g');
    for (auto& signal : go) service::require(notification_signal(signal.selector()).status);
    const auto mode = input(port);
    check(mode == 'r' || mode == 'k');
    if (mode == 'k') {
        port.write("[file-fault] stopping Files with page-in pending\n");
        const auto status = supervisor.stop(tasks[1]);
        (void)printer.print<"[file-fault] Files stop status={}\n">(status);
        check(status == STATUS_CANCELED);
    }
    for (unsigned i = 2; i != 4; ++i) {
        const auto status = supervisor.wait(tasks[i]);
        (void)printer.print<"[file-fault] client {} status={}\n">(i, status);
        check(status == (mode == 'k' ? STATUS_PEER_FAULT : STATUS_OK));
    }
    const auto block = supervisor.observe(tasks[0]);
    (void)printer.print<"[file-fault] Block observe status={} value={} result={}\n">(block.status, block.value, block.value2);
    check(block.status == STATUS_OK && block.value == 0);
    if (mode == 'r') check(supervisor.stop(tasks[1]) == STATUS_CANCELED);
    const auto block_stop = supervisor.stop(tasks[0]);
    (void)printer.print<"[file-fault] Block stop status={}\n">(block_stop);
    check(block_stop == STATUS_CANCELED);
    port.write(mode == 'k' ? "[file-fault] Files death released both faults, Block survived\n"
                           : "[file-fault] shared request supplied both mappings\n");
    exit();
}
