#pragma once

#include <expected>
#include <optional>


#include <arch/trap.hpp>
#include <arch/user.hpp>
#include <cap/grant.hpp>
#include <base/types.hpp>
#include <task/env.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <sync.hpp>
#include <mm/kspace.hpp>
#include <mm/mem.hpp>
#include <mm/pmm.hpp>
#include <mm/vspace.hpp>
#include <object/ref.hpp>
#include <wait.hpp>
#include <ipc/transfer.hpp>
#include <resource/sponsorship.hpp>
#include <sched/types.hpp>
#include <sched/queues.hpp>
#include <time/time.hpp>
#include <uapi/endpoint.h>

class CpuRegistry;
class Thread;

namespace sched {
class Dispatcher;
}

namespace ipc {

enum class EndpointError : u8 {
    Closed,
    Busy,
    InvalidConfig,
    InvalidCaller,
    DepthExceeded,
    GenerationExhausted,
    QueueFull,
    BudgetTooLow,
    Denied,
    TransferFailed,
};

struct EndpointConfig final {
    arch::UserStart entry{};
    usize capacity{};
    usize call_capacity{};
    usize max_depth{};
    time::Duration budget_floor{};
    sched::Urgency urgency_ceiling{*sched::Urgency::make(0)};
};

using CodePages = libk::InplaceVector<
    mm::PageHold, MYOS_ENDPOINT_MAX_CODE_PAGES>;
using StackPages = libk::InplaceVector<
    mm::PageHold, MYOS_ENDPOINT_MAX_STACK_PAGES>;

class Endpoint;
class Call;

// Stable Endpoint-owned slot:
// The active callee stack defines reply ownership and lifetime.
class Activation final : private libk::noncopyable_nonmovable {
public:
    Activation(
        Endpoint& endpoint,
        resource::Charge&& stack_charge,
        mm::Stack&& kernel_stack,
        mm::View&& user_stack,
        std::optional<Buffer>&& ipc,
        StackPages&& resident,
        mm::Virt user_stack_top) noexcept;
    ~Activation() noexcept;

    [[nodiscard]] auto stack() noexcept -> mm::Stack& { return kernel_stack_; }
    [[nodiscard]] auto env() noexcept -> Env&;
    [[nodiscard]] auto ipc_buffer() noexcept -> Buffer* { return ipc_ ? &*ipc_ : nullptr; }
    [[nodiscard]] auto wait() noexcept -> Wait& { return wait_; }
    [[nodiscard]] auto previous() const noexcept -> Activation* { return previous_; }
    [[nodiscard]] auto cancel_pending() const noexcept -> bool;
    void unwind(arch::TrapContext&, sched::Dispatcher&, isize) noexcept;
    void release() noexcept;
    [[nodiscard]] auto endpoint() const noexcept -> Endpoint& {
        return *endpoint_;
    }

private:
    friend class Endpoint;
    friend class ::Thread;

    Endpoint* endpoint_{};
    resource::Charge stack_charge_{};
    mm::Stack kernel_stack_;
    mm::View user_stack_;
    std::optional<Buffer> ipc_{};
    StackPages resident_{};
    Wait wait_{};
    Activation* previous_{};
    Call* call_{};
    mm::Virt user_stack_top_{};

};

// Endpoint-owned call truth. It survives queueing, admission and activation;
// Activation contributes only the executable stack/root slot.
class Call final : private libk::noncopyable_nonmovable {
public:
    enum class State : u8 {
        Free,
        Preparing,
        Queued,
        Ready,
        Committing,
        Active,
        Replying,
        Canceling,
        Reaping,
        Complete,
    };

    explicit Call(Endpoint& endpoint) noexcept;
    ~Call() noexcept;

private:
    friend class Endpoint;
    friend class Activation;

    void release() noexcept;
    [[nodiscard]] auto cancel() noexcept -> bool;
    static void revoke(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void grant_done(void* context) noexcept;
    void expire() noexcept;

    static const cap::GrantAttachmentOps grant_ops_;

    Endpoint* endpoint_{};
    Completion completion_;
    sched::Deadline deadline_;
    libk::ManualLifetime<cap::GrantAttachment> grant_{};
    object::ref<Thread> caller_{};
    arch::UserFrame caller_frame_{};
    Activation* activation_{};
    usize arguments_[3]{};
    WaitResult result_{};
    u64 generation_{};
    usize urgency_{};
    usize badge_{};
    usize receive_limit_{};
    Transfer::Specs request_caps_{};
    Transfer::Handles installed_caps_{};
    Transfer transfer_{};
    isize cancel_status_{MYOS_STATUS_CANCELED};
    CpuRegistry* cpus_{};
    usize publishers_{};
    bool cancel_pending_{};
    State state_{State::Free};
};

// Immutable protected-procedure entry plus a fixed, sponsor-paid Activation
// pool. The Endpoint is the sole owner of service roots, code relation,
// admission state and every callee stack.
class Endpoint final : private libk::noncopyable_nonmovable {
    friend class Activation;
public:
    static constexpr usize max_activations = MYOS_ENDPOINT_MAX_ACTIVATIONS;

    Endpoint(
        mm::Pmm& pmm,
        Env&& service,
        mm::View&& code,
        CodePages&& resident_code,
        EndpointConfig config) noexcept;
    ~Endpoint() noexcept;

    [[nodiscard]] auto add_activation(
        resource::Charge&& stack_charge,
        mm::Stack&& kernel_stack,
        mm::View&& user_stack,
        std::optional<Buffer>&& ipc,
        StackPages&& resident,
        mm::Virt user_stack_top) noexcept
        -> std::expected<void, EndpointError>;
    [[nodiscard]] auto add_call() noexcept
        -> std::expected<void, EndpointError>;
    [[nodiscard]] auto open() noexcept -> std::expected<void, EndpointError>;
    [[nodiscard]] auto call(
        cap::Resolved<Endpoint>&& view,
        Thread& caller,
        arch::TrapContext& trap,
        sched::Dispatcher& dispatcher,
        CpuRegistry& cpus,
        const usize (&arguments)[3],
        std::optional<time::Instant> deadline) noexcept
        -> std::expected<void, EndpointError>;
    [[nodiscard]] auto reply(
        Thread& caller,
        arch::TrapContext& trap,
        sched::Dispatcher& dispatcher,
        isize status,
        usize value) noexcept -> std::expected<void, EndpointError>;
    [[nodiscard]] auto abort(
        Thread& caller,
        arch::TrapContext& trap,
        sched::Dispatcher& dispatcher,
        isize status) noexcept -> std::expected<void, EndpointError>;

    void close() noexcept;
    void retire(object::cleanup&& cleanup) noexcept;
    void bind_sponsor(resource::Sponsorship& sponsor) noexcept;

private:
    friend class Call;
    friend class Activation;

    enum class State : u8 {
        Constructing,
        Open,
        Draining,
        Closed,
    };

    [[nodiscard]] static auto hold(Thread& thread) noexcept
        -> std::expected<object::ref<Thread>, EndpointError>;
    [[nodiscard]] auto depth(const Thread& thread) const noexcept -> usize;
    void release_call(Call& call) noexcept;
    [[nodiscard]] auto cancel_call(Call& call) noexcept -> bool;
    void invalidate_call(Call& call) noexcept;
    void expire_call(Call& call) noexcept;
    void grant_drained(Call& call) noexcept;
    void publish_cancel(Call& call) noexcept;
    void publisher_done(Call& call) noexcept;
    void finish_reap(Call& call) noexcept;
    void publish_ready(Call& call) noexcept;
    [[nodiscard]] auto enter(
        Call& call,
        arch::TrapContext& trap,
        sched::Dispatcher& dispatcher) noexcept -> bool;
    [[nodiscard]] auto snapshot_caps(
        const Buffer* buffer,
        usize limit,
        Transfer::Specs& specs,
        usize& receive_limit) noexcept -> bool;
    [[nodiscard]] auto commit_caps(
        Transfer& transfer,
        cap::CSpace& source,
        cap::CSpace& destination,
        const Transfer::Specs& specs,
        Buffer* receiver,
        Transfer::Handles& installed) noexcept -> bool;
    void close_installed(Call& call) noexcept;
    [[nodiscard]] auto finish_active(
        Activation& activation,
        arch::TrapContext& trap,
        sched::Dispatcher& dispatcher,
        isize status,
        usize value,
        bool reply) noexcept -> bool;
    [[nodiscard]] auto next_call_locked(Activation& slot) noexcept -> Call*;
    void reset_call_locked(Call& call) noexcept;
    void try_finish_retire() noexcept;

    mutable sync::Spin lock_{};
    Env service_;
    mm::View code_;
    CodePages resident_code_{};
    EndpointConfig config_{};
    mm::Slab<Activation, false> activations_;
    mm::Slab<Call, false> calls_;
    Activation* slots_[max_activations]{};
    Call* call_slots_[MYOS_ENDPOINT_MAX_CALLS]{};
    object::cleanup cleanup_{};
    usize slot_count_{};
    usize call_count_{};
    usize outstanding_{};
    u64 generation_{};
    State state_{State::Constructing};
};

} // namespace ipc
