#include <sys/start.hpp>
#include <servers/uart/protocol.hpp>
#include <servers/runtime/service.hpp>
#include <sys/channel.hpp>
#include <servers/uart/port.hpp>

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
    auto region = vm_slice(vspace, base, 4096, MYOS_VM_READ | MYOS_VM_WRITE, MYOS_RIGHT_MAP);
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
    bool line_start = true;
    service::Message pending{};
    bool pending_valid{};
    bool eof_pending{};
    bool carriage_return{};
    uint64_t read_sequence = 0;
    uint64_t write_sequence = 0;
    for (;;) {
        // Bound each drain so sustained output cannot starve receive/IRQ ack.
        for (unsigned count = 0; count < 16; ++count) {
            service::Message message{};
            const auto result = service::receive(output, message, false);
            if (result.status == MYOS_STATUS_WOULD_BLOCK || result.status == MYOS_STATUS_BUSY) break;
            service::require(result.status);
            read_sequence = result.value;
            if (message.operation == static_cast<uint64_t>(console::Operation::Prompt)) {
                if (!line_start) { port.put('\n'); line_start = true; }
            } else if (message.operation != static_cast<uint64_t>(console::Operation::Bytes)) {
                continue;
            }
            for (size_t i = 0; i < message.size; ++i) {
                port.put(message.data[i]);
                line_start = message.data[i] == '\n' || message.data[i] == '\r';
            }
        }
        if (!pending_valid) {
            if (eof_pending) {
                pending.operation = static_cast<uint64_t>(stream::Frame::End);
                pending_valid = true;
                eof_pending = false;
            } else {
                uint8_t byte{};
                while (pending.size < sizeof(pending.data) && port.try_get(byte)) {
                    if (byte == '\n' && carriage_return) {
                        carriage_return = false;
                        continue;
                    }
                    carriage_return = byte == '\r';
                    if (carriage_return) byte = '\n';
                    if (byte == 4) {
                        if (pending.size == 0) {
                            pending.operation = static_cast<uint64_t>(stream::Frame::End);
                            pending_valid = true;
                        } else eof_pending = true;
                        break;
                    }
                    pending.data[pending.size++] = static_cast<char>(byte);
                    pending_valid = true;
                    if (byte == '\n') break;
                }
            }
        }
        if (pending_valid) {
            const auto result = service::send(input, pending, false);
            if (result.status == MYOS_STATUS_OK) {
                write_sequence = result.value;
                pending = {};
                pending_valid = false;
            } else if (result.status != MYOS_STATUS_WOULD_BLOCK && result.status != MYOS_STATUS_BUSY) {
                service::require(result.status);
            }
        }
        if (!pending_valid && eof_pending) continue;
        if (!pending_valid) {
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
        if (pending_valid)
            service::require(channel_arm(input, writable.value, write_sequence).status);
        service::require(notification_wait(events).status);
    }
}
