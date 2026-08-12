#pragma once

#include <cpu/topology.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <mm/addr.hpp>
#include <mm/page_state.hpp>
#include <mm/reclaim.hpp>
#include <mm/permissions.hpp>
#include <operation/completion.hpp>

namespace kernel::mm {
class MemoryObject;
class VSpace;
enum class FaultKind : u8;
/*luna change: remove the private VSpaceError dependency, reason: fault classification now crosses the shared mm boundary*/
}

namespace kernel::operation {

// One durable page-fault continuation. Wait owns this object as its only
// page-specific edge; MemoryObject and Pager index its embedded relation.
class PageFault final : private libk::noncopyable_nonmovable {
public:
    PageFault() noexcept;
    ~PageFault() noexcept;

    [[nodiscard]] auto completion() noexcept -> Completion& {
        return completion_;
    }
    [[nodiscard]] auto relation() noexcept -> mm::WaitRelation& {
        return relation_;
    }
    [[nodiscard]] auto active() const noexcept -> bool;
    /*luna change: expose the terminal retry result to the shared Thread trap policy, reason: resumed completion must consume and reset one PageFault without a second Thread state*/
    [[nodiscard]] auto terminal() const noexcept -> bool;
    [[nodiscard]] auto kind() const noexcept -> mm::FaultKind;
    [[nodiscard]] auto address() const noexcept -> mm::VirtAddr {
        return address_;
    }
    [[nodiscard]] auto access() const noexcept -> mm::Access {
        return access_;
    }
    [[nodiscard]] auto start(
        mm::VSpace& vspace,
        CpuRegistry& cpus,
        CpuId local,
        mm::VirtAddr address,
        mm::Access access) noexcept -> mm::FaultKind;
    [[nodiscard]] auto cancel() noexcept -> bool;
    void reset() noexcept;

private:
    enum class Phase : u8 {
        Idle,
        Attaching,
        Pending,
        Ready,
        Terminal,
        Canceled,
    };

    [[nodiscard]] auto complete() const noexcept -> bool;
    [[nodiscard]] auto read() noexcept -> Result;
    void release() noexcept;
    [[nodiscard]] auto resume(arch::TrapContext& trap) noexcept
        -> Completion::ResumeResult;
    static void publish(void* owner, mm::PageWaitResult result) noexcept;
    [[nodiscard]] auto admit() noexcept -> mm::FaultKind;
    void drop_pin() noexcept;

    mm::VSpace* vspace_{};
    CpuRegistry* cpus_{};
    CpuId local_{};
    mm::VirtAddr address_{};
    mm::Access access_{mm::Access::Read};
    libk::Atomic<mm::MemoryObject*> memory_{};
    libk::Atomic<u64> generation_{};
    /*luna change: keep the PageFault header independent of VSpace internals, reason: the continuation stores only the fault outcome byte and retries through the VSpace boundary*/
    /*luna change: zero-initialize the opaque fault outcome, reason: FaultKind remains a forward-declared boundary in this header and zero is NoMapping*/
    libk::Atomic<u8> kind_{};
    libk::Atomic<Phase> phase_{Phase::Idle};
    mm::WaitRelation relation_{};
    /*luna change: reserve one owner-fixed pressure payload beside PageFault,
      reason: a Pressure retry cannot reuse PageRequest storage before its
      relation is terminal and unlinked*/
    mm::FrameDemand demand_{};
    Completion completion_;
};

} // namespace kernel::operation
