#pragma once

#include <cap/grant.hpp>
#include <libk/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <atomic>
#include <sync.hpp>
#include <object/ref.hpp>
#include <libk/inplace_ring.hpp>
#include <sched/sched.hpp>
#include <task/thread.hpp>
#include <work.hpp>

namespace sched {

class Domain;
class Dispatcher;

class Sc final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto thread() noexcept -> Thread& { return owner_.get(); }
    [[nodiscard]] auto thread() const noexcept -> const Thread& { return owner_.get(); }
    [[nodiscard]] auto reference() const noexcept
        -> object::ref<Thread> {
        return owner_;
    }
    [[nodiscard]] auto queued() const noexcept -> bool {
        return ready_hook_.is_linked();
    }

    static constexpr usize max_refills = 8;

    struct Config final {
        u64 budget{};
        u64 period{};
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
    };

    using Result = std::expected<void, Error>;

    [[nodiscard]] static constexpr auto valid_config(Config config) noexcept
        -> bool {
        return config.budget != 0
            && config.period != 0
            && config.budget <= config.period
            && config.refill_capacity != 0
            && config.refill_capacity <= max_refills;
    }

    Sc(Config config, u64 now) noexcept;
    ~Sc() noexcept;

    [[nodiscard]] auto config() const noexcept -> Config { return config_; }
    [[nodiscard]] auto urgency() const noexcept -> Urgency {
        return config_.urgency;
    }
    [[nodiscard]] auto eligible(u64 now) const noexcept -> bool;
    [[nodiscard]] auto available(u64 now) const noexcept
        -> u64;
    [[nodiscard]] auto next_refill() const noexcept -> u64 {
        return refills_.front().ready_at;
    }
    // Home-CPU serialized, off the timer index; returns execution beyond budget.
    auto charge(u64 now, u64 elapsed) noexcept -> u64;
    [[nodiscard]] auto bound() const noexcept -> bool { return static_cast<bool>(owner_); }
    [[nodiscard]] auto admitted() const noexcept -> bool {
        return domain_ != nullptr;
    }
    [[nodiscard]] auto home_cpu() const noexcept -> CpuId { return home_cpu_; }

    [[nodiscard]] auto admit(
        cap::Resolved<Domain>& authority,
        CpuId home_cpu) noexcept -> Result;
    [[nodiscard]] auto bind(object::ref<Thread> target) noexcept -> Result;
    [[nodiscard]] auto bind_authorized(object::ref<Thread>,
        const cap::Resolved<Sc>&, const cap::Resolved<Thread>&) noexcept -> Result;
    // Transfer the target reference after all queue consumers have finished.
    // The caller retains it through stop callbacks and final target access.
    [[nodiscard]] auto unbind() noexcept
        -> std::expected<object::ref<>, Error>;
    void bind_work(WorkQueue& work) noexcept { work_ = &work; }
    void retire(object::cleanup&&) noexcept;
    [[nodiscard]] auto startable() const noexcept -> bool;

private:

    class auth final : private libk::noncopyable_nonmovable {
        friend class Sc;
        struct link final : private libk::noncopyable_nonmovable {
            link(auth& owner, const cap::GrantAttachmentOps& ops) noexcept
                : owner(&owner), attachment(this, ops) {}

            auth* owner{};
            cap::GrantAttachment attachment;
            cap::GrantWork work{};
            bool detaching{};
        };

    public:
        auth(Sc&, Thread&) noexcept;
        ~auth() noexcept;
        auto attach(const cap::Resolved<Sc>&,
                    const cap::Resolved<Thread>&) noexcept -> std::expected<void, cap::GrantError>;
        void release() noexcept;
        [[nodiscard]] auto reusable() const noexcept -> bool;

    private:
        void stopped() noexcept;
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
                // Publish under this lock: reusable() cannot destroy auth
                // before the last publisher has posted its completion.
                if (--owner_.publishers_ == 0) owner_.sc_->post_close();
            }
            flight(const flight&) = delete;
            auto operator=(const flight&) -> flight& = delete;
        private:
            auth& owner_;
        };
        static const cap::GrantAttachmentOps ops_;

        Sc* sc_{};
        Thread* target_{};
        mutable sync::Spin lock_{};
        link context_;
        link target_cap_;
        Stop stop_;
        usize publishers_{};
        bool attaching_{};
        bool revoking_{};
        bool stop_pending_{};
        bool ended_{};
    };

    friend class Domain;
    friend class Dispatcher;

    static void invalidate_domain(
        void* context,
        cap::GrantWork&& work) noexcept;
    static void released_domain(void* context) noexcept;
    void invalidate_domain(cap::GrantWork&& work) noexcept;
    void stopped() noexcept;
    void post_close() noexcept;
    void finish_close() noexcept;
    auto bind_locked(object::ref<Thread>) noexcept -> Result;

    static const cap::GrantAttachmentOps domain_ops_;

    [[nodiscard]] auto activate(CpuId cpu) noexcept -> bool;
    void deactivate(CpuId cpu) noexcept;
    void refill() noexcept;
    [[nodiscard]] auto unbind(Dispatcher* owner) noexcept
        -> std::expected<object::ref<>, Error>;
    [[nodiscard]] auto active() const noexcept -> bool {
        return active_cpu_.load(std::memory_order_acquire)
            != MaxCpus;
    }

    struct Refill { u64 ready_at; u64 amount; };
    Config config_{};
    libk::inplace_ring<Refill, max_refills> refills_{};
    mutable sync::Spin lock_{};
    object::ref<Thread> owner_{};
    libk::intrusive_list_hook ready_hook_{};
    Deadline refill_{Deadline::Fn::bind<&Sc::refill>(*this)};
    libk::intrusive_list_hook mail_hook_{};
    u8 actions_{}; // Protected by the home dispatcher's mail lock.
    // Queued or consumer-owned. External unbind must not release the binding
    // while a home consumer has removed the hook to execute its actions.
    std::atomic<bool> mailed_{};
    // Home-CPU-owned one-bit event credit. It closes the wake-before-block
    // race without allowing a remote producer to modify Thread state.
    bool wake_credit_{};
    libk::manual_lifetime<auth> auth_{};
    Domain* domain_{};
    CpuId home_cpu_{};
    std::atomic<usize> active_cpu_{MaxCpus};
    cap::GrantAttachment domain_cap_{this, domain_ops_};
    cap::GrantWork domain_work_{};
    Stop domain_stop_{Stop::Notifier::bind<
        &Sc::stopped>(*this)};
    bool withdrawing_{};
    bool stop_pending_{}; // Includes the Stop notifier publication, not just Thread exit.
    WorkQueue* work_{};
    Work close_work_{Work::Fn::bind<&Sc::finish_close>(*this)};
    object::cleanup cleanup_{};
};

} // namespace sched
