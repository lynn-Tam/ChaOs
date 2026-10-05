#pragma once

#include <base/types.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/delegate.hpp>
#include <sync.hpp>

namespace ipc {

class Notification;

// A source owns this relation and its canonical readiness state. Notification
// stores only the non-owning aggregation edge.
class NotificationSource final : private libk::noncopyable_nonmovable {
public:
    using Closed = libk::delegate<void() noexcept>;
    explicit NotificationSource(Closed closed = {}) noexcept : closed_(closed) {}

    ~NotificationSource() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto signal() noexcept -> bool;
    void reset() noexcept;

private:
    friend class Notification;

    mutable sync::Spin lock_{};
    libk::IntrusiveListHook hook_{};
    Closed closed_{};
    Notification* notification_{};
    u64 badge_{};
};

} // namespace ipc
