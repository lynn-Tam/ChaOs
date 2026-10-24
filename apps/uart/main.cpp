#include <io.hpp>
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
    ev_watch readable, writable, interrupt;
    for (auto status : {readable.open(events, output, SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_READ),
        writable.open(events, input, SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE),
        interrupt.open(events, irq, SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_READ)})
        if (status != STATUS_OK) sys::exit(status);
    uart_port port{base + *offset};
    port.enable_rx();
    port.write("uart: console ready\n");
    std::array<char, 80> pending{};
    size_t pending_size{};
    bool pending_valid{};
    io_frame outgoing;
    size_t sent{};
    for (;;) {
        if (sent == outgoing.size) {
            const auto received = io_recv(output, outgoing, false);
            if (received != STATUS_OK && received != STATUS_WOULD_BLOCK) sys::exit(received);
            if (received == STATUS_OK) sent = 0;
        }
        const auto end = std::min(outgoing.size, sent + 256);
        while (sent < end) port.put(outgoing.data[sent++]);
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
        if (sent < outgoing.size) continue;
        if (const auto rc = readable.arm().status;
            rc != STATUS_OK)
            sys::exit(rc);
        if (!pending_valid) writable.ack();
        if (pending_valid)
            if (const auto rc = writable.arm().status;
                rc != STATUS_OK)
                sys::exit(rc);
        if (const auto rc = interrupt.arm().status; rc != STATUS_OK) sys::exit(rc);
        if (const auto rc = ev_wait(events).status; rc != STATUS_OK)
            sys::exit(rc);
    }
}
