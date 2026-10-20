#include <vm.hpp>
#include <util.hpp>
#include <start.hpp>
#include <lib/stream/stream.hpp>
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
    const auto output = info.selector(rt_stdin);
    const auto input = info.selector(rt_stdout);
    constexpr uintptr_t base = 0x30010000;
    auto region = sys::vm_slice(vspace, base, 4096, VM_READ | VM_WRITE, RIGHT_MAP);
    if (region.status != STATUS_OK) sys::exit(region.status);
    if (const auto rc = vm_call(region.value, VM_OP_MAP, {device, base, 4096, 0, VM_READ | VM_WRITE});
        rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::irq_set(irq, true).status; rc != STATUS_OK)
        sys::exit(rc);
    const auto readable = sys::port_watch(events, output, SOURCE_READY | SOURCE_TERMINAL, 0, 0, 0, SOURCE_READ);
    const auto writable = sys::port_watch(events, input, SOURCE_READY | SOURCE_TERMINAL, 0, 0, 0, SOURCE_WRITE);
    if (readable.status != STATUS_OK) sys::exit(readable.status);
    if (writable.status != STATUS_OK) sys::exit(writable.status);
    const auto interrupt = sys::port_watch(events, irq, SOURCE_READY | SOURCE_TERMINAL, 0, 0, 0, SOURCE_READ);
    if (interrupt.status != STATUS_OK) sys::exit(interrupt.status);
    uart_port port{base + *offset};
    port.enable_rx();
    port.write("uart: console ready\n");
    str_chunk pending{};
    bool pending_valid{};
    for (;;) {
        // Bound each drain so sustained output cannot starve receive/IRQ ack.
        for (unsigned count = 0; count < 16; ++count) {
            ubus_msg event;
            const auto status = ubus_receive_event(output, event);
            if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) break;
            if (status != STATUS_OK) sys::exit(status);
            str_chunk message;
            if (str_decode(event, message) != STATUS_OK) continue;
            for (size_t i = 0; i < message.size; ++i) {
                port.put(message.data[i]);
            }
        }
        if (!pending_valid) {
            uint8_t byte{};
            while (pending.size < sizeof(pending.data) && port.try_get(byte)) {
                pending.data[pending.size++] = static_cast<char>(byte);
                pending_valid = true;
            }
        }
        if (pending_valid) {
            const auto status = str_send(input, pending, false);
            if (status == STATUS_OK) {
                pending = {};
                pending_valid = false;
            } else if (status != STATUS_WOULD_BLOCK && status != STATUS_BUSY) sys::exit(status);
        }
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
        if (const auto rc = sys::port_arm(events, readable.value).status;
            rc != STATUS_OK)
            sys::exit(rc);
        if (pending_valid)
            if (const auto rc = sys::port_arm(events, writable.value).status;
                rc != STATUS_OK)
                sys::exit(rc);
        if (const auto rc = sys::port_arm(events, interrupt.value).status; rc != STATUS_OK) sys::exit(rc);
        if (const auto rc = ev_wait(events).status; rc != STATUS_OK)
            sys::exit(rc);
    }
}
