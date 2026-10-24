#pragma once

#include <optional>
#include <libk/types.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <sync.hpp>
#include <cpu/types.hpp>
#include <object/ref.hpp>

// An embedded Work has a weak owner key. Each queued/running callback owns a
// storage Ref until it returns; static boot/pool work needs no owner ticket.
namespace sched { class Domain; }
class Source;
class Work final : private libk::noncopyable_nonmovable {
public:
    using Fn = libk::delegate<void() noexcept>;
    // Accepted work is indexed independently of a specific operation protocol.
    // Domain claims cancellation under its index lock, then calls outside it;
    // release waits for the claimed callback before the record can be reused.
    struct Link final : private libk::noncopyable_nonmovable {
        friend class sched::Domain;
        Link(Fn cancel, Fn ready) noexcept : cancel_(cancel), ready_(ready) {}
        void bind(object::Key owner) noexcept { owner_ = owner; }
    private:
        Link* prev_{}, *next_{};
        object::Key owner_;
        Fn cancel_, ready_;
        bool linked_{};
        usize flights_{};
    };
    constexpr Work() noexcept = default;
    constexpr explicit Work(Fn fn, Fn cancel = {}) noexcept : fn_(fn), cancel_(cancel) {}
    auto cancelable() const noexcept -> bool { return bool(cancel_); }
    auto key() const noexcept -> object::Key { return owner_; }
    void bind(object::Key owner) noexcept { libk_assert(!owner_.valid() && !hook_.is_linked()); owner_ = owner; }
private:
    friend class WorkList;
    friend class WorkQueue;
    libk::intrusive_list_hook hook_{};
    Fn fn_{}, cancel_{};
    object::Key owner_{};
    object::ref<> ticket_;
    bool running_{}, again_{};
};

// Runners claim independent finite steps. Reposting a running Work merges one
// next step; its execution Ref remains owned until the callback actually returns.
class WorkList : private libk::noncopyable_nonmovable {
public:
    constexpr WorkList() noexcept = default;
    constexpr ~WorkList() noexcept { libk_assert(queue_.empty() && !running_); }
    using Wake = libk::delegate<void(CpuId) noexcept>;
    void bind(Wake wake) noexcept {
        sync::Lock guard{lock_}; libk_assert(wake && !wake_); wake_ = wake;
    }
    void open(Work& work, Work::Fn fn) noexcept {
        sync::Lock guard{lock_};
        libk_assert(fn && !work.fn_ && !work.hook_.is_linked());
        work.fn_ = fn;
    }
    void post(Work& work) noexcept {
        {
            sync::Lock guard{lock_};
            if (!work.fn_) return;
            if (work.running_) { work.again_ = true; return; }
            if (work.hook_.is_linked()) return;
        }
        wake(enqueue(work, object::ref<>::retain_alive(work.owner_)));
    }
    // The caller may combine queue admission with its own short publication
    // boundary. Storage preparation and the wake stay outside that boundary.
    auto enqueue(Work& work, object::ref<> ticket) noexcept -> std::optional<CpuId> {
        sync::Lock guard{lock_};
        return enqueue_locked(work, std::move(ticket));
    }

    void wake(std::optional<CpuId> cpu) noexcept { if (cpu) wake_(*cpu); }
    auto close(Work& work, bool permanent = true) noexcept -> object::ref<> {
        object::ref<> ticket;
        {
            sync::Lock guard{lock_};
            if (permanent) work.fn_.reset();
            work.again_ = false;
            if (work.hook_.is_linked()) queue_.erase(work);
            ticket = std::move(work.ticket_);
        }
        return ticket;
    }
    auto run(usize budget = 64, bool cancel = false) noexcept -> bool {
        libk_assert(budget);
        for (usize i = 0; i < budget; ++i) {
            object::ref<> ticket;
            Work* work;
            Work::Fn fn;
            std::optional<CpuId> wake;
            {
                sync::Lock guard{lock_};
                if (queue_.empty()) return false;
                work = &queue_.pop_front();
                libk_assert(!work->running_ && !work->again_);
                work->running_ = true; ++running_;
                fn = cancel ? work->cancel_ : work->fn_;
                libk_assert(fn);
                ticket = std::move(work->ticket_);
            }
            fn();
            {
                // The execution Ref still protects embedded Work here. Static
                // pool/Graph jobs outlive every runner, including boot drains.
                sync::Lock guard{lock_};
                libk_assert(work->running_ && running_);
                work->running_ = false; --running_;
                if (std::exchange(work->again_, false) && work->fn_) {
                    work->ticket_ = std::move(ticket);
                    queue_.push_back(*work);
                    wake = wake_locked();
                }
            }
            if (wake) wake_(*wake);
        }
        sync::Lock guard{lock_};
        return !queue_.empty();
    }
    void disarm(CpuId runner) noexcept { sync::Lock guard{lock_}; static_cast<void>(armed_.erase(runner)); }
    auto drained() noexcept -> bool { sync::Lock guard{lock_}; return queue_.empty() && !running_; }
    auto arm(CpuId runner) noexcept -> bool {
        sync::Lock guard{lock_};
        libk_assert(runner.raw < MaxCpus && wake_);
        if (!queue_.empty()) return false;
        (void)armed_.insert(runner);
        return true;
    }
private:
    auto wake_locked() noexcept -> std::optional<CpuId> {
        libk_assert(lock_.held());
        auto cpu = armed_.pop(CpuId{next_});
        if (cpu) next_ = (cpu->raw + 1) % MaxCpus;
        return cpu;
    }
protected:
    auto enqueue_locked(Work& work, object::ref<>&& ticket) noexcept -> std::optional<CpuId> {
        libk_assert(lock_.held());
        if (!work.fn_) return {};
        if (work.running_) { work.again_ = true; return {}; }
        if (work.hook_.is_linked()) return {};
        work.ticket_ = std::move(ticket);
        queue_.push_back(work);
        return wake_locked();
    }
    sync::Spin lock_{};
    libk::intrusive_list<Work, &Work::hook_> queue_{};
    Wake wake_{};
    CpuSet armed_{};
    usize next_{}, running_{};
};

// Ordinary Domain queues carry only Work. Maintenance queues additionally
// own one prepaid, serial dispatcher for all of their Sources.
class WorkQueue final : public WorkList {
    friend class Source;
    void queue_source(Source&) noexcept;
    void source_step() noexcept;
    Source* sources_{};
    Source** source_tail_{&sources_};
    Work source_job_{Work::Fn::bind<&WorkQueue::source_step>(*this)};
public:
    constexpr WorkQueue() noexcept = default;
    constexpr ~WorkQueue() noexcept { libk_assert(!sources_); }
};
