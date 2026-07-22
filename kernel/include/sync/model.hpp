#pragma once

#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/limits.hpp>

namespace kernel::sync {

enum class DepStatus : u8 {
    Added,
    Exists,
    Cycle,
};

struct OwnerWord final {
    static constexpr u64 cpu_bits = 9;
    static constexpr u64 cpu_mask = (u64{1} << cpu_bits) - 1;
    static constexpr u64 max_generation =
        libk::numeric_limits<u64>::max() >> cpu_bits;

    [[nodiscard]] static constexpr auto pack(
        CpuId cpu, u64 generation) noexcept -> u64 {
        return (generation << cpu_bits) | (cpu.raw + 1);
    }

    [[nodiscard]] static constexpr auto none(u64 generation) noexcept -> u64 {
        return generation << cpu_bits;
    }

    [[nodiscard]] static constexpr auto cpu(u64 word) noexcept -> usize {
        const u64 encoded = word & cpu_mask;
        return encoded == 0
            ? max_cpu_count : static_cast<usize>(encoded - 1);
    }

    [[nodiscard]] static constexpr auto generation(u64 word) noexcept -> u64 {
        return word >> cpu_bits;
    }
};

static_assert(max_cpu_count + 1 <= (u64{1} << OwnerWord::cpu_bits));

// Fixed-storage directed graph used both by lockdep and pure model tests.
// Multiword rows deliberately avoid coupling the lock taxonomy to u64 width.
template<usize Nodes>
class DepGraph final {
    static constexpr usize word_bits = sizeof(u64) * 8;
    static constexpr usize words = (Nodes + word_bits - 1) / word_bits;

public:
    struct Path final {
        usize nodes[Nodes + 1]{};
        usize size{};
    };

    struct Result final {
        DepStatus status{};
        Path path{};
    };

    [[nodiscard]] constexpr auto has(usize from, usize to) const noexcept
        -> bool {
        return (rows_[from][to / word_bits]
            & (u64{1} << (to % word_bits))) != 0;
    }

    constexpr void insert(usize from, usize to) noexcept {
        rows_[from][to / word_bits] |= u64{1} << (to % word_bits);
    }

    [[nodiscard]] constexpr auto check_insert(
        usize from, usize to) noexcept -> Result {
        if (has(from, to)) {
            return Result{DepStatus::Exists, {}};
        }
        Path reverse{};
        if (path(to, from, reverse)) {
            return Result{DepStatus::Cycle, reverse};
        }
        insert(from, to);
        return Result{DepStatus::Added, {}};
    }

    [[nodiscard]] constexpr auto path(
        usize from, usize to, Path& result) const noexcept -> bool {
        bool visited[Nodes]{};
        usize parent[Nodes]{};
        usize queue[Nodes]{};
        usize head{};
        usize tail{};

        visited[from] = true;
        parent[from] = from;
        queue[tail++] = from;
        while (head != tail) {
            const usize node = queue[head++];
            if (node == to) {
                usize reverse[Nodes]{};
                usize count{};
                for (usize cursor = to;; cursor = parent[cursor]) {
                    reverse[count++] = cursor;
                    if (cursor == from) {
                        break;
                    }
                }
                result.size = count;
                for (usize index = 0; index < count; ++index) {
                    result.nodes[index] = reverse[count - index - 1];
                }
                return true;
            }
            for (usize next = 0; next < Nodes; ++next) {
                if (!visited[next] && has(node, next)) {
                    visited[next] = true;
                    parent[next] = node;
                    queue[tail++] = next;
                }
            }
        }
        result = {};
        return false;
    }

private:
    u64 rows_[Nodes][words]{};
};

struct WaitStamp final {
    usize cpu{};
    usize lock{};
    usize owner_ref{};
    u64 wait_generation{};
    u64 owner_word{};
};

enum class WaitCheck : u8 {
    Stable,
    Changed,
    Open,
    TooShort,
};

template<usize Capacity>
[[nodiscard]] constexpr auto validate_wait_cycle(
    const WaitStamp (&candidate)[Capacity],
    const WaitStamp (&observed)[Capacity],
    usize count) noexcept -> WaitCheck {
    if (count < 2 || count > Capacity) {
        return WaitCheck::TooShort;
    }
    for (usize index = 0; index < count; ++index) {
        const WaitStamp& first = candidate[index];
        const WaitStamp& second = observed[index];
        if (first.cpu != second.cpu
            || first.lock != second.lock
            || first.owner_ref != second.owner_ref
            || first.wait_generation != second.wait_generation
            || first.owner_word != second.owner_word) {
            return WaitCheck::Changed;
        }
        const usize next = candidate[(index + 1) % count].cpu;
        if (OwnerWord::cpu(first.owner_word) != next) {
            return WaitCheck::Open;
        }
    }
    return WaitCheck::Stable;
}

} // namespace kernel::sync
