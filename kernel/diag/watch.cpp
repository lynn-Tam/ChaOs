#include <diag/concurrency_private.hpp>

namespace kernel::diag::concurrency {

void WatchdogCandidate::publish(const StallFingerprint& value) noexcept {
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(fingerprint_sequence, odd)) {
        return;
    }
    root_identity.store<libk::MemoryOrder::Relaxed>(value.root.identity);
    root_kind.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(value.root.kind));
    root_generation.store<libk::MemoryOrder::Relaxed>(
        value.root.generation);
    fingerprint_phase.store<libk::MemoryOrder::Relaxed>(value.phase);
    fingerprint_progress.store<libk::MemoryOrder::Relaxed>(
        value.progress_epoch);
    fingerprint_activity.store<libk::MemoryOrder::Relaxed>(
        value.activity_epoch);
    relation_hash.store<libk::MemoryOrder::Relaxed>(value.relation_hash);
    driver_identity.store<libk::MemoryOrder::Relaxed>(value.driver.identity);
    driver_kind.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(value.driver.kind));
    driver_generation.store<libk::MemoryOrder::Relaxed>(
        value.driver.generation);
    blocker_identity.store<libk::MemoryOrder::Relaxed>(value.blocker.identity);
    blocker_kind.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(value.blocker.kind));
    blocker_generation.store<libk::MemoryOrder::Relaxed>(
        value.blocker.generation);
    AtomicSnapshotWriter::end(fingerprint_sequence, odd);
}

auto WatchdogCandidate::read(StallFingerprint& value) const noexcept -> bool {
    for (usize attempt = 0; attempt < 3; ++attempt) {
        const u64 first = AtomicSnapshotReader::begin(fingerprint_sequence);
        if ((first & 1U) != 0) {
            continue;
        }
        StallFingerprint candidate{};
        candidate.root = NodeRef{
            static_cast<NodeRef::Kind>(
                root_kind.load<libk::MemoryOrder::Relaxed>()),
            root_identity.load<libk::MemoryOrder::Relaxed>(),
            root_generation.load<libk::MemoryOrder::Relaxed>()};
        candidate.phase = fingerprint_phase.load<libk::MemoryOrder::Relaxed>();
        candidate.progress_epoch = fingerprint_progress.load<
            libk::MemoryOrder::Relaxed>();
        candidate.activity_epoch = fingerprint_activity.load<
            libk::MemoryOrder::Relaxed>();
        candidate.relation_hash = relation_hash.load<
            libk::MemoryOrder::Relaxed>();
        candidate.driver = NodeRef{
            static_cast<NodeRef::Kind>(driver_kind.load<
                libk::MemoryOrder::Relaxed>()),
            driver_identity.load<libk::MemoryOrder::Relaxed>(),
            driver_generation.load<libk::MemoryOrder::Relaxed>()};
        candidate.blocker = NodeRef{
            static_cast<NodeRef::Kind>(blocker_kind.load<
                libk::MemoryOrder::Relaxed>()),
            blocker_identity.load<libk::MemoryOrder::Relaxed>(),
            blocker_generation.load<libk::MemoryOrder::Relaxed>()};
        if (AtomicSnapshotReader::valid(fingerprint_sequence, first)) {
            value = candidate;
            return true;
        }
    }
    return false;
}

} // namespace kernel::diag::concurrency
