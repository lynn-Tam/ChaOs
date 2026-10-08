#pragma once

#include <base/types.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <sync.hpp>

// An owner embeds its work. close prevents future execution, but an already
// copied callback retains the owner's existing pin/cleanup gate until return.
class Work final : private libk::noncopyable_nonmovable {
public:
    using Fn = libk::delegate<void() noexcept>;
    Work() noexcept = default;
    explicit Work(Fn fn) noexcept : fn_(fn) {}
private:
    friend class WorkQueue;
    libk::IntrusiveListHook hook_{};
    Fn fn_{};
};

// One runner, no allocation. Runtime finalizers run only here; exclusive boot
// and teardown may drain before the worker starts or after it has stopped.
class WorkQueue final : private libk::noncopyable_nonmovable {
public:
    constexpr WorkQueue() noexcept = default;
    constexpr ~WorkQueue() noexcept { libk_assert(queue_.empty() && !running_); }
    void bind(Work::Fn wake = {}) noexcept {
        sync::Lock guard{lock_};
        wake_ = wake;
        armed_ = false;
    }
    void open(Work& work, Work::Fn fn) noexcept {
        sync::Lock guard{lock_};
        libk_assert(fn && !work.fn_ && !work.hook_.is_linked());
        work.fn_ = fn;
    }
    void post(Work& work) noexcept {
        Work::Fn wake;
        {
            sync::Lock guard{lock_};
            if (!work.fn_ || work.hook_.is_linked()) return;
            queue_.push_back(work);
            if (std::exchange(armed_, false)) wake = wake_;
        }
        // Scheduler wake credit covers a post between arm() and block().
        if (wake) wake();
    }
    void close(Work& work) noexcept {
        sync::Lock guard{lock_};
        work.fn_.reset();
        if (work.hook_.is_linked()) queue_.erase(work);
    }
    auto run(usize budget = 64) noexcept -> bool {
        {
            sync::Lock guard{lock_};
            libk_assert(budget && !running_);
            running_ = true;
        }
        libk::scope_exit done{[&]() noexcept {
            sync::Lock guard{lock_};
            running_ = false;
        }};
        for (usize i = 0; i < budget; ++i) {
            Work::Fn fn;
            {
                sync::Lock guard{lock_};
                if (queue_.empty()) return false;
                fn = queue_.pop_front().fn_;
            }
            fn(); // Never touch the Work again: this may finish its owner.
        }
        sync::Lock guard{lock_};
        return !queue_.empty();
    }
    auto arm() noexcept -> bool {
        sync::Lock guard{lock_};
        libk_assert(wake_);
        return armed_ = queue_.empty();
    }
private:
    sync::Spin lock_{};
    libk::IntrusiveList<Work, &Work::hook_> queue_{};
    Work::Fn wake_{};
    bool armed_{}, running_{};
};
