#include <boot/timebase.hpp>

namespace kernel::boot {

auto parse_timebase_frequency(const Fdt& tree) noexcept -> TimebaseResult {
    const auto cpus = tree.child(tree.root(), "cpus");
    if (!cpus) {
        return libk::unexpected(TimebaseError::MissingCpusNode);
    }
    const size_t count = tree.property_count(*cpus, "timebase-frequency");
    if (count == 0) {
        return libk::unexpected(TimebaseError::MissingFrequency);
    }
    if (count != 1) {
        return libk::unexpected(TimebaseError::DuplicateFrequency);
    }
    uint32_t frequency{};
    if (!Fdt::read_u32(*tree.property(*cpus, "timebase-frequency"), frequency)
        || frequency == 0) {
        return libk::unexpected(TimebaseError::InvalidFrequency);
    }
    return libk::expected(static_cast<uint64_t>(frequency));
}

} // namespace kernel::boot
