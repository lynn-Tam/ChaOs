#include "kernel/trap/dump.hpp"

#include <diag/panic.hpp>

namespace kernel::trap {

void panic_unhandled(
    const Event& event,
    const arch::TrapContext& context) noexcept {
    kernel::diag::FatalEvent fatal{
        .facility = kernel::diag::Facility::Trap,
        .id = kernel::diag::EventId{0x30000001},
        .arguments = {
            static_cast<usize>(event.origin()),
            event.exception() != nullptr
                ? static_cast<usize>(event.exception()->cause)
                : static_cast<usize>(event.interrupt()->cause),
            event.exception() != nullptr
                ? static_cast<usize>(event.exception()->access)
                : 0,
            event.pc(),
            event.fault_addr(),
        },
        .argument_count = 5,
    };
    kernel::diag::panic_unhandled(fatal, context);
}

} // namespace kernel::trap
