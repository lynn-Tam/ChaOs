#pragma once

#include <core/types.hpp>

namespace kernel::pager {

class Pager;

// Fixed per-execution index of outstanding service claims, embedded in the
// claiming Thread/Vproc (the same owner-fixed pattern as WaitRelation and the
// terminal record). The claimer is the only writer of free entries; Pager
// transitions clear entries under the owning Pager lock with an exact
// identity match, so single-word stores keep the array consistent without a
// second lock domain. Two tickets bound a worker's pipelined claims while
// keeping the Vproc pool slot inside one page.
inline constexpr usize claims_per_execution = 2;

struct ClaimIndex final {
    struct Entry final {
        Pager* pager{};
        u16 slot{};
        u64 delivery_generation{};
        u64 claim_generation{};
    };

    Entry entries[claims_per_execution]{};

    [[nodiscard]] auto empty() const noexcept -> bool {
        for (const Entry& entry : entries) {
            if (entry.pager != nullptr) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto free_ticket() const noexcept -> usize {
        for (usize ticket = 0; ticket < claims_per_execution; ++ticket) {
            if (entries[ticket].pager == nullptr) {
                return ticket;
            }
        }
        return claims_per_execution;
    }

    void clear(usize ticket) noexcept {
        if (ticket < claims_per_execution) {
            entries[ticket] = Entry{};
        }
    }
};

} // namespace kernel::pager
