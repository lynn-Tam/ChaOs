#pragma once

#include <boot/firmware/devicetree/fdt.hpp>
#include <core/types.hpp>
#include <libk/expected.hpp>

namespace kernel::boot {

enum class TimebaseError : u8 {
    MissingCpusNode,
    MissingFrequency,
    DuplicateFrequency,
    InvalidFrequency,
};

using TimebaseResult = libk::Expected<u64, TimebaseError>;

[[nodiscard]] auto parse_timebase_frequency(
    const Fdt& tree) noexcept -> TimebaseResult;

} // namespace kernel::boot
