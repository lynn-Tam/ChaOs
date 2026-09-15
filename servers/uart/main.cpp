#include <user/lib/imports.hpp>
#include <user/lib/service.hpp>
#include <user/lib/uart.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto vspace = service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    const auto device = service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY);
    const auto irq = service::capability(info, MYOS_BOOTSTRAP_CAP_IRQ);
    const auto events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    const auto output = service::capability(info, myos::bootstrap::imports::ConsoleOutput);
    const auto input = service::capability(info, myos::bootstrap::imports::ConsoleInput);
    constexpr uintptr_t base = 0x30010000;
    auto region = vm_create_region(vspace, base, 4096, MYOS_VM_READ | MYOS_VM_WRITE,
                                   MYOS_VM_DEVICE, MYOS_RIGHT_MAP);
    service::require(region.status);
    service::require(vm_map(region.value, device, base, 4096, 0,
                            MYOS_VM_READ | MYOS_VM_WRITE).status);
    service::require(irq_bind(irq, events, service::EventsBadge).status);
    const auto readable = channel_bind(output, events, MYOS_CHANNEL_READABLE);
    const auto writable = channel_bind(input, events, MYOS_CHANNEL_WRITABLE);
    service::require(readable.status);
    service::require(writable.status);
    uart::Port port{base};
    port.enable_rx();
    port.write("uart: console ready\n");
    service::Message pending{};
    uint64_t read_sequence = 0;
    uint64_t write_sequence = 0;
    for (;;) {
        // Bound each drain so sustained output cannot starve receive/IRQ ack.
        for (unsigned count = 0; count < MYOS_CHANNEL_MAX_QUEUE; ++count) {
            service::Message message{};
            const auto result = service::receive(output, message, false);
            if (result.status == MYOS_STATUS_WOULD_BLOCK || result.status == MYOS_STATUS_BUSY) break;
            service::require(result.status);
            read_sequence = result.value;
            port.write(message.data, message.size);
        }
        if (pending.size == 0) {
            uint8_t byte{};
            while (pending.size < sizeof(pending.data) && port.try_get(byte))
                pending.data[pending.size++] = static_cast<char>(byte);
        }
        if (pending.size != 0) {
            const auto result = service::send(input, pending, false);
            if (result.status == MYOS_STATUS_OK) {
                write_sequence = result.value;
                pending = {};
            } else if (result.status != MYOS_STATUS_WOULD_BLOCK && result.status != MYOS_STATUS_BUSY) {
                service::require(result.status);
            }
        }
        if (pending.size == 0) {
            const auto observed = irq_observe(irq);
            if (observed.status == MYOS_STATUS_OK) {
                const auto status = irq_ack(irq, observed.value2, observed.value).status;
                if (status != MYOS_STATUS_REASSERTED) service::require(status);
            // BoundIdle has no dispatched delivery to acknowledge.
            } else if (observed.status != MYOS_STATUS_BUSY
                       && observed.status != MYOS_STATUS_WOULD_BLOCK
                       && observed.status != MYOS_STATUS_RETRY) {
                service::require(observed.status);
            }
            if (port.rx_ready()) continue;
        }
        service::require(channel_arm(output, readable.value, read_sequence).status);
        if (pending.size != 0)
            service::require(channel_arm(input, writable.value, write_sequence).status);
        service::require(notification_wait(events).status);
    }
}
