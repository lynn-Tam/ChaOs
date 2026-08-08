#include <diag/concurrency_private.hpp>

namespace kernel::diag::concurrency {

#if MYOS_CONCURRENCY_DIAG >= 2

void FlightRecorder::initialize(
    CpuId id,
    FlightPage* const (&storage)[page_count]) noexcept {
    id_ = id;
    head_.store<libk::MemoryOrder::Relaxed>(0);
    degraded_.store<libk::MemoryOrder::Relaxed>(0);
    wrapped_.store<libk::MemoryOrder::Relaxed>(0);
    for (usize page = 0; page < page_count; ++page) {
        pages_[page] = storage[page];
        for (auto& record : pages_[page]->records) {
            record.sequence.store<libk::MemoryOrder::Relaxed>(0);
            record.absolute_id.store<libk::MemoryOrder::Relaxed>(
                libk::numeric_limits<u64>::max());
        }
    }
}

void FlightRecorder::push(
    u64 tick,
    FlightDomain domain,
    FlightEvent event,
    u64 actor,
    u64 subject,
    u64 arg0,
    u64 arg1,
    u64 arg2,
    SourceSite site) noexcept {
    const u64 limit = libk::numeric_limits<u64>::max();
    u64 head = head_.load<libk::MemoryOrder::Relaxed>();
    for (;;) {
        if (head == limit) {
            degraded_.store<libk::MemoryOrder::Release>(1);
            return;
        }
        if (head_.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Relaxed>(head, head + 1)) {
            break;
        }
    }
    if (head >= capacity) {
        wrapped_.store<libk::MemoryOrder::Release>(1);
    }
    const usize slot = static_cast<usize>(head % capacity);
    FlightRecord& record =
        pages_[slot / FlightPage::capacity]
            ->records[slot % FlightPage::capacity];
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record.sequence, odd)) {
        degraded_.store<libk::MemoryOrder::Release>(1);
        return;
    }
    record.absolute_id.store<libk::MemoryOrder::Relaxed>(head);
    record.tick.store<libk::MemoryOrder::Relaxed>(tick);
    record.domain.store<libk::MemoryOrder::Relaxed>(static_cast<u32>(domain));
    record.event.store<libk::MemoryOrder::Relaxed>(static_cast<u32>(event));
    record.actor.store<libk::MemoryOrder::Relaxed>(actor);
    record.subject.store<libk::MemoryOrder::Relaxed>(subject);
    record.arg0.store<libk::MemoryOrder::Relaxed>(arg0);
    record.arg1.store<libk::MemoryOrder::Relaxed>(arg1);
    record.arg2.store<libk::MemoryOrder::Relaxed>(arg2);
    record.site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    record.site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.function));
    record.site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    AtomicSnapshotWriter::end(record.sequence, odd);
}

auto FlightRecorder::read(
    usize logical_index,
    FlightRecordValue& result) const noexcept -> bool {
    const u64 head = head_.load<libk::MemoryOrder::Acquire>();
    const usize count = head < capacity ? static_cast<usize>(head) : capacity;
    if (logical_index >= count) {
        return false;
    }
    const u64 absolute = head - count + logical_index;
    const usize slot = static_cast<usize>(absolute % capacity);
    const FlightRecord& record =
        pages_[slot / FlightPage::capacity]
            ->records[slot % FlightPage::capacity];
    for (usize attempt = 0; attempt < 3; ++attempt) {
        const u64 first = AtomicSnapshotReader::begin(record.sequence);
        if ((first & 1U) != 0) {
            continue;
        }
        FlightRecordValue value{};
        value.sequence = first;
        value.absolute_id = record.absolute_id.load<
            libk::MemoryOrder::Relaxed>();
        value.tick = record.tick.load<libk::MemoryOrder::Relaxed>();
        value.domain = static_cast<FlightDomain>(
            record.domain.load<libk::MemoryOrder::Relaxed>());
        value.event = static_cast<FlightEvent>(
            record.event.load<libk::MemoryOrder::Relaxed>());
        value.actor = record.actor.load<libk::MemoryOrder::Relaxed>();
        value.subject = record.subject.load<libk::MemoryOrder::Relaxed>();
        value.arg0 = record.arg0.load<libk::MemoryOrder::Relaxed>();
        value.arg1 = record.arg1.load<libk::MemoryOrder::Relaxed>();
        value.arg2 = record.arg2.load<libk::MemoryOrder::Relaxed>();
        value.site.file = reinterpret_cast<const char*>(
            record.site_file.load<libk::MemoryOrder::Relaxed>());
        value.site.function = reinterpret_cast<const char*>(
            record.site_function.load<libk::MemoryOrder::Relaxed>());
        value.site.line = record.site_line.load<libk::MemoryOrder::Relaxed>();
        if (value.absolute_id == absolute
            && AtomicSnapshotReader::valid(record.sequence, first)) {
            result = value;
            return true;
        }
    }
    return false;
}

#else

void FlightRecorder::initialize(
    CpuId id,
    FlightPage* const (&storage)[page_count]) noexcept {
    static_cast<void>(id);
    static_cast<void>(storage);
}

void FlightRecorder::push(
    u64 tick,
    FlightDomain domain,
    FlightEvent event,
    u64 actor,
    u64 subject,
    u64 arg0,
    u64 arg1,
    u64 arg2,
    SourceSite site) noexcept {
    static_cast<void>(tick);
    static_cast<void>(domain);
    static_cast<void>(event);
    static_cast<void>(actor);
    static_cast<void>(subject);
    static_cast<void>(arg0);
    static_cast<void>(arg1);
    static_cast<void>(arg2);
    static_cast<void>(site);
}

auto FlightRecorder::read(
    usize logical_index,
    FlightRecordValue& result) const noexcept -> bool {
    static_cast<void>(logical_index);
    static_cast<void>(result);
    return false;
}

#endif

} // namespace kernel::diag::concurrency
