#pragma once

#include <diag/concurrency.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>

namespace kernel {

class Execution;
class Thread;
class Vproc;

namespace execution {

// One operation-owned interest in an execution target reaching its terminal
// scheduler state. Target objects own neither this storage nor the stop
// reason; they only index attached interests until the dispatcher has removed
// every execution relation.
class Stop final : private libk::noncopyable_nonmovable {
public:
    using Notifier = libk::delegate<void() noexcept>;

    explicit Stop(Notifier notifier = {}) noexcept : notifier_(notifier) {}
    ~Stop() noexcept;

    [[nodiscard]] auto started() const noexcept -> bool { return started_; }
    [[nodiscard]] auto complete() const noexcept -> bool { return complete_; }
    [[nodiscard]] auto observation_key() const noexcept
        -> diag::concurrency::ObservationKey {
        return observation_;
    }
    void start(Thread& thread) noexcept;
    void start(Vproc& vproc) noexcept;

private:
    friend class kernel::Thread;
    friend class kernel::Vproc;

    void finish(Thread& thread) noexcept;
    void finish(Vproc& vproc) noexcept;

    Notifier notifier_{};
    Execution* target_{};
    libk::IntrusiveListHook hook_{};
    diag::concurrency::ObservationKey observation_{};
    bool started_{};
    bool complete_{};
};

} // namespace execution
} // namespace kernel
