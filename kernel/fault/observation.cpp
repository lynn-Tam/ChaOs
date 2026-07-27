#include <fault/observation.hpp>

#include <core/debug.hpp>
#include <ipc/notification.hpp>
#include <libk/memory.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::fault {

namespace {

constexpr usize observation_capacity = 32;
struct ObservationSlot final {
    bool occupied{};
    alignas(TerminalObservation) byte storage[sizeof(TerminalObservation)];
};

kernel::sync::SpinLock<kernel::sync::LockClass::Terminal>
    observation_lock{};
ObservationSlot observations[observation_capacity]{};

} // namespace

TerminalObservation::TerminalObservation() noexcept
    : observer_(this, &TerminalObservation::notify),
      source_(ipc::NotificationSource::bind<
          TerminalObservation, &TerminalObservation::closed>(*this)) {}

TerminalObservation::~TerminalObservation() noexcept {
    reset();
}

auto TerminalObservation::bind(
    TerminalRecord& target,
    ipc::Notification& notification,
    u64 badge) noexcept -> bool {
    if (badge == 0 || target_ != nullptr
        || !notification.bind(source_, badge)) {
        return false;
    }
    if (!target.attach(observer_)) {
        source_.reset();
        return false;
    }
    target_ = &target;
    notification_ = &notification;
    if (++generation_ == 0) {
        generation_ = 1;
    }
    if (target.published()) {
        static_cast<void>(source_.signal());
    }
    return true;
}

void TerminalObservation::reset() noexcept {
    if (target_ != nullptr) {
        target_->detach(observer_);
        target_ = nullptr;
    }
    if (notification_ != nullptr) {
        source_.reset();
        notification_ = nullptr;
    }
}

auto TerminalObservation::query() const noexcept -> Snapshot {
    return target_ != nullptr ? target_->snapshot() : Snapshot{};
}

void TerminalObservation::notify(void* context) noexcept {
    auto& self = *static_cast<TerminalObservation*>(context);
    if (self.notification_ != nullptr) {
        static_cast<void>(self.source_.signal());
    }
}

void TerminalObservation::closed() noexcept {
    if (target_ != nullptr) {
        target_->detach(observer_);
        target_ = nullptr;
    }
    notification_ = nullptr;
}

auto allocate_observation() noexcept -> TerminalObservation* {
    kernel::sync::IrqLockGuard guard{observation_lock};
    for (ObservationSlot& slot : observations) {
        if (slot.occupied) {
            continue;
        }
        slot.occupied = true;
        return libk::construct_at(
            reinterpret_cast<TerminalObservation*>(slot.storage));
    }
    return nullptr;
}

void release_observation(TerminalObservation& observation) noexcept {
    ObservationSlot* found{};
    for (ObservationSlot& slot : observations) {
        if (reinterpret_cast<TerminalObservation*>(slot.storage)
            == &observation) {
            found = &slot;
            break;
        }
    }
    KASSERT(found != nullptr && found->occupied);
    libk::destroy_at(&observation);
    kernel::sync::IrqLockGuard guard{observation_lock};
    found->occupied = false;
}

} // namespace kernel::fault
