#include <servers/runtime/service.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/deploy/services.hpp>
#include <servers/uart/port.hpp>

namespace {
deploy::program program;
constexpr size_t ServiceCapacity = 8;
using Supervisor = deploy::tasks<ServiceCapacity, 24>;
Supervisor supervisor;

void report_uart(const boot::BootView& info,
    status_t startup = STATUS_OK,
    deploy::ByteView task = {}) {
    const auto vspace = sys::service::capability(info, BOOT_VSPACE);
    const auto memory = sys::service::capability(info, boot::UartMem);
    constexpr auto address = 0x30010000;
    const auto region = sys::vm_slice(vspace, address, 4096,
        VM_READ | VM_WRITE,
        RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
    sys::service::require(region.status);
    sys::service::require(sys::vm_map(region.value, memory, address, 4096, 0,
                                      VM_READ | VM_WRITE).status);
    sys::uart::Port port{address};
    if (startup == STATUS_OK) {
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
    sys::service::require(sys::vm_unmap(region.value, address, 4096).status);
    sys::service::require(sys::vm_clear(region.value).status);
    sys::service::require(sys::cap_close(region.value).status);
}
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    const auto info = sys::service::bootstrap(address, size);
    report_uart(info);
    sys::service::require(supervisor.load(program, info));
    supervisor.open(info);
    sys::service::require(supervisor.add_boot_sources(info));
    sys::service::require(supervisor.add("uart.memory",
        sys::service::capability(info, boot::UartMem),
        OBJECT_KIND_MEMORY, RIGHT_MAP, 0, 1,
        VM_READ | VM_WRITE, 0));
    sys::service::require(supervisor.add("uart.irq",
        sys::service::capability(info, boot::UartIrq), OBJECT_KIND_IRQ,
        RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK));
    const auto pool = sys::service::capability(info, BOOT_POOL);
    const auto notification = sys::notification_create(pool, 1);
    sys::service::require(notification.status);
    sys::cap::OwnedCap events{{notification.value, 0}};
    sys::service::require(supervisor.add("service.wake", events.selector(),
        OBJECT_KIND_NOTIFICATION, RIGHT_SIGNAL));
    const auto pair = sys::channel_create(pool, 4, CHANNEL_MAX_WORDS, 1, 1);
    sys::service::require(pair.status);
    sys::cap::OwnedCap control_root{{pair.value, 0}}, control_client{{pair.value2, 0}};
    const auto endpoint = sys::channel_mint(control_root.selector(),
        sys::service::capability(info, BOOT_CSPACE), 1,
        RIGHT_RECEIVE | RIGHT_SEND | RIGHT_DUPLICATE);
    sys::service::require(endpoint.status);
    control_root = sys::cap::OwnedCap{{endpoint.value, 0}};
    sys::service::require(supervisor.add("service.control", control_client.selector(),
        OBJECT_KIND_CHANNEL, RIGHT_SEND | RIGHT_RECEIVE | RIGHT_DUPLICATE, 1));
    sys::service::Connection control{control_root.selector(), events.selector()};
    for (size_t i = 0; i < info.device_count(); ++i) {
        const auto* device = info.device_import(i);
        sys::service::require(supervisor.add(device->name, device->handle,
            OBJECT_KIND_DEVICE, RIGHT_CONNECT | RIGHT_DUPLICATE));
    }
    deploy::services<ServiceCapacity, 24> services{supervisor, program, events.selector()};
    const auto started = services.start();
    if (started.status != STATUS_OK) {
        const auto task = started.task
            ? program.plan().symbol(program.plan().task(*started.task)->name)
            : deploy::ByteView{};
        report_uart(info, started.status, task);
    }
    sys::service::require(started.status);
    for (;;) {
        sys::service::require(services.poll());
        bool requested{};
        for (;;) {
            sys::service::Message request{};
            sys::cap::OwnedCap reply_endpoint;
            const auto received = control.try_receive(request, reply_endpoint);
            if (received.status == STATUS_WOULD_BLOCK) break;
            if (received.status == STATUS_BAD_ARGS
                || received.status == STATUS_TRANSFER_FAILED) continue;
            sys::service::require(received.status);
            const auto index = program.plan().find_task({
                reinterpret_cast<const uint8_t*>(request.data), static_cast<size_t>(request.size)});
            const auto status = index ? services.restart(*index) : STATUS_NOT_FOUND;
            sys::service::Message reply{.status = status};
            // The client supplies this capability; a revoked or malformed
            // reply endpoint must not terminate the root supervisor.
            (void)sys::service::send(reply_endpoint.selector(), reply, false);
            requested |= status == STATUS_OK;
        }
        sys::service::require(control.arm().status);
        if (requested) continue;
        if (services.needs_poll()) { sys::yield(); continue; }
        const auto wake = sys::notification_wait(events.selector(), services.deadline());
        if (wake.status == STATUS_TIMED_OUT) continue;
        sys::service::require(wake.status);
        services.notify(wake.value);
    }
}
