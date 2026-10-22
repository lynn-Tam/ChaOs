#pragma once

#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <atomic>
#include <uapi/ipc.h>
#include <sync.hpp>
#include <sched/sched.hpp>
#include <libk/intrusive_list.hpp>
#include <optional>
#include <utility>
#include <expected>
#include <array>
#include <bit>
#include <object/ref.hpp>

class Cpus;
class Thread;
class WorkQueue;

class Wait;

struct WaitResult final {
    status_t status{STATUS_OK};
    usize value{};
    u64 sequence{};
};

// Facts and registrations share one boundary. Notifications consume bits;
// state sources leave facts intact and compare a caller's observed version.
class Source : private libk::noncopyable_nonmovable {
public:
    class Publication;
    using Drained = libk::delegate<void() noexcept>;
    using Action = libk::delegate<void() noexcept>;
    using Pin = libk::delegate<void(bool) noexcept>;
    // Installation is atomic with source registration. The returned action
    // drains authority; ready may run only after that drain actually finishes.
    using Commit = libk::delegate<void(Action drain) noexcept>;
    using Admission = libk::delegate<status_t(Commit commit, Action revoke, Action ready) noexcept>;
    struct Condition {
        u64 mask{~u64{}};
        u64 seen{};
        bool changed{};
    };
    class Watch final : private libk::noncopyable_nonmovable {
        friend class Source;
        static constexpr usize registered = usize{1} << (sizeof(usize) * 8 - 1);
        Source* source_{};
        libk::intrusive_list_hook hook_, publish_hook_;
        Condition condition_{};
        WaitResult pending_{};
        std::atomic<usize> state_{};
        bool armed_{};
        u64 armed_at_{}, scanned_{};
        libk::delegate<void(WaitResult) noexcept> deliver_;
        Drained drained_;
        Pin pin_;
        void release() noexcept;
    public:
        Watch(libk::delegate<void(WaitResult) noexcept> deliver, Drained drained) noexcept
            : deliver_(deliver), drained_(drained) {}
        ~Watch() noexcept;
        // Owner serializes detach and retains its weak authority Link through
        // the drained callback. Arm holds a transient target storage use.
        void detach() noexcept;
        auto attached() const noexcept -> bool { return state_.load(std::memory_order_acquire) & registered; }
        auto source() const noexcept -> Source* { return source_; }
    };
    explicit Source(Drained drained = {}) noexcept : drained_(drained) {}
    void bind_work(WorkQueue& work, object::Key key, Drained drained = {}, Pin pin = {}) noexcept {
        work_ = &work; key_ = key; pin_ = pin; if (drained) drained_ = drained;
    }
    ~Source() noexcept;
    // A caller already owning a registration may borrow its metadata hold.
    auto retain() noexcept -> Pin { auto pin = pin_; if (pin) pin(true); return pin; }
    auto post(u64 bits) noexcept -> bool;
    auto update(u64 bits) noexcept -> bool;
    // Claim delivery under a provider's lock, publish after releasing it.
    auto prepare(u64 bits, bool replace = false) noexcept -> Publication;
    template<class Install> auto watch(Watch& watch, Condition condition, Install&& install) noexcept -> status_t;
    auto arm(Watch&, Condition) noexcept -> status_t;
    template<class Install> auto arm(Watch&, Condition, Install&&) noexcept -> status_t;
    auto take(u64 mask = ~u64{}) noexcept -> WaitResult;
    auto snapshot() const noexcept -> WaitResult;
    // Pending means the Thread's prepaid record is armed. No borrowed stack
    // storage or future user pointer is retained by this source.
    struct Context { Thread& thread; Cpus& cpus; sched::Dispatcher& dispatcher; };
    auto await(Context, Condition, std::optional<u64> deadline = {},
               Admission admission = {}, Action release = {}) noexcept -> WaitResult;
    auto registrations() const noexcept -> usize { sync::Lock guard{lock_}; return registrations_; }
    void close() noexcept;
    auto drained() const noexcept -> bool;
    auto idle() const noexcept -> bool { sync::Lock guard{lock_}; return !registrations_ && !publishers_ && watches_.empty(); }
private:
    friend class Wait;
    friend class WorkQueue;
    void schedule() noexcept;
    auto scan(Pin&) noexcept -> bool;
    auto observed(Condition, u64) const noexcept -> std::optional<WaitResult>;
    auto closed_result(const Watch&) const noexcept -> WaitResult;
protected:
    void history(std::span<u64> values, u64 mask) noexcept { raised_ = values.data(); mask_ = mask; }
private:
    class Registration final : private libk::noncopyable_nonmovable {
    public:
        explicit Registration(Wait&) noexcept;
        ~Registration() noexcept;
    private:
        friend class Source;
        friend class Wait;
        friend class sched::Dispatcher;
        void release() noexcept;
        void cancel() noexcept;
        void expire() noexcept;
        void revoke() noexcept;
        void publish() noexcept;
        void complete() noexcept;
        Action drain_, release_;
        auto deliver(WaitResult) noexcept -> bool; // Source lock held.
        Source* source_{};
        bool queued_{};
        u64 armed_at_{}, scanned_{};
        libk::intrusive_list_hook hook_{};
        Condition condition_{};
        WaitResult result_{};
        Wait& owner_;
        sched::Deadline deadline_;
    };
    using Queue = libk::intrusive_list<Registration, &Registration::hook_>;
    using Watches = libk::intrusive_list<Watch, &Watch::hook_>;
    using Hints = libk::intrusive_list<Watch, &Watch::publish_hook_>;
    void hint(Watch&, WaitResult, Publication&) noexcept;
    auto matches(const Condition&) const noexcept -> bool;
    auto result(const Condition&) noexcept -> WaitResult;
    void release() noexcept;
    mutable sync::Spin lock_{};
    Queue waiters_{};
    Watches watches_{};
    u64 bits_{}, version_{};
    usize registrations_{}, publishers_{};
    bool closed_{};
    Drained drained_{};
    u64* raised_{};
    u64 mask_{}, scan_version_{}, closed_at_{};
    WorkQueue* work_{};
    Source* next_{};
    object::Key key_{};
    Pin pin_{};
    bool scheduled_{};
public:
    class Publication final : private libk::noncopyable {
        friend class Source;
        Source* source_{};
        Queue ready_;
        Hints hints_;
        bool deferred_{};
        explicit Publication(Source& source) noexcept : source_(&source) {}
    public:
        Publication() noexcept = default;
        Publication(Publication&& other) noexcept
            : source_(std::exchange(other.source_, nullptr)), ready_(std::move(other.ready_)), hints_(std::move(other.hints_)), deferred_(other.deferred_) {}
        auto operator=(Publication&& other) noexcept -> Publication& {
            libk_assert(!source_);
            source_ = std::exchange(other.source_, nullptr);
            ready_ = std::move(other.ready_);
            hints_ = std::move(other.hints_);
            deferred_ = other.deferred_;
            return *this;
        }
        ~Publication() noexcept { publish(); }
        explicit operator bool() const noexcept { return source_ != nullptr; }
        void publish() noexcept;
    };
};

// Only bits actually produced need pulse history. A deferred scan can still
// claim READY after a later clear, without allocating an event log.
template<u64 Mask> class Src final : public Source {
    std::array<u64, std::popcount(Mask)> raised_{};
public:
    explicit Src(Drained drained = {}) noexcept : Source(drained) { history(raised_, Mask); }
};

template<class Install>
auto Source::watch(Watch& watch, Condition condition, Install&& install) noexcept -> status_t {
    Publication publication;
    auto pin = pin_;
    if (pin) pin(true);
    libk::scope_exit release{[&]() noexcept { if (pin) pin(false); }};
    {
        sync::Lock guard{lock_};
        if (closed_) return STATUS_CLOSED;
        if (watch.state_.load()) return STATUS_BAD_ARGS;
    }
    // Admission owns the outer Grant/Gates. The source is checked again at
    // their commit, and callbacks leave every publication lock before delivery.
    return install([&]() noexcept {
        sync::Lock guard{lock_};
        watch.source_ = this;
        watch.pin_ = std::exchange(pin, {});
        watch.condition_ = condition; watch.armed_ = true;
        watch.armed_at_ = version_; watch.scanned_ = scan_version_;
        watch.state_.store(Watch::registered, std::memory_order_release);
        watches_.push_back(watch);
        // A close racing admission is an accepted terminal hint, not a
        // half-installed authority which the caller would have to roll back.
        if (closed_ || matches(condition)) {
            ++publishers_; publication = Publication{*this};
            hint(watch, closed_ ? WaitResult{STATUS_CLOSED, 0, version_} : result(condition), publication);
        }
    });
}

template<class Install>
auto Source::arm(Watch& watch, Condition condition, Install&& install) noexcept -> status_t {
    Publication publication;
    status_t result = STATUS_OK;
    {
        sync::Lock guard{lock_};
        if (watch.source_ != this || !watch.attached()) return STATUS_STALE;
        if (closed_) return STATUS_CLOSED;
    }
    const auto status = install([&]() noexcept {
        sync::Lock guard{lock_};
        if (watch.source_ != this || !watch.attached()) { result = STATUS_STALE; return; }
        if (closed_) { result = STATUS_CLOSED; return; }
        watch.condition_ = condition; watch.armed_ = true;
        watch.armed_at_ = version_;
        if (matches(condition)) {
            ++publishers_; publication = Publication{*this};
            hint(watch, this->result(condition), publication);
        }
    });
    return status == STATUS_OK ? result : status;
}

// The Thread owns one fixed registration. Source queued_ decides who claims
// its result; Ready is published only after any authority drain completes.
class Wait final : private libk::noncopyable_nonmovable {
public:
    Wait() noexcept = default;
    ~Wait() noexcept { libk_assert(!attached()); }
    auto attached() const noexcept -> bool { return phase_.load(std::memory_order_acquire) != Phase::Idle; }
    auto ready() const noexcept -> bool { return phase_.load(std::memory_order_acquire) == Phase::Ready; }
    auto begin(Cpus&, Thread&) noexcept -> bool;
    auto finish() noexcept -> bool;
    auto cancel() noexcept -> bool;
    auto result() const noexcept -> WaitResult { return source_.result_; }
private:
    friend class Source;
    friend class sched::Dispatcher;
    void wake() noexcept;
    enum class Phase : u8 { Idle, Armed, Ready, Releasing };
    Cpus* cpus_{};
    Thread* binding_{};
    sync::Spin lock_;
    std::atomic<Phase> phase_{Phase::Idle};
    Source::Registration source_{*this};
};
