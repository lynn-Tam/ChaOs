#include <boot/timebase.hpp>

namespace kernel::boot {
namespace {

class TimebaseCollector final {
public:
    [[nodiscard]] auto begin_node(kernel::boot::fdt::StrView name, int depth) noexcept
        -> bool {
        if (depth == 1) {
            in_cpus_ = name == "cpus";
            saw_cpus_ = saw_cpus_ || in_cpus_;
        }
        return true;
    }

    [[nodiscard]] auto prop(
        kernel::boot::fdt::StrView name,
        kernel::boot::fdt::ByteSpan value,
        int depth) noexcept -> bool {
        if (depth != 1 || !in_cpus_
            || name != "timebase-frequency") {
            return true;
        }
        if (saw_frequency_) {
            error_ = TimebaseError::DuplicateFrequency;
            return false;
        }
        saw_frequency_ = true;

        kernel::boot::fdt::ByteReader reader{value.data(), value.size()};
        u32 frequency{};
        if (value.size() != sizeof(frequency)
            || !reader.read_be32(frequency)
            || frequency == 0) {
            error_ = TimebaseError::InvalidFrequency;
            return false;
        }
        frequency_ = frequency;
        return true;
    }

    [[nodiscard]] auto end_node(int depth) noexcept -> bool {
        if (depth == 1) {
            in_cpus_ = false;
        }
        return true;
    }

    [[nodiscard]] auto finish() const noexcept -> TimebaseResult {
        if (!saw_cpus_) {
            return libk::unexpected(TimebaseError::MissingCpusNode);
        }
        if (!saw_frequency_) {
            return libk::unexpected(TimebaseError::MissingFrequency);
        }
        return libk::expected(static_cast<u64>(frequency_));
    }

    [[nodiscard]] auto error() const noexcept -> TimebaseError {
        return error_;
    }

private:
    bool in_cpus_{};
    bool saw_cpus_{};
    bool saw_frequency_{};
    u32 frequency_{};
    TimebaseError error_{TimebaseError::InvalidFrequency};
};

} // namespace

auto parse_timebase_frequency(const kernel::boot::fdt::FDT_View& view) noexcept
    -> TimebaseResult {
    TimebaseCollector collector{};
    if (!kernel::boot::fdt::walk_view(view, collector)) {
        return libk::unexpected(collector.error());
    }
    return collector.finish();
}

} // namespace kernel::boot
