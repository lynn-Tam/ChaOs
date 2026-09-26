#include <user/lib/service_supervisor.hpp>
#include <user/lib/supervisor.hpp>
#include <user/lib/uart.hpp>

namespace {
myos::deploy::Program program;
myos::deploy::Supervisor<5, 24> supervisor;

void report(const myos::bootstrap::BootstrapView& info, const char* message,
    bool initialize = false) noexcept {
    using namespace myos;
    constexpr uintptr_t address = 0x30010000;
    const auto vspace = service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    const auto region = vm_create_region(vspace, address, 4096,
        MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_DEVICE,
        MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
    service::require(region.status);
    service::require(vm_map(region.value,
        service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY),
        address, 4096, 0, MYOS_VM_READ | MYOS_VM_WRITE).status);
    uart::Port port{address};
    if (initialize) port.reset();
    port.write(message);
    service::require(vm_complete(vspace, vm_unmap(region.value, address, 4096)).status);
    service::require(vm_complete(vspace, vm_destroy_region(region.value)).status);
    service::require(cap_close(region.value).status);
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    report(info, "[service-start] fixture\n", true);
    service::require(supervisor.load(program, info));
    supervisor.open(info);
    service::require(supervisor.add_boot_sources(info));
    service::require(supervisor.add("uart.memory",
        service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY),
        MYOS_OBJECT_KIND_MEMORY, MYOS_RIGHT_MAP, 0, 1,
        MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_DEVICE));
    service::require(supervisor.add("uart.irq",
        service::capability(info, MYOS_BOOTSTRAP_CAP_IRQ), MYOS_OBJECT_KIND_IRQ,
        MYOS_RIGHT_ROUTE | MYOS_RIGHT_OBSERVE | MYOS_RIGHT_ACK));
    const auto pool = service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const auto notification = notification_create(pool, 1);
    service::require(notification.status);
    cap::OwnedCap events{{notification.value, 0}};
    const auto pair = channel_create(pool, 4, MYOS_CHANNEL_MAX_WORDS, 1, 1);
    service::require(pair.status);
    cap::OwnedCap control_root{{pair.value, 0}}, control_client{{pair.value2, 0}};
    service::require(supervisor.add("service.control", control_client.selector(),
        MYOS_OBJECT_KIND_CHANNEL, MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_DUPLICATE, 1));
    const auto block_device = service::initial_device(info);
    service::require(device_info(block_device).status);
    service::require(supervisor.add("pci.0008", block_device,
        MYOS_OBJECT_KIND_DEVICE, MYOS_RIGHT_CONNECT | MYOS_RIGHT_DUPLICATE));

    deploy::ServiceSupervisor<5, 24> services{supervisor, program, events.selector()};
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        const auto started = services.start();
        if (started.status != MYOS_STATUS_NO_MEMORY) exit(started.status == MYOS_STATUS_OK
            ? MYOS_STATUS_INTERNAL : started.status);
        report(info, attempt == 0 ? "[service-start] first rollback complete\n"
            : "[service-start] second rollback complete\n");
    }
    exit();
}
