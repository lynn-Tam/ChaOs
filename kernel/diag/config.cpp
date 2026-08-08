#include <diag/concurrency.hpp>
#include <sync/trace.hpp>

#ifndef MYOS_CONCURRENCY_DIAG
#define MYOS_CONCURRENCY_DIAG 0
#endif
#ifndef MYOS_LOCK_DIAG
#define MYOS_LOCK_DIAG 0
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

namespace kernel::sync {
namespace {

consteval auto selected_sync_level() noexcept -> Level {
    if constexpr (MYOS_LOCK_DIAG >= 3 || MYOS_CONCURRENCY_DIAG >= 4) {
        return Level::Profile;
    } else if constexpr (MYOS_LOCK_DIAG >= 2 || MYOS_CONCURRENCY_DIAG >= 1) {
        return Level::Trace;
    } else if constexpr (MYOS_LOCK_DIAG >= 1) {
        return Level::Verify;
    } else {
        return Level::Off;
    }
}

} // namespace

extern const Level level = selected_sync_level();

auto enabled(Level required) noexcept -> bool {
    return static_cast<u8>(level) >= static_cast<u8>(required);
}

} // namespace kernel::sync
