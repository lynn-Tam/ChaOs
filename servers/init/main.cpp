#include <servers/runtime/service.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/deploy/services.hpp>
#include <servers/uart/port.hpp>

namespace {
deploy::program program;
constexpr size_t ServiceCapacity = 8;
using Supervisor = deploy::tasks<ServiceCapacity, 24>;
Supervisor supervisor;

void report_uart(const myos::bootstrap::BootstrapView& info,
    myos_status_t startup = MYOS_STATUS_OK,
    deploy::ByteView task = {}) {
    const auto vspace = myos::service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    const auto memory = myos::service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY);
    constexpr auto address = 0x30010000;
    const auto region = myos::vm_slice(vspace, address, 4096,
        MYOS_VM_READ | MYOS_VM_WRITE,
        MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
    myos::service::require(region.status);
    myos::service::require(myos::vm_map(region.value, memory, address, 4096, 0,
                                      MYOS_VM_READ | MYOS_VM_WRITE).status);
    myos::uart::Port port{address};
    if (startup == MYOS_STATUS_OK) {
        port.reset();
        port.write("init: native services\n");
    } else {
        (void)libk::fmt::format_to<"init: service startup failed: {}">(port, startup);
        if (task) {
            port.write(" task=");
            port.write(reinterpret_cast<const char*>(task.data()), task.size());
        }
        port.write("\n");
    }
    myos::service::require(myos::vm_unmap(region.value, address, 4096).status);
    myos::service::require(myos::vm_clear(region.value).status);
    myos::service::require(myos::cap_close(region.value).status);
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = myos::service::bootstrap(address, size);
    report_uart(info);
    myos::service::require(supervisor.load(program, info));
    supervisor.open(info);
    myos::service::require(supervisor.add_boot_sources(info));
    myos::service::require(supervisor.add("uart.memory",
        myos::service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY),
        MYOS_OBJECT_KIND_MEMORY, MYOS_RIGHT_MAP, 0, 1,
        MYOS_VM_READ | MYOS_VM_WRITE, 0));
    myos::service::require(supervisor.add("uart.irq",
        myos::service::capability(info, MYOS_BOOTSTRAP_CAP_IRQ), MYOS_OBJECT_KIND_IRQ,
        MYOS_RIGHT_ROUTE | MYOS_RIGHT_OBSERVE | MYOS_RIGHT_ACK));
    const auto pool = myos::service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const auto notification = myos::notification_create(pool, 1);
    myos::service::require(notification.status);
    myos::cap::OwnedCap events{{notification.value, 0}};
    myos::service::require(supervisor.add("service.wake", events.selector(),
        MYOS_OBJECT_KIND_NOTIFICATION, MYOS_RIGHT_SIGNAL));
    const auto pair = myos::channel_create(pool, 4, MYOS_CHANNEL_MAX_WORDS, 1, 1);
    myos::service::require(pair.status);
    myos::cap::OwnedCap control_root{{pair.value, 0}}, control_client{{pair.value2, 0}};
    const auto endpoint = myos::channel_mint(control_root.selector(),
        myos::service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), 1,
        MYOS_RIGHT_RECEIVE | MYOS_RIGHT_SEND | MYOS_RIGHT_DUPLICATE);
    myos::service::require(endpoint.status);
    control_root = myos::cap::OwnedCap{{endpoint.value, 0}};
    myos::service::require(supervisor.add("service.control", control_client.selector(),
        MYOS_OBJECT_KIND_CHANNEL, MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_DUPLICATE, 1));
    myos::service::Connection control{control_root.selector(), events.selector()};
    for (size_t i = 0; i < info.device_count(); ++i) {
        const auto* device = info.device_import(i);
        myos::service::require(supervisor.add(device->name, device->handle,
            MYOS_OBJECT_KIND_DEVICE, MYOS_RIGHT_CONNECT | MYOS_RIGHT_DUPLICATE));
    }
    deploy::services<ServiceCapacity, 24> services{supervisor, program, events.selector()};
    const auto started = services.start();
    if (started.status != MYOS_STATUS_OK) {
        const auto task = started.task
            ? program.plan().symbol(program.plan().task(*started.task)->name)
            : deploy::ByteView{};
        report_uart(info, started.status, task);
    }
    myos::service::require(started.status);
    for (;;) {
        myos::service::require(services.poll());
        bool requested{};
        for (;;) {
            myos::service::Message request{};
            myos::cap::OwnedCap reply_endpoint;
            const auto received = control.try_receive(request, reply_endpoint);
            if (received.status == MYOS_STATUS_WOULD_BLOCK) break;
            if (received.status == MYOS_STATUS_BAD_ARGS
                || received.status == MYOS_STATUS_TRANSFER_FAILED) continue;
            myos::service::require(received.status);
            const auto index = program.plan().find_task({
                reinterpret_cast<const uint8_t*>(request.data), static_cast<size_t>(request.size)});
            const auto status = index ? services.restart(*index) : MYOS_STATUS_NOT_FOUND;
            myos::service::Message reply{.status = status};
            // The client supplies this capability; a revoked or malformed
            // reply endpoint must not terminate the root supervisor.
            (void)myos::service::send(reply_endpoint.selector(), reply, false);
            requested |= status == MYOS_STATUS_OK;
        }
        myos::service::require(control.arm().status);
        if (requested) continue;
        if (services.needs_poll()) { myos::yield(); continue; }
        const auto wake = myos::notification_wait(events.selector(), services.deadline());
        if (wake.status == MYOS_STATUS_TIMED_OUT) continue;
        myos::service::require(wake.status);
        services.notify(wake.value);
    }
}
