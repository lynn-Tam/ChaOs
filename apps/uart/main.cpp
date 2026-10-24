#include <vm.hpp>
#include <util.hpp>
#include <start.hpp>
#include <ubus/ubus.hpp>
#include "uart.hpp"

int main() {
    const auto& info = rt_startup();
    ubus_conn parent;
    auto status = parent.bind(info.environment); if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = parent.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    const auto message = setup.data["message"];
    const auto offset = message["offset"].as<uint64_t>(); if (!offset || *offset > 4096 - 8) return STATUS_BAD_ARGS;
    const auto vspace = info.vspace;
    const auto device = setup.capability(message["registers"]);
    const auto irq = setup.capability(message["irq"]);
    const auto events = rt_events();
    const auto output = setup.capability(message["stdin"]);
    const auto input = setup.capability(message["stdout"]);
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
    std::array<char, 80> pending{};
    size_t pending_size{};
    bool pending_valid{};
    for (;;) {
        // Bound each drain so sustained output cannot starve receive/IRQ ack.
        for (unsigned count = 0; count < 16; ++count) {
            ubus_msg event;
            const auto status = ubus_receive_event(output, event);
            if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) break;
            if (status != STATUS_OK) sys::exit(status);
            const auto bytes = event.data["data"].as<std::span<const std::byte>>();
            if (!bytes) continue;
            for (const auto byte : *bytes) port.put(char(byte));
        }
        if (!pending_valid) {
            uint8_t byte{};
            while (pending_size < pending.size() && port.try_get(byte)) {
                pending[pending_size++] = static_cast<char>(byte);
                pending_valid = true;
            }
        }
        if (pending_valid) {
            ubus_msg frame; frame.data.put("data", std::as_bytes(std::span{pending}.first(pending_size)));
            const auto status = ubus_send(input, frame, false);
            if (status == STATUS_OK) {
                pending_size = 0;
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
