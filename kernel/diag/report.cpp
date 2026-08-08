#include <diag/concurrency_private.hpp>

namespace kernel::diag::concurrency {

#if MYOS_CONCURRENCY_DIAG >= 3

auto ReportQueue::publish(const ReportRecord& value) noexcept -> bool {
    const u64 head = head_.load<libk::MemoryOrder::Relaxed>();
    const u64 tail = tail_.load<libk::MemoryOrder::Acquire>();
    if (head - tail >= capacity) {
        // ReportQueue is SPSC, so the sole watchdog writer can saturate the
        // loss counter with one load/store pair. Do not turn this diagnostic
        // hint into a CAS retry loop on the IRQ-off producer path.
        const u64 current = lost_.load<libk::MemoryOrder::Relaxed>();
        lost_.store<libk::MemoryOrder::Relaxed>(
            current == libk::numeric_limits<u64>::max()
                ? current : current + 1);
        return false;
    }
    records_[head % capacity] = value;
    head_.store<libk::MemoryOrder::Release>(head + 1);
    return true;
}

auto ReportQueue::consume(ReportRecord& value) noexcept -> bool {
    const u64 tail = tail_.load<libk::MemoryOrder::Relaxed>();
    const u64 head = head_.load<libk::MemoryOrder::Acquire>();
    if (tail == head) {
        return false;
    }
    value = records_[tail % capacity];
    tail_.store<libk::MemoryOrder::Release>(tail + 1);
    return true;
}

auto ReportQueue::pending() const noexcept -> bool {
    return tail_.load<libk::MemoryOrder::Acquire>()
        != head_.load<libk::MemoryOrder::Acquire>();
}

#else

auto ReportQueue::publish(const ReportRecord& value) noexcept -> bool {
    static_cast<void>(value);
    return false;
}

auto ReportQueue::consume(ReportRecord& value) noexcept -> bool {
    static_cast<void>(value);
    return false;
}

auto ReportQueue::pending() const noexcept -> bool {
    return false;
}

#endif

} // namespace kernel::diag::concurrency
