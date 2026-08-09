#include <diag/concurrency.hpp>

#ifndef MYOS_CONCURRENCY_DIAG
#define MYOS_CONCURRENCY_DIAG 0
#endif
namespace kernel::diag::concurrency {
namespace {

consteval auto selected_level() noexcept -> Level {
    if constexpr (MYOS_CONCURRENCY_DIAG >= 4) {
        return Level::Profile;
    } else if constexpr (MYOS_CONCURRENCY_DIAG >= 3) {
        return Level::Watch;
    } else if constexpr (MYOS_CONCURRENCY_DIAG >= 2) {
        return Level::Trace;
    } else if constexpr (MYOS_CONCURRENCY_DIAG >= 1) {
        return Level::Snapshot;
    } else {
        return Level::Off;
    }
}

} // namespace

extern const Level level = selected_level();

auto enabled(Level required) noexcept -> bool {
    return static_cast<u8>(level) >= static_cast<u8>(required);
}

} // namespace kernel::diag::concurrency
