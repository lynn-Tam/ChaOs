#pragma once

#include <cap/grant.hpp>
#include <mm/pmm.hpp>
#include <uapi/ipc.h>
#include <wait.hpp>
#include <work.hpp>

namespace ipc {

// Receiver-owned prepaid subscriptions and coalesced hints. Results and
// source facts stay with their providers; no observing target Ref is kept.
class Port final : private libk::noncopyable_nonmovable {
public:
    struct Bound { object::Scope* scope{}; object::Id object{}; cap::ScopeLimit view{}; };
private:
    struct Watch {
        enum class State : u8 { Free, Prepared, Live, Closing, Terminal };
        explicit Watch(Port&) noexcept;
        void deliver(WaitResult) noexcept;
        void drained() noexcept;
        Port& owner;
        libk::intrusive_list_hook hook, queue;
        libk::intrusive_tree_hook index;
        cap::Attachment authority;
        object::LinkWork work;
        Source::Watch source;
        PortEvent event{};
        Source::Condition condition{};
        Bound bound{};
        State state{};
        usize calls{};
        u64 pending{};
        bool source_done{}, detach_started{}, grant_done{}, erase{}, finishing{};
    };
    using Watches = libk::intrusive_list<Watch, &Watch::hook>;
    using Queue = libk::intrusive_list<Watch, &Watch::queue>;
    struct Compare {
        bool operator()(const Watch& a, const Watch& b) const noexcept { return a.event.watch_gen < b.event.watch_gen; }
        bool operator()(u64 a, const Watch& b) const noexcept { return a < b.event.watch_gen; }
        bool operator()(const Watch& a, u64 b) const noexcept { return a.event.watch_gen < b; }
    };
public:
    struct Config { usize watches, queue; };
    Port(mm::Pmm&, Config) noexcept;
    ~Port() noexcept;
    static auto storage_bytes(Config) noexcept -> std::optional<usize>;
    auto open() noexcept -> status_t;
    void bind_work(WorkQueue& work, object::Key key) noexcept { work_ = &work; job_.bind(key); ready_.bind_work(work, key); }
    void bind_funding(resource::Funding& funding) noexcept { payer_ = funding.payer(); }
    template<class Transaction> auto billing(const object::ref<>& to, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        { sync::Lock guard{lock_}; if (!opened_) return std::unexpected(STATUS_BUSY); }
        object::ref<> payer{to};
        return transaction([&](auto&& visit) { storage_.charges(visit); }, [&] { std::swap(payer_, payer); });
    }
    auto events() noexcept -> Source& { return ready_; }
    auto watch(cap::Resolved<Port>&, const cap::GrantLease&, Source&, u32 facet, u64 key,
               Source::Condition, Bound) noexcept -> std::expected<u64, status_t>;
    auto arm(u64 id, u64 seen) noexcept -> status_t;
    auto unwatch(u64 id) noexcept -> status_t;
    auto take(PortEvent&) noexcept -> status_t;
    auto records(u64 epoch, u64 last, PortRecords&) const noexcept -> status_t;
    auto resync(u64 epoch) noexcept -> status_t;
    void retire(object::cleanup&&) noexcept;
private:
    class Flight {
        Port& owner_;
    public:
        explicit Flight(Port& owner) noexcept : owner_(owner) { sync::Lock guard{owner.lock_}; ++owner.calls_; }
        ~Flight() noexcept { owner_.finish_call(true); }
    };
    static const cap::AttachOps authority_ops_;
    void close(Watch&, bool erase, object::LinkWork = {}) noexcept;
    void finish(Watch&) noexcept;
    auto emit(Watch&, WaitResult) noexcept -> bool;
    void complete() noexcept { finish_call(false); }
    void finish_call(bool release) noexcept;
    void run_work() noexcept;
    void changed() noexcept { if (version_ != UINT64_MAX) ++version_; }
    auto find(u64) noexcept -> Watch*;
    auto update() noexcept -> Source::Publication;
    Config config_;
    object::ref<> payer_;
    mm::Slab<Watch, false> storage_;
    Watches watches_;
    Queue queue_, free_;
    libk::intrusive_tree<Watch, &Watch::index, Compare> index_;
    mutable sync::Spin lock_;
    Src<PORT_READABLE | PORT_OVERFLOW> ready_{Source::Drained::bind<&Port::complete>(*this)};
    u64 version_{1}, acknowledged_{}, scan_{}, readiness_{};
    usize calls_{};
    bool opened_{}, closing_{}, overflow_{}, scanned_{};
    object::cleanup cleanup_;
    WorkQueue* work_{};
    Work job_{Work::Fn::bind<&Port::run_work>(*this)};
};

} // namespace ipc
