#include <execution/vproc.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <ipc/notification.hpp>
#include <libk/limits.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel {

auto Vproc::attach_notification(
    ipc::NotificationLink& link,
    usize slot,
    usize tag) noexcept -> libk::optional<u64> {
    if (slot >= MYOS_VPROC_MAX_NOTIFICATIONS) {
        return libk::nullopt;
    }
    kernel::sync::IrqLockGuard guard{state_lock_};
    NotificationSlot& target = notifications_[slot];
    if (relation_admission_closed_ || target.link != nullptr
        || target.binding_generation == libk::numeric_limits<u64>::max()) {
        return libk::nullopt;
    }
    ++target.binding_generation;
    KASSERT(target.binding_generation != 0);
    target.link = &link;
    target.signal_sequence = 0;
    target.tag = tag;
    return target.binding_generation;
}

void Vproc::detach_notification(
    ipc::NotificationLink& link,
    usize slot,
    u64 binding_generation) noexcept {
    if (slot >= MYOS_VPROC_MAX_NOTIFICATIONS) {
        return;
    }
    kernel::sync::IrqLockGuard guard{state_lock_};
    NotificationSlot& target = notifications_[slot];
    if (target.link != &link
        || target.binding_generation != binding_generation) {
        return;
    }
    target.link = nullptr;
    target.signal_sequence = 0;
    target.tag = 0;
    const u64 bit = u64{1} << slot;
    notification_mask_ &= ~bit;
    if (runtime_.events != nullptr) {
        __atomic_fetch_and(
            &runtime_.events->notification_mask, ~bit, __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->notification_sequence[slot],
            0,
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->notification_tag[slot], 0, __ATOMIC_RELEASE);
    }
}

void Vproc::publish_notification(
    ipc::NotificationLink& link,
    usize slot,
    u64 binding_generation,
    u64 signal_sequence,
    usize tag,
    CpuRegistry& cpus) noexcept {
    if (slot >= MYOS_VPROC_MAX_NOTIFICATIONS) {
        return;
    }
    {
        kernel::sync::IrqLockGuard guard{state_lock_};
        NotificationSlot& target = notifications_[slot];
        if (target.link != &link
            || target.binding_generation != binding_generation
            || target.tag != tag
            || signal_sequence <= target.signal_sequence) {
            return;
        }
        target.signal_sequence = signal_sequence;
        const u64 bit = u64{1} << slot;
        ++pending_sequence_;
        KASSERT(pending_sequence_ != 0);
        notification_mask_ |= bit;
        __atomic_store_n(
            &runtime_.events->notification_sequence[slot],
            signal_sequence,
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->notification_tag[slot], tag, __ATOMIC_RELEASE);
        __atomic_fetch_or(
            &runtime_.events->notification_mask, bit, __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->pending_sequence,
            pending_sequence_,
            __ATOMIC_RELEASE);
    }
    static_cast<void>(sched::activate(cpus, *this));
}

void Vproc::clear_notification(
    ipc::NotificationLink& link,
    usize slot,
    u64 binding_generation,
    u64 signal_sequence) noexcept {
    if (slot >= MYOS_VPROC_MAX_NOTIFICATIONS) {
        return;
    }
    kernel::sync::IrqLockGuard guard{state_lock_};
    NotificationSlot& target = notifications_[slot];
    if (target.link != &link
        || target.binding_generation != binding_generation
        || target.signal_sequence != signal_sequence) {
        return;
    }
    target.signal_sequence = 0;
    const u64 bit = u64{1} << slot;
    notification_mask_ &= ~bit;
    __atomic_fetch_and(
        &runtime_.events->notification_mask, ~bit, __ATOMIC_RELEASE);
    __atomic_store_n(
        &runtime_.events->notification_sequence[slot], 0, __ATOMIC_RELEASE);
}

void Vproc::close_notifications() noexcept {
    for (;;) {
        ipc::Notification* notification{};
        {
            kernel::sync::IrqLockGuard guard{state_lock_};
            for (NotificationSlot& slot : notifications_) {
                if (slot.link != nullptr) {
                    notification = slot.link->notification;
                    break;
                }
            }
            if (notification == nullptr) {
                return;
            }
            notification->retain_relation();
        }
        notification->peer_stopped(*this);
        notification->release_relation();
    }
}

} // namespace kernel
