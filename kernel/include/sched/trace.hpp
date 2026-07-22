#pragma once

#include <core/types.hpp>
#include <core/debug.hpp>
#include <cpu/topology.hpp>
#include <libk/inplace_ring.hpp>
#include <sched/types.hpp>
#include <time/time.hpp>

namespace kernel::sched {

struct DispatchRecord final {
    CpuId cpu{};
    usize outgoing{};
    usize incoming{};
    usize context{};
    DispatchReason reason{DispatchReason::Start};
    time::Instant observed_at{};
    time::Duration charged{};
    time::Instant deadline{time::Instant::max()};
    usize ready_count{};
    usize timer_count{};
    usize remote_count{};
};

// Per-CPU bounded diagnostic projection. CpuDispatcher is the only producer
// with interrupts disabled. Overwriting the oldest record never changes any
// scheduler state or policy decision.
class DispatchTrace final {
public:
    // Kept intentionally compact because CpuDispatcher is part of one
    // page-bounded CpuRuntime metadata object. A future mapped debug stream
    // can drain this recent-history window without changing scheduler truth.
    static constexpr usize capacity = 14;

    void push(DispatchRecord record) noexcept {
        if (records_.full()) {
            records_.pop_front();
        }
        if (records_.try_emplace_back(record) == nullptr) {
            ++dropped_;
        }
    }

    [[nodiscard]] auto records() const noexcept -> const auto& {
        return records_;
    }

private:
    libk::InplaceRing<DispatchRecord, capacity> records_{};
    usize dropped_{};
};

} // namespace kernel::sched
