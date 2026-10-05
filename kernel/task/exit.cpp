#include <task/exit.hpp>
#include <ipc/notification.hpp>

Exit::~Exit() noexcept {
    source_.reset();
}

auto Exit::read() const noexcept -> Info {
    sync::Lock guard{lock_};
    return value_;
}

auto Exit::published() const noexcept -> bool {
    return read().sequence != 0;
}

auto Exit::claim(Reason reason, myos_status_t status, usize detail,
                 usize pc, usize address, u8 locus) noexcept -> bool {
    {
        sync::Lock guard{lock_};
        if (value_.sequence != 0) return false;
        value_ = {1, reason, status, detail, pc, address, locus};
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
    if (value_.sequence != 0) static_cast<void>(source_.signal());
    return true;
}
