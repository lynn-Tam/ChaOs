#include <sys/start.hpp>
#include <sys/protocol.hpp>
#include <sys/channel.hpp>
#include <servers/uart.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    const auto vspace = sys::capability(info, BOOT_VSPACE);
    const auto device = sys::capability(info, boot::UartMem);
    const auto irq = sys::capability(info, boot::UartIrq);
    const auto events = sys::capability(info, BOOT_EVENTS);
    const auto output = sys::capability(info, boot::ConsoleOutput);
    const auto input = sys::capability(info, boot::ConsoleInput);
    constexpr uintptr_t base = 0x30010000;
    auto region = vm_slice(vspace, base, 4096, VM_READ | VM_WRITE, RIGHT_MAP);
    sys::require(region.status);
    sys::require(vm_map(region.value, device, base, 4096, 0,
                            VM_READ | VM_WRITE).status);
    sys::require(irq_bind(irq, events, boot::event_badge).status);
    const auto readable = channel_bind(output, events, CHANNEL_READABLE);
    const auto writable = channel_bind(input, events, CHANNEL_WRITABLE);
    sys::require(readable.status);
    sys::require(writable.status);
    uart::Port port{base};
    port.enable_rx();
    port.write("uart: console ready\n");
    bool line_start = true;
    sys::Message pending{};
    bool pending_valid{};
    bool eof_pending{};
    bool carriage_return{};
    uint64_t read_sequence = 0;
    uint64_t write_sequence = 0;
    for (;;) {
        // Bound each drain so sustained output cannot starve receive/IRQ ack.
        for (unsigned count = 0; count < 16; ++count) {
            sys::Message message{};
            const auto result = sys::receive(output, message, false);
            if (result.status == STATUS_WOULD_BLOCK || result.status == STATUS_BUSY) break;
            sys::require(result.status);
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
            const auto result = sys::send(input, pending, false);
            if (result.status == STATUS_OK) {
                write_sequence = result.value;
                pending = {};
                pending_valid = false;
            } else if (result.status != STATUS_WOULD_BLOCK && result.status != STATUS_BUSY) {
                sys::require(result.status);
            }
        }
        if (!pending_valid && eof_pending) continue;
        if (!pending_valid) {
            const auto observed = irq_observe(irq);
            if (observed.status == STATUS_OK) {
                const auto status = irq_ack(irq, observed.value2, observed.value).status;
                if (status != STATUS_REASSERTED) sys::require(status);
            // BoundIdle has no dispatched delivery to acknowledge.
            } else if (observed.status != STATUS_BUSY
                       && observed.status != STATUS_WOULD_BLOCK
                       && observed.status != STATUS_RETRY) {
                sys::require(observed.status);
            }
            if (port.rx_ready()) continue;
        }
        sys::require(channel_arm(output, readable.value, read_sequence).status);
        if (pending_valid)
            sys::require(channel_arm(input, writable.value, write_sequence).status);
        sys::require(notification_wait(events).status);
    }
}
