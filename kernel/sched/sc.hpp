#pragma once

#include <cap/grant.hpp>
#include <cpu/types.hpp>
#include <libk/inplace_ring.hpp>
#include <libk/manual_lifetime.hpp>
#include <sched/sched.hpp>
#include <task/thread.hpp>
#include <work.hpp>

namespace sched {

class Sc;

class Domain final : private libk::noncopyable_nonmovable {
public:
    enum class Error : u8 { NotAdmitted, InvalidCpu, Busy };
    using Result = std::expected<void, Error>;
    explicit Domain(usize count) noexcept : count_(count) { libk_assert(count && count <= MaxCpus); }
    auto admit(Sc&, CpuId) noexcept -> Result;
    auto unadmit(Sc&) noexcept -> Result;
    auto allows(CpuId cpu) const noexcept -> bool { return cpu.raw < count_; }
private:
    usize count_;
};

// One independently owned time resource. Only its binding's home charges it.
class Sc final : private libk::noncopyable_nonmovable {
public:
    static constexpr usize max_refills = 8;
    struct Config {
        u64 budget{}, period{}; // Both zero means uncapped.
        u64 cpus{~u64{0}};
        u32 weight{1024};
        usize refill_capacity{max_refills};
    };
    enum class Error : u8 { InvalidConfig, AlreadyAdmitted, NotAdmitted, AlreadyBound,
                           NotBound, WrongCpu, Active, Stale };
    using Result = std::expected<void, Error>;
    static constexpr auto valid_config(Config c) noexcept -> bool {
        return c.cpus && c.weight && c.weight <= 65535 && c.refill_capacity && c.refill_capacity <= max_refills
            && ((!c.budget && !c.period) || (c.budget && c.budget <= c.period));
    }
    Sc(Config, u64 now) noexcept;
    ~Sc() noexcept;
    auto config() const noexcept -> Config { sync::Lock guard{lock_}; return config_; }
    auto eligible(u64 now) const noexcept -> bool { return available(now) != 0; }
    auto available(u64 now) const noexcept -> u64;
    auto next_refill() const noexcept -> u64;
    auto charge(u64 now, u64 elapsed) noexcept -> u64;
    auto bound() const noexcept -> bool { sync::Lock guard{lock_}; return owner_ != nullptr; }
    auto admit(cap::Resolved<Domain>&, CpuId) noexcept -> Result;
    auto bind(object::ref<Sc>, Thread&) noexcept -> Result;
    auto bind_authorized(object::ref<Sc>, Thread&,
        const cap::Resolved<Sc>&, const cap::Resolved<Thread>&) noexcept -> Result;
    auto withdraw(u64 generation) noexcept -> Result;
    auto generation() const noexcept -> u64 { sync::Lock guard{lock_}; return generation_; }
    auto info(u64 now) const noexcept -> ScInfo;
    auto configure(Config, u64 now) noexcept -> Result;
    auto observe(ipc::Notification&, u64 badge) noexcept -> bool;
    void bind_work(WorkQueue& work) noexcept { work_ = &work; }
    void retire(object::cleanup&&) noexcept;

private:
    class auth final : private libk::noncopyable_nonmovable {
        friend class Sc;
        struct link {
            link(auth& owner) noexcept : owner(&owner), attachment(this, ops_) {}
            auth* owner;
            cap::GrantAttachment attachment;
            cap::GrantWork work{};
            bool detaching{};
        };
    public:
        explicit auth(Sc&) noexcept;
        ~auth() noexcept;
        auto attach(const cap::Resolved<Sc>&, const cap::Resolved<Thread>&) noexcept
            -> std::expected<void, cap::GrantError>;
        void release() noexcept;
        auto reusable() const noexcept -> bool;
    private:
        void invalidate(link&, cap::GrantWork&&) noexcept;
        void drain(link&) noexcept;
        class flight {
        public:
            explicit flight(auth& a) noexcept : owner_(a) {
                sync::Lock guard{a.lock_}; ++a.publishers_;
            }
            ~flight() noexcept {
                sync::Lock guard{owner_.lock_};
                if (--owner_.publishers_ == 0) owner_.sc_->post_close();
            }
        private:
            auth& owner_;
        };
        static const cap::GrantAttachmentOps ops_;
        Sc* sc_;
        mutable sync::Spin lock_{};
        link context_, target_cap_;
        usize publishers_{};
        bool attaching_{}, revoking_{}, ended_{};
    };
    friend class Domain;
    friend class Dispatcher;
    friend class ::Thread;
    static const cap::GrantAttachmentOps domain_ops_;
    void post_close() noexcept;
    void finish_close() noexcept;
    auto bind_locked(object::ref<Sc>, Thread&) noexcept -> Result;
    void release(Thread&) noexcept;
    void locate(Dispatcher&) noexcept;
    struct Refill { u64 ready_at, amount; };
    ipc::NotificationSource changed_{};
    Config config_;
    libk::inplace_ring<Refill, max_refills> refills_{};
    mutable sync::Spin lock_{};
    Thread* owner_{}; // Thread owns the SC reference; retirement drains this relation.
    libk::manual_lifetime<auth> auth_{};
    Domain* domain_{};
    Dispatcher* location_{};
    u64 service_{}, fraction_{}, debt_{}, generation_{}, applied_{}, runtime_{}, lag_{};
    cap::GrantAttachment domain_cap_{this, domain_ops_};
    cap::GrantWork domain_work_{};
    bool withdrawing_{}, attaching_{};
    WorkQueue* work_{};
    Work close_work_{Work::Fn::bind<&Sc::finish_close>(*this)};
    object::cleanup cleanup_{};
};

} // namespace sched
