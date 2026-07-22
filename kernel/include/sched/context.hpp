#pragma once

#include <cap/grant.hpp>
#include <cap/resolved.hpp>
#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/expected.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <object/object_ref.hpp>
#include <object/vproc_pool.hpp>
#include <sched/binding.hpp>
#include <sched/authority.hpp>
#include <sched/refill_queue.hpp>
#include <sched/types.hpp>
#include <time/time.hpp>
#include <execution/stop.hpp>

namespace kernel::sched {

class SchedulingDomain;
class CpuDispatcher;

class SchedulingContext final : private libk::noncopyable_nonmovable {
public:
    static constexpr usize max_refills = RefillQueue::max_capacity;

    struct Config final {
        time::Duration budget{};
        time::Duration period{};
        Urgency urgency{*Urgency::make(0)};
        usize refill_capacity{max_refills};
    };

    enum class Error : u8 {
        InvalidConfig,
        AlreadyAdmitted,
        NotAdmitted,
        AlreadyBound,
        NotBound,
        WrongCpu,
        Active,
        ArithmeticOverflow,
    };

    using Result = libk::Expected<void, Error>;

    [[nodiscard]] static constexpr auto valid_config(Config config) noexcept
        -> bool {
        return !config.budget.empty()
            && !config.period.empty()
            && config.budget <= config.period
            && config.refill_capacity != 0
            && config.refill_capacity <= max_refills;
    }

    SchedulingContext(Config config, time::Instant now) noexcept;
    ~SchedulingContext() noexcept;

    [[nodiscard]] auto config() const noexcept -> Config { return config_; }
    [[nodiscard]] auto urgency() const noexcept -> Urgency {
        return config_.urgency;
    }
    [[nodiscard]] auto eligible(time::Instant now) const noexcept -> bool;
    [[nodiscard]] auto available(time::Instant now) const noexcept
        -> time::Duration;
    [[nodiscard]] auto next_refill() const noexcept
        -> libk::optional<time::Instant>;
    [[nodiscard]] auto overrun() const noexcept -> time::Duration {
        return overrun_;
    }
    [[nodiscard]] auto activation_count() const noexcept -> u64 {
        return activation_count_;
    }
    [[nodiscard]] auto binding() noexcept -> Binding* {
        return binding_ ? &*binding_ : nullptr;
    }
    [[nodiscard]] auto binding() const noexcept -> const Binding* {
        return binding_ ? &*binding_ : nullptr;
    }
    [[nodiscard]] auto admitted() const noexcept -> bool {
        return domain_ != nullptr;
    }
    [[nodiscard]] auto home_cpu() const noexcept -> CpuId { return home_cpu_; }

    [[nodiscard]] auto admit(
        cap::Resolved<SchedulingDomain>& authority,
        CpuId home_cpu) noexcept -> Result;
    [[nodiscard]] auto bind(
        object::ObjectHold<Thread>&& target) noexcept -> Result;
    [[nodiscard]] auto bind(
        object::ObjectHold<Vproc>&& target) noexcept -> Result;
    [[nodiscard]] auto bind_authorized(
        object::ObjectHold<Thread>&& target,
        const cap::Resolved<SchedulingContext>& context,
        const cap::Resolved<Thread>& thread) noexcept -> Result;
    [[nodiscard]] auto bind_authorized(
        object::ObjectHold<Vproc>&& target,
        const cap::Resolved<SchedulingContext>& context,
        const cap::Resolved<Vproc>& vproc) noexcept -> Result;
    [[nodiscard]] auto unbind() noexcept -> Result;
    [[nodiscard]] auto prepare_retire() noexcept -> bool;
    [[nodiscard]] auto startable() const noexcept -> bool;

private:
    friend class SchedulingDomain;
    friend class CpuDispatcher;

    static void invalidate_domain(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void released_domain(void* context) noexcept;
    void invalidate_domain(cap::GrantWork&& work) noexcept;
    void finish_domain() noexcept;

    static const cap::GrantAttachmentOps domain_ops_;

    [[nodiscard]] auto activate(CpuId cpu) noexcept -> bool;
    void deactivate(CpuId cpu) noexcept;
    void charge(time::Instant now, time::Duration elapsed) noexcept;
    [[nodiscard]] auto bind_target(
        execution::TargetHold&& target) noexcept -> Result;
    [[nodiscard]] auto unbind(CpuDispatcher* owner) noexcept -> Result;

    Config config_{};
    RefillQueue refills_;
    mutable libk::TicketSpinLock authority_lock_{};
    libk::ManualLifetime<Binding> binding_{};
    libk::ManualLifetime<BindingAuthority> binding_authority_{};
    SchedulingDomain* domain_{};
    CpuId home_cpu_{};
    libk::optional<CpuId> active_cpu_{};
    time::Duration overrun_{};
    u64 activation_count_{};
    cap::GrantAttachment domain_authority_{this, domain_ops_};
    cap::GrantWork domain_work_{};
    execution::Stop domain_stop_{execution::Stop::Notifier::bind<
        &SchedulingContext::finish_domain>(*this)};
    bool withdrawing_{};
};

} // namespace kernel::sched
