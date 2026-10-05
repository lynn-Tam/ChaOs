#pragma once

#include <cap/grant.hpp>
#include <base/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/intrusive_tree.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <optional>
#include <libk/sync/atomic.hpp>
#include <sync.hpp>
#include <object/ref.hpp>
#include <sched/remote_queue.hpp>
#include <sched/refill_queue.hpp>
#include <sched/types.hpp>
#include <time/time.hpp>
#include <task/thread.hpp>

namespace sched {

class Domain;
class Dispatcher;

class Sc final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto thread() noexcept -> Thread& { return owner_.get(); }
    [[nodiscard]] auto thread() const noexcept -> const Thread& { return owner_.get(); }
    [[nodiscard]] auto reference() const noexcept
        -> std::expected<object::ref<>, object::error> {
        return owner_.erase();
    }
    [[nodiscard]] auto queued() const noexcept -> bool {
        return ready_hook_.is_linked();
    }
    [[nodiscard]] auto timer_queued() const noexcept -> bool {
        return timer_hook_.is_linked();
    }

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

    using Result = std::expected<void, Error>;

    [[nodiscard]] static constexpr auto valid_config(Config config) noexcept
        -> bool {
        return !config.budget.empty()
            && !config.period.empty()
            && config.budget <= config.period
            && config.refill_capacity != 0
            && config.refill_capacity <= max_refills;
    }

    Sc(Config config, time::Instant now) noexcept;
    ~Sc() noexcept;

    [[nodiscard]] auto config() const noexcept -> Config { return config_; }
    [[nodiscard]] auto urgency() const noexcept -> Urgency {
        return config_.urgency;
    }
    [[nodiscard]] auto eligible(time::Instant now) const noexcept -> bool;
    [[nodiscard]] auto available(time::Instant now) const noexcept
        -> time::Duration;
    [[nodiscard]] auto next_refill() const noexcept
        -> std::optional<time::Instant>;
    [[nodiscard]] auto overrun() const noexcept -> time::Duration {
        return overrun_;
    }
    [[nodiscard]] auto activation_count() const noexcept -> u64 {
        return activation_count_;
    }
    [[nodiscard]] auto bound() const noexcept -> bool { return static_cast<bool>(owner_); }
    [[nodiscard]] auto admitted() const noexcept -> bool {
        return domain_ != nullptr;
    }
    [[nodiscard]] auto home_cpu() const noexcept -> CpuId { return home_cpu_; }

    [[nodiscard]] auto admit(
        cap::Resolved<Domain>& authority,
        CpuId home_cpu) noexcept -> Result;
    [[nodiscard]] auto bind(object::ref<Thread>&& target) noexcept -> Result;
    [[nodiscard]] auto bind_authorized(object::ref<Thread>&&,
        const cap::Resolved<Sc>&, const cap::Resolved<Thread>&) noexcept -> Result;
    // Transfer the target reference after all queue consumers have finished.
    // The caller retains it through stop callbacks and final target access.
    [[nodiscard]] auto unbind() noexcept
        -> std::expected<object::ref<>, Error>;
    [[nodiscard]] auto prepare_retire() noexcept -> bool;
    [[nodiscard]] auto startable() const noexcept -> bool;

private:
    friend class ReadyQueue;
    friend class TimerQueue;
    friend class RemoteQueue;

    class auth final : private libk::noncopyable_nonmovable {
        struct link final : private libk::noncopyable_nonmovable {
            link(auth& owner, const cap::GrantAttachmentOps& ops) noexcept
                : owner(&owner), attachment(this, ops) {}

            auth* owner{};
            cap::GrantAttachment attachment;
            cap::GrantWork work{};
            bool detaching{};
        };

    public:
        explicit auth(Thread&) noexcept;
        ~auth() noexcept;
            auto attach(const cap::Resolved<Sc>&,
                    const cap::Resolved<Thread>&) noexcept -> std::expected<void, cap::GrantError>;
        void release() noexcept;
        [[nodiscard]] auto reusable() const noexcept -> bool;

    private:
        void invalidate(link& link, cap::GrantWork&& work) noexcept;
        void drain(link& link) noexcept;
        class flight {
        public:
            explicit flight(auth& owner) noexcept : owner_(owner) {
                sync::Lock guard{owner_.lock_};
                ++owner_.publishers_;
            }
            ~flight() noexcept {
                sync::Lock guard{owner_.lock_};
                --owner_.publishers_;
            }
            flight(const flight&) = delete;
            auto operator=(const flight&) -> flight& = delete;
        private:
            auth& owner_;
        };
        static const cap::GrantAttachmentOps ops_;

        Thread* target_{};
        mutable sync::Spin
            lock_{};
        link context_;
        link target_cap_;
        Stop stop_;
        usize publishers_{};
        bool attaching_{};
        bool revoking_{};
        bool ended_{};
    };

    friend class Domain;
    friend class Dispatcher;

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
    [[nodiscard]] auto unbind(Dispatcher* owner) noexcept
        -> std::expected<object::ref<>, Error>;
    [[nodiscard]] auto active() const noexcept -> bool {
        return active_cpu_.load<libk::MemoryOrder::Acquire>()
            != MaxCpus;
    }

    Config config_{};
    RefillQueue refills_;
    mutable sync::Spin
        authority_lock_{};
    object::ref<Thread> owner_{};
    libk::IntrusiveListHook ready_hook_{};
    libk::IntrusiveTreeHook timer_hook_{};
    time::Instant timer_deadline_{};
    RemoteRequest start_{RemoteKind::Start, this};
    RemoteRequest wake_{RemoteKind::Wake, this};
    RemoteRequest stop_{RemoteKind::Stop, this};
    // Home-CPU-owned one-bit event credit. It closes the wake-before-block
    // race without allowing a remote producer to modify Thread state.
    bool wake_credit_{};
    libk::ManualLifetime<auth> auth_{};
    Domain* domain_{};
    CpuId home_cpu_{};
    libk::Atomic<usize> active_cpu_{MaxCpus};
    time::Duration overrun_{};
    u64 activation_count_{};
    cap::GrantAttachment domain_authority_{this, domain_ops_};
    cap::GrantWork domain_work_{};
    Stop domain_stop_{Stop::Notifier::bind<
        &Sc::finish_domain>(*this)};
    bool withdrawing_{};
};

} // namespace sched
