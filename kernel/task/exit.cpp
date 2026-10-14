#include <task/exit.hpp>
#include <ipc/notification.hpp>

Exit::~Exit() noexcept {
    source_.reset();
}

auto Exit::read() const noexcept -> std::optional<status_t> {
    sync::Lock guard{lock_};
    return status_;
}

auto Exit::published() const noexcept -> bool {
    return read().has_value();
}

auto Exit::claim(status_t status) noexcept -> bool {
    {
        sync::Lock guard{lock_};
        if (status_) return false;
        status_ = status;
    }
    // The embedded source lives with the record. No borrowed observer can
    // disappear between publication and delivery.
    static_cast<void>(source_.signal());
    return true;
}

auto Exit::observe(ipc::Notification& notification, u64 badge) noexcept -> bool {
    sync::Lock guard{lock_};
    if (!notification.bind(source_, badge)) return false;
    // Bind and publication share lock_: a late subscriber gets readiness too.
    if (status_) static_cast<void>(source_.signal());
    return true;
}
