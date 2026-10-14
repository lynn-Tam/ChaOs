#include <util.hpp>
#include <start.hpp>
#include <protocols/stream.hpp>
#include <protocols/msg.hpp>
#include <servers/uart.hpp>
#include <libk/parse.hpp>

int main(int argc, char** argv) {

    const auto& info = rt_startup();
    if (argc != 2) sys::exit(STATUS_BAD_ARGS);
    const auto offset = libk::parse<uint64_t>(argv[1]);
    if (!offset || *offset > 4096 - 8) sys::exit(STATUS_BAD_ARGS);
    const auto vspace = info.vspace;
    const auto device = info.selector(uart_mem);
    const auto irq = info.selector(uart_irq);
    const auto events = info.events;
    const auto output = info.selector(con_output);
    const auto input = info.selector(con_input);
    constexpr uintptr_t base = 0x30010000;
    auto region = sys::vm_slice(vspace, base, 4096, VM_READ | VM_WRITE, RIGHT_MAP);
    if (region.status != STATUS_OK) sys::exit(region.status);
    if (const auto rc = sys::vm_map(region.value, device, base, 4096, 0, VM_READ | VM_WRITE).status;
        rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::irq_bind(irq, events, rt_event_badge).status; rc != STATUS_OK)
        sys::exit(rc);
    const auto readable = sys::channel_bind(output, events, CHANNEL_READABLE);
    const auto writable = sys::channel_bind(input, events, CHANNEL_WRITABLE);
    if (readable.status != STATUS_OK) sys::exit(readable.status);
    if (writable.status != STATUS_OK) sys::exit(writable.status);
    uart_port port{base + *offset};
    port.enable_rx();
    port.write("uart: console ready\n");
    bool line_start = true;
    msg_frame pending{};
    bool pending_valid{};
    bool eof_pending{};
    bool carriage_return{};
    uint64_t read_sequence = 0;
    uint64_t write_sequence = 0;
    for (;;) {
        // Bound each drain so sustained output cannot starve receive/IRQ ack.
        for (unsigned count = 0; count < 16; ++count) {
            msg_frame message{};
            const auto result = msg_recv(output, message, false);
            if (result.status == STATUS_WOULD_BLOCK || result.status == STATUS_BUSY)
                break;
            if (result.status != STATUS_OK) sys::exit(result.status);
            read_sequence = result.value;
            if (message.operation == static_cast<uint64_t>(con_operation::Prompt)) {
                if (!line_start) {
                    port.put('\n');
                    line_start = true;
                }
            } else if (message.operation != static_cast<uint64_t>(con_operation::Bytes)) {
                continue;
            }
            for (size_t i = 0; i < message.size; ++i) {
                port.put(message.data[i]);
                line_start = message.data[i] == '\n' || message.data[i] == '\r';
            }
        }
        if (!pending_valid) {
            if (eof_pending) {
                pending.operation = static_cast<uint64_t>(str_frame::End);
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
                    if (carriage_return)
                        byte = '\n';
                    if (byte == 4) {
                        if (pending.size == 0) {
                            pending.operation = static_cast<uint64_t>(str_frame::End);
                            pending_valid = true;
                        } else
                            eof_pending = true;
                        break;
                    }
                    pending.data[pending.size++] = static_cast<char>(byte);
                    pending_valid = true;
                    if (byte == '\n')
                        break;
                }
            }
        }
        if (pending_valid) {
            const auto result = msg_send(input, pending, false);
            if (result.status == STATUS_OK) {
                write_sequence = result.value;
                pending = {};
                pending_valid = false;
            } else if (result.status != STATUS_WOULD_BLOCK && result.status != STATUS_BUSY) {
                if (result.status != STATUS_OK) sys::exit(result.status);
            }
        }
        if (!pending_valid && eof_pending)
            continue;
        if (!pending_valid) {
            const auto observed = sys::irq_observe(irq);
            if (observed.status == STATUS_OK) {
                const auto status = sys::irq_ack(irq, observed.value2, observed.value).status;
                if (status != STATUS_REASSERTED)
                    if (status != STATUS_OK) sys::exit(status);
                // BoundIdle has no dispatched delivery to acknowledge.
            } else if (observed.status != STATUS_BUSY && observed.status != STATUS_WOULD_BLOCK &&
                       observed.status != STATUS_RETRY) {
                if (observed.status != STATUS_OK) sys::exit(observed.status);
            }
            if (port.rx_ready())
                continue;
        }
        if (const auto rc = sys::channel_arm(output, readable.value, read_sequence).status;
            rc != STATUS_OK)
            sys::exit(rc);
        if (pending_valid)
            if (const auto rc = sys::channel_arm(input, writable.value, write_sequence).status;
                rc != STATUS_OK)
                sys::exit(rc);
        if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
            sys::exit(rc);
    }
}
