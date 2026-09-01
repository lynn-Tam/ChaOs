#include <user/lib/bootstrap.hpp>
#include <user/lib/context.hpp>
#include <user/lib/syscall.hpp>
#include <user/lib/uart.hpp>
#include <uapi/bootstrap.h>
#include <uapi/status.h>
#include <uapi/vm.h>

namespace {

constexpr myos_word_t UartAddress = 0x3001'0000;
constexpr myos_word_t PageSize = 4096;
constexpr myos_word_t IrqBadge = 1;

[[noreturn]] void stop() noexcept {
    myos::exit();
}

[[noreturn]] void run(
    const myos::bootstrap::BootstrapView& info) noexcept {
    const myos_cap_t vspace = info.selector(MYOS_BOOTSTRAP_CAP_VSPACE);
    const myos_cap_t memory = info.selector(MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY);
    const myos_cap_t irq = info.selector(MYOS_BOOTSTRAP_CAP_IRQ);
    const myos_cap_t notification = info.selector(
        MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    const myos_cap_t readiness = info.selector(
        MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION);
    if (vspace == 0 || memory == 0 || irq == 0 || notification == 0
        || readiness == 0) {
        stop();
    }

    const auto region = myos::vm_create_region(
        vspace, UartAddress, PageSize,
        MYOS_VM_READ | MYOS_VM_WRITE,
        MYOS_VM_DEVICE,
        MYOS_RIGHT_MAP);
    if (region.status != MYOS_STATUS_OK) {
        stop();
    }
    const auto mapped = myos::vm_map(
        region.value, memory, UartAddress, PageSize, 0,
        MYOS_VM_READ | MYOS_VM_WRITE);
    if (mapped.status != MYOS_STATUS_OK) {
        stop();
    }

    // Bind first, then enable the UART's receive interrupt.  This ordering
    // prevents an early byte from becoming an unobservable PLIC edge.
    const auto bound = myos::irq_bind(irq, notification, IrqBadge);
    if (bound.status != MYOS_STATUS_OK) {
        stop();
    }
    myos::uart::Port port{UartAddress};
    port.enable_rx();
    if (!port.valid()) {
        stop();
    }
    myos::uart::Printer printer{myos::uart::Writer{port}};
    if (!printer.print<"uart: online irq={} base={:#x}\n">(
            IrqBadge, UartAddress)) {
        stop();
    }
    if (myos::notification_signal(readiness).status != MYOS_STATUS_OK) {
        stop();
    }

    for (;;) {
        const auto notice = myos::notification_wait(notification);
        if (notice.status != MYOS_STATUS_OK
            || (notice.value & IrqBadge) == 0) {
            if (notice.status == MYOS_STATUS_CLOSED) {
                stop();
            }
            myos::yield();
            continue;
        }

        const auto observed = myos::irq_observe(irq);
        if (observed.status != MYOS_STATUS_OK) {
            myos::yield();
            continue;
        }
        uint8_t value{};
        while (port.try_get(value)) {
            if (value == '\r') {
                port.put('\n');
            } else {
                port.put(static_cast<char>(value));
            }
        }
        const auto acknowledged = myos::irq_ack(
            irq, observed.value2, observed.value);
        if (acknowledged.status != MYOS_STATUS_OK
            && acknowledged.status != MYOS_STATUS_REASSERTED) {
            myos::yield();
        }
    }
}

} // namespace

extern "C" [[noreturn]] void myos_main(
    const void* bootstrap,
    myos_word_t bootstrap_size) noexcept {
    const auto info = myos::bootstrap::BootstrapView::parse(
        bootstrap, bootstrap_size);
    if (!info || info->cpu_count() == 0) {
        stop();
    }
    run(*info);
}
