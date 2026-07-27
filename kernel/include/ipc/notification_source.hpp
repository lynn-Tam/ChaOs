#pragma once

#include <core/types.hpp>
#include <ipc/notification_link.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <sync/lock.hpp>

namespace kernel::ipc {

class Notification;

// A source owns this relation and its canonical readiness state. Notification
// stores only the non-owning aggregation edge.
class NotificationSource final : private libk::noncopyable_nonmovable {
public:
    template<typename Owner, void (Owner::*Closed)() noexcept>
    [[nodiscard]] static auto bind(Owner& owner) noexcept
        -> NotificationSource {
        static constexpr Ops operations{
            .closed = [](void* context) noexcept {
                (static_cast<Owner*>(context)->*Closed)();
            },
        };
        return NotificationSource{owner, operations};
    }

    ~NotificationSource() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto signal() noexcept -> bool;
    void reset() noexcept;

private:
    friend class Notification;

    struct Ops final {
        void (*closed)(void*) noexcept;
    };

    template<typename Owner>
    explicit NotificationSource(Owner& owner, const Ops& ops) noexcept
        : owner_(&owner), ops_(&ops) {}

    mutable kernel::sync::SpinLock<
        kernel::sync::LockClass::NotificationSource> lock_{};
    libk::IntrusiveListHook hook_{};
    void* owner_{};
    const Ops* ops_{};
    Notification* notification_{};
    u64 badge_{};
};

} // namespace kernel::ipc
