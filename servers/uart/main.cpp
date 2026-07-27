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

[[nodiscard]] auto valid(
    const myos_bootstrap_info* info,
    myos_word_t size) noexcept -> bool {
    return info != nullptr
        && size >= sizeof(myos_bootstrap_info)
        && info->magic == MYOS_BOOTSTRAP_MAGIC
        && info->major == MYOS_BOOTSTRAP_MAJOR
        && info->minor >= MYOS_BOOTSTRAP_MINOR
        && info->size == sizeof(myos_bootstrap_info)
        && info->cap_count <= MYOS_BOOTSTRAP_MAX_CAPS
        && info->cpu_count != 0;
}

[[nodiscard]] auto capability(
    const myos_bootstrap_info& info,
    uint32_t kind) noexcept -> myos_cap_t {
    for (uint32_t index = 0; index < info.cap_count; ++index) {
        if (info.caps[index].kind == kind) {
            return info.caps[index].handle;
        }
    }
    return 0;
}

[[noreturn]] void stop() noexcept {
    myos::exit();
}

[[noreturn]] void run(const myos_bootstrap_info& info) noexcept {
    const myos_cap_t vspace = capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    const myos_cap_t memory = capability(
        info, MYOS_BOOTSTRAP_CAP_UART_MEMORY);
    const myos_cap_t irq = capability(info, MYOS_BOOTSTRAP_CAP_UART_IRQ);
    const myos_cap_t notification = capability(
        info, MYOS_BOOTSTRAP_CAP_UART_NOTIFICATION);
    if (vspace == 0 || memory == 0 || irq == 0 || notification == 0) {
        stop();
    }

    const auto region = myos::vm_create_region(
        vspace, UartAddress, PageSize,
        MYOS_VM_READ | MYOS_VM_WRITE,
        MYOS_VM_DEVICE,
        MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP);
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
    port.initialize();
    if (!port.valid()) {
        stop();
    }
    myos::uart::Printer printer{myos::uart::Writer{port}};
    if (!printer.print<"uart: online irq={} base={:#x}\n">(
            IrqBadge, UartAddress)) {
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
        const auto acknowledged = myos::irq_ack(irq, observed.value);
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
    const auto* const info = static_cast<const myos_bootstrap_info*>(
        bootstrap);
    if (!valid(info, bootstrap_size)) {
        stop();
    }
    run(*info);
}
