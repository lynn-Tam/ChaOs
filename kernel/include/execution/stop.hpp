#pragma once

#include <libk/delegate.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/variant.hpp>

namespace kernel {

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
    void start(Thread& thread) noexcept;
    void start(Vproc& vproc) noexcept;

private:
    friend class kernel::Thread;
    friend class kernel::Vproc;

    void finish(Thread& thread) noexcept;
    void finish(Vproc& vproc) noexcept;

    using Target = libk::variant<libk::monostate, Thread*, Vproc*>;

    Notifier notifier_{};
    Target target_{};
    libk::IntrusiveListHook hook_{};
    bool started_{};
    bool complete_{};
};

} // namespace execution
} // namespace kernel
