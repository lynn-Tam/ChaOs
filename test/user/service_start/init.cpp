#include <sys/pci.hpp>
#include <servers/runtime/service.hpp>
#include <servers/deploy/services.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/uart/port.hpp>

namespace {
deploy::program program;
deploy::tasks<5, 24> supervisor;

void report(const boot::BootView& info, const char* message,
    bool initialize = false) noexcept {
    using namespace sys;
    constexpr uintptr_t address = 0x30010000;
    const auto vspace = service::capability(info, BOOT_VSPACE);
    const auto region = vm_slice(vspace, address, 4096,
        VM_READ | VM_WRITE,
        RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
    service::require(region.status);
    service::require(vm_map(region.value,
        service::capability(info, boot::UartMem),
        address, 4096, 0, VM_READ | VM_WRITE).status);
    uart::Port port{address};
    if (initialize) port.reset();
    port.write(message);
    service::require(vm_unmap(region.value, address, 4096).status);
    service::require(vm_clear(region.value).status);
    service::require(cap_close(region.value).status);
}
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    auto pci = sys::pci::Bus::open(info, 0x3100'0000);
    if (!pci) exit(pci.error());
    service::require(pci->configure(0x1042'1af4));
    auto selected = pci->find(0x1042'1af4, 0);
    if (!selected) sys::exit(selected.error());
    report(info, "[service-start] fixture\n", true);
    service::require(supervisor.load(program, info));
    supervisor.open(info);
    service::require(supervisor.add_boot_sources(info));
    service::require(supervisor.add("uart.memory",
        service::capability(info, boot::UartMem),
        OBJECT_KIND_MEMORY, RIGHT_MAP, 0, 1,
        VM_READ | VM_WRITE, 0));
    service::require(supervisor.add("uart.irq",
        service::capability(info, boot::UartIrq), OBJECT_KIND_IRQ,
        RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK));
    const auto pool = service::capability(info, BOOT_POOL);
    const auto notification = notification_create(pool, 1);
    service::require(notification.status);
    cap::OwnedCap events{{notification.value, 0}};
    const auto pair = channel_create(pool, 4, CHANNEL_MAX_WORDS, 1, 1);
    service::require(pair.status);
    cap::OwnedCap control_root{{pair.value, 0}}, control_client{{pair.value2, 0}};
    service::require(supervisor.add("service.control", control_client.selector(),
        OBJECT_KIND_CHANNEL, RIGHT_SEND | RIGHT_RECEIVE | RIGHT_DUPLICATE, 1));
    const auto block_device = selected->cap.selector();
    service::require(supervisor.add("block.host", block_device,
        OBJECT_KIND_IO_HOST, RIGHT_CONNECT | RIGHT_DUPLICATE, selected->rid, 1));

    deploy::services<5, 24> services{supervisor, program, events.selector()};
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        const auto started = services.start();
        if (started.status != STATUS_NO_MEMORY) exit(started.status == STATUS_OK
            ? STATUS_INTERNAL : started.status);
        report(info, attempt == 0 ? "[service-start] first rollback complete\n"
            : "[service-start] second rollback complete\n");
    }
    exit();
}
