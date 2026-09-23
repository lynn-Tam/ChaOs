#pragma once

#include <diag/concurrency.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <sync/lock.hpp>
#include <operation/completion.hpp>
#include <operation/page_access.hpp>
#include <operation/vm_wait.hpp>
#include <ipc/channel_wait.hpp>
#include <resource/allocation.hpp>

namespace arch {
class TrapContext;
}

namespace kernel {

class CpuRegistry;

namespace sched {
class Binding;
}

namespace operation {

class Completion;

// One blocking edge owned by a kernel-managed continuation. Thread owns the
// base edge; an execution Frame may override it while an Activation is active.
// The operation still owns Completion and the dispatcher still owns run state.
class Wait final : private libk::noncopyable_nonmovable {
public:
    Wait() noexcept;
    ~Wait() noexcept;

    [[nodiscard]] auto prepare_channel(ipc::Channel& channel) noexcept -> ipc::ChannelWait*;
    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto observation_key() const noexcept
        -> diag::concurrency::ObservationKey;
    // Mutually exclusive syscall continuations share resident storage. A
    // release operation must not allocate the memory needed to wait for it.
    [[nodiscard]] auto page_access() noexcept -> PageAccess&;
    [[nodiscard]] auto find_page_access() noexcept -> PageAccess*;
    [[nodiscard]] auto prepare_revoke(cap::GrantGraph& graph) noexcept -> cap::GrantRevokeWait*;
    [[nodiscard]] auto prepare_close(cap::GrantGraph& graph) noexcept -> resource::CloseWait*;
    [[nodiscard]] auto prepare_vm(object::ObjectRef&& target, mm::VSpace& space) noexcept -> VmWait*;
    [[nodiscard]] auto begin(
        Completion& completion,
        CpuRegistry& cpus,
        sched::Binding& binding) noexcept -> bool;
    [[nodiscard]] auto finish(arch::TrapContext& trap) noexcept -> bool;
    [[nodiscard]] auto cancel() noexcept -> bool;

private:
    friend class Completion;

    [[nodiscard]] auto wake() noexcept
        -> diag::concurrency::ObservationKey;

    enum class EdgePhase : u8 {
        Detached,
        Attached,
        CancelOwned,
        FinishOwned,
    };

    Completion* completion_{};
    CpuRegistry* cpus_{};
    sched::Binding* binding_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Wait> lock_{};
    libk::Atomic<bool> ready_{};
    EdgePhase phase_{EdgePhase::Detached};
    enum class LocalKind : u8 { None, Page, Revoke, Close, Vm, Channel };
    LocalKind local_kind_{LocalKind::Page};
    union Local {
        PageAccess page;
        cap::GrantRevokeWait revoke;
        resource::CloseWait close;
        VmWait vm;
        ipc::ChannelWait channel;
        Local() noexcept : page{} {}
        ~Local() noexcept {}
    } local_;
    void reset_local() noexcept;
};

} // namespace operation
} // namespace kernel
