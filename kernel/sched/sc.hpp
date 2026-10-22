#pragma once
#include <mm/slab.hpp>

#include <bit>
#include <cap/grant.hpp>
#include <cpu/types.hpp>
#include <libk/inplace_ring.hpp>
#include <sched/sched.hpp>
#include <task/thread.hpp>
#include <work.hpp>

class Op;

namespace sched {

class Sc;

class Domain final : private libk::noncopyable_nonmovable {
public:
    static constexpr usize max_depth = 32;
    struct Config { u64 cpus{}, budget{}, period{}; u32 weight{1024}; };
    enum class Error : u8 { NotAdmitted, InvalidCpu, Busy };
    static auto valid_config(Config c) noexcept -> bool {
        return c.cpus && c.weight && c.weight <= 65535
            && c.budget <= static_cast<u64>(std::numeric_limits<i64>::max())
            && static_cast<unsigned __int128>(c.budget) <= static_cast<unsigned __int128>(std::popcount(c.cpus)) * c.period
            && ((!c.budget && !c.period) || (c.budget && c.period));
    }
    using Result = std::expected<void, Error>;
    struct Local {
        Local(Domain&, CpuId) noexcept;
        Domain& owner;
        CpuId cpu;
        Node node;
        Deadline refill;
        std::atomic<bool> active{};
        libk::intrusive_list_hook mail_hook{};
        object::ref<> ticket;
        bool mailed{};
        std::atomic<bool> stopped{}, waiting{};
        void wake() noexcept;
        static void run(void*) noexcept;
        Thread worker;
    };
    Domain(mm::Pmm&, Cpus&, Config) noexcept;
    ~Domain() noexcept;
    void bind_allocation(resource::Allocation& allocation) noexcept { quota_ = allocation.quota(); }
    template<class Transaction> auto reassign_usage(const object::ref<>& to, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        object::ref<> quota{to};
        return transaction([&](auto&& visit) { visit(index_fee_); locals_.charges(visit); }, [&] { std::swap(quota_, quota); });
    }
    auto open() noexcept -> status_t;
    auto post(Work&) noexcept -> status_t;
    auto attach(cap::Resolved<Domain>&) noexcept -> status_t;
    auto attach(object::ref<Domain>, const cap::GrantPin* = nullptr) noexcept -> status_t;
    auto close() noexcept -> u32;
    auto reparent(const cap::Resolved<Domain>& self, cap::Resolved<Domain>& parent) noexcept -> std::expected<u64, status_t>;
    auto configure(Config, Domain* parent) noexcept -> std::expected<u64, status_t>;
    void links(u64 cursor, LinkRecords& out) const noexcept {
        parent_binding_[0].links(out, LINK_PARENT, cursor); parent_binding_[1].links(out, LINK_PARENT, cursor);
    }
    auto info(u64 now) const noexcept -> DomainInfo;
    auto events() noexcept -> Source& { return events_; }
    void bind_work(WorkQueue& work, object::Key key) noexcept { work_ = &work; close_work_.bind(key); parent_binding_[0].bind(key); parent_binding_[1].bind(key); events_.bind_work(work, key); }
    void retire(object::cleanup&&) noexcept;
    auto admit(Sc&, CpuId) noexcept -> Result;
    auto unadmit(Sc&) noexcept -> Result;
    auto accepting() const noexcept -> bool {
        return phase_.load(std::memory_order_acquire) == DOMAIN_OPEN && object::ref<>::admitting(close_work_.key());
    }
    auto allows(CpuId cpu) const noexcept -> bool { sync::Lock guard{tree_lock_}; return allows_locked(cpu); }
    auto local(CpuId cpu) noexcept -> Local& { libk_assert(cpu.raw < count_ && nodes_[cpu.raw]); return *nodes_[cpu.raw]; }
private:
    friend class Sc;
    friend class ::Thread;
    friend class ::Op;
    friend class Dispatcher;
    friend struct Local;
    void parent_revoked() noexcept { static_cast<void>(close()); }
    inline static sync::Spin tree_lock_;
    auto allows_locked(CpuId cpu) const noexcept -> bool {
        libk_assert(tree_lock_.held());
        return cpu.raw < std::min(count_, usize{64}) && (config_.cpus & (u64{1} << cpu.raw));
    }
    auto admit_locked(Sc&, CpuId) noexcept -> Result;
    void post_close() noexcept;
    void wake_work(CpuId) noexcept;
    void finish_close() noexcept;
    auto finish_move() noexcept -> bool;
    auto lineage_open() const noexcept -> bool;
    auto lineage_admitting() const noexcept -> bool;
    auto depth() const noexcept -> usize;
    void notify(bool all = false) noexcept;
    WorkList ordinary_;
    mm::Pmm& pmm_;
    Cpus& cpus_;
    usize count_;
    inline static u64 tree_version_{1};
    std::atomic<usize> pending_mail_{};
    std::atomic<u64> runtime_{}, work_runtime_{};
    Config config_;
    std::optional<Config> pending_config_;
    u64 requested_{}, applied_{};
    status_t config_status_{};
    Budget budget_;
    std::atomic<u32> phase_{DOMAIN_OPEN};
    Domain* children_{}, *prev_{}, *next_{}, *closing_child_{};
    Sc* contexts_{}, *closing_sc_{};
    Op* ops_{}, *closing_op_{};
    using ParentBinding = cap::Bindings<1>;
    ParentBinding parent_binding_[2]{
        {ParentBinding::Action::bind<&Domain::parent_revoked>(*this), ParentBinding::Action::bind<&Domain::post_close>(*this)},
        {ParentBinding::Action::bind<&Domain::parent_revoked>(*this), ParentBinding::Action::bind<&Domain::post_close>(*this)}};
    usize parent_slot_{};
    object::ref<Domain> next_parent_, scan_;
    Domain* pending_children_{}, *closing_pending_{}, *pending_prev_{}, *pending_next_{};
    u64 scan_version_{};
    std::atomic<bool> relocating_{};
    void unstage() noexcept;
    Src<DOMAIN_CLOSING | DOMAIN_DRAINED | DOMAIN_CONFIGURING> events_{Source::Drained::bind<&Domain::post_close>(*this)};
    WorkQueue* work_{};
    Work close_work_{Work::Fn::bind<&Domain::finish_close>(*this)};
    object::cleanup cleanup_{};
    object::ref<> quota_;
    object::ref<Domain> parent_;
    mm::Slab<Local, false, false> locals_;
    resource::Usage index_fee_;
    mm::OwnedPage index_;
    std::span<Local*> nodes_;
};

// One independently owned time resource. Only its binding's home charges it.
class Sc final : private libk::noncopyable_nonmovable {
public:
    static constexpr bool fixed_usage = true;
    static constexpr usize max_refills = Budget::max_refills;
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
        return c.budget <= static_cast<u64>(std::numeric_limits<i64>::max()) && c.cpus && c.weight && c.weight <= 65535 && c.refill_capacity && c.refill_capacity <= max_refills
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
    auto bind(object::ref<Sc>, Thread&,
        const cap::Resolved<Sc>&, const cap::Resolved<Thread>&) noexcept -> Result;
    auto withdraw(u64 generation) noexcept -> Result;
    auto generation() const noexcept -> u64 { sync::Lock guard{lock_}; return generation_; }
    void links(u64 cursor, LinkRecords& out) const noexcept {
        domain_binding_.links(out, LINK_CPU, cursor); binding_.links(out, LINK_BIND, cursor);
    }
    auto info(u64 now) const noexcept -> ScInfo;
    auto configure(Config, u64 now) noexcept -> Result;
    auto events() noexcept -> Source& { return events_; }
    void state_changed() noexcept;
    void bind_work(WorkQueue& work, object::Key owner) noexcept { work_ = &work; close_work_.bind(owner); binding_.bind(owner); domain_binding_.bind(owner); events_.bind_work(work, owner); }
    void retire(object::cleanup&&) noexcept;

private:
    friend class Domain;
    friend class Dispatcher;
    friend class ::Thread;
    void domain_revoked() noexcept;
    void post_close() noexcept;
    void finish_close() noexcept;
    auto bind_locked(object::ref<Sc>, Thread&) noexcept -> Result;
    void release(Thread&) noexcept;
    auto drain_binding() noexcept -> bool;
    void locate(Dispatcher&) noexcept;
    void record(u64 elapsed) noexcept;
    Src<SC_BOUND | SC_WITHDRAWING> events_{Source::Drained::bind<&Sc::post_close>(*this)};
    Config config_;
    Budget budget_;
    mutable sync::Spin lock_{};
    Thread* owner_{}; // Thread owns the SC reference; retirement drains this relation.
    cap::Bindings<2> binding_{cap::Bindings<2>::Action::bind<&Sc::post_close>(*this),
        cap::Bindings<2>::Action::bind<&Sc::post_close>(*this)};
    Domain* domain_{};
    Sc* prev_{}, *next_{};
    Dispatcher* location_{};
    u64 service_{}, fraction_{}, generation_{}, applied_{}, runtime_{}, lag_{};
    cap::Bindings<1> domain_binding_{cap::Bindings<1>::Action::bind<&Sc::domain_revoked>(*this),
        cap::Bindings<1>::Action::bind<&Sc::post_close>(*this)};
    bool withdrawing_{}, releasing_{};
    WorkQueue* work_{};
    Work close_work_{Work::Fn::bind<&Sc::finish_close>(*this)};
    object::cleanup cleanup_{};
};

} // namespace sched
