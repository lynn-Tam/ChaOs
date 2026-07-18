#pragma once

#include <cap/grant.hpp>
#include <libk/noncopyable.hpp>
#include <object/object_ref.hpp>
#include <sync/completion.hpp>
#include <execution/stop.hpp>
#include <operation/completion.hpp>

namespace kernel::cap {
class GrantGraph;
}

namespace kernel::resource {

class ResourcePool;
class CloseWait;

enum class AllocationState : u8 {
    Empty,
    Pending,
    Live,
    Revoking,
    Revoked,
    Stopping,
    Stopped,
    Retiring,
};

// Embedded in the pool-controlled Grant root. Sponsorship accounts for the
// resource; Allocation is the independent authority relation used to revoke
// all derived capabilities and retire the target during strong close.
class Allocation final : private libk::noncopyable_nonmovable {
public:
    Allocation() noexcept;
    ~Allocation() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return state_ != AllocationState::Empty;
    }

private:
    friend class ResourcePool;
    friend class kernel::cap::GrantGraph;
    friend class AllocationTxn;

    void ready() noexcept;
    void target_ready() noexcept;
    void child_closed() noexcept;

    ResourcePool* pool_{};
    kernel::cap::GrantGraph* graph_{};
    kernel::cap::GrantKey root_{};
    kernel::object::ObjectRef target_{};
    kernel::cap::GrantRevoke revoke_{};
    execution::Stop stop_;
    Allocation* previous_{};
    Allocation* next_{};
    AllocationState state_{AllocationState::Empty};
    bool independent_close_{};
};

// Stable operation storage for a blocking child-pool close. The operation is
// completed by the sponsored child object's exact refund edge, after its
// ObjectPool slot and delegated budget are reusable by the parent.
class CloseWait final : private libk::noncopyable_nonmovable {
public:
    explicit CloseWait(kernel::cap::GrantGraph& graph) noexcept;
    ~CloseWait() noexcept = default;

    [[nodiscard]] auto complete() const noexcept -> bool {
        return completion_.complete();
    }
    [[nodiscard]] auto arm() noexcept -> bool { return completion_.arm(); }
    void commit() noexcept { committed_ = true; }
    [[nodiscard]] auto relation() noexcept -> kernel::operation::Completion& {
        return relation_;
    }
    [[nodiscard]] auto notifier() noexcept -> RefundNotifier;

private:
    friend class kernel::cap::GrantGraph;

    void refunded() noexcept;
    [[nodiscard]] auto read() noexcept -> kernel::operation::Result;
    void release() noexcept;
    [[nodiscard]] auto cancel() noexcept -> bool;

    kernel::cap::GrantGraph* graph_{};
    kernel::sync::Completion completion_;
    kernel::operation::Completion relation_;
    bool committed_{};
};

// Construction guard for an allocation root that is already visible to its
// ResourcePool but has not yet published a user capability. Dropping it
// revokes the unpublished lineage and retires the object through the normal
// object lifecycle.
class AllocationTxn final : private libk::noncopyable {
public:
    AllocationTxn() noexcept = default;
    AllocationTxn(AllocationTxn&& other) noexcept;
    auto operator=(AllocationTxn&& other) noexcept -> AllocationTxn&;
    ~AllocationTxn() noexcept;

    [[nodiscard]] auto acquire() const noexcept
        -> libk::Expected<kernel::cap::GrantLease, kernel::cap::GrantError>;
    void commit() noexcept;
    void reset() noexcept;

private:
    friend class kernel::cap::GrantGraph;

    AllocationTxn(
        kernel::cap::GrantGraph& graph,
        Allocation& allocation,
        kernel::cap::GrantRef&& root) noexcept
        : graph_(&graph), allocation_(&allocation), root_(libk::move(root)) {}

    kernel::cap::GrantGraph* graph_{};
    Allocation* allocation_{};
    kernel::cap::GrantRef root_{};
};

} // namespace kernel::resource
