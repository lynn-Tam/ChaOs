#pragma once

#include <libk/types.hpp>
#include <optional>
#include <libk/assert.hpp>
#include <libk/noncopyable.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_tree.hpp>

namespace sched {

enum class DispatchReason : u8 {
    Start,
    Yield,
    Timer,
    Block,
    Exit,
    Stop,
    RemoteWake,
};

class Dispatcher;

// Fixed-storage one-shot deadline owned by exactly one Dispatcher while
// armed. The callback runs on that CPU with interrupts disabled; remote
// producers publish subsystem state and wake the owner instead of mutating
// this queue.
class Deadline final : private libk::noncopyable_nonmovable {
public:
    using Fn = libk::delegate<void() noexcept>;

    explicit Deadline(Fn cb) noexcept : cb_(cb) {}
    ~Deadline() noexcept { libk_assert(!armed() && !hook_.is_linked() && cb_); }

    [[nodiscard]] auto armed() const noexcept -> bool {
        return owner_ != nullptr;
    }

private:
    friend class Dispatcher;

    libk::intrusive_tree_hook hook_{};
    Fn cb_{};
    Dispatcher* owner_{};
    u64 when_{};
};

} // namespace sched
