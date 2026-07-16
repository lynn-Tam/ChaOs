#include <mm/vspace_work.hpp>

#include <core/debug.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

VSpaceExecutor::~VSpaceExecutor() noexcept {
    KASSERT(!notifier_);
    KASSERT(queue_.empty());
}

void VSpaceExecutor::submit(VSpace& space) noexcept {
    Notifier notifier{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (space.work_hook_.is_linked()) {
            return;
        }
        queue_.push_back(space);
        notifier = notifier_;
    }
    if (notifier) {
        notifier();
    }
}

auto VSpaceExecutor::take() noexcept -> VSpace* {
    kernel::sync::IrqLockGuard guard{lock_};
    return queue_.empty() ? nullptr : &queue_.pop_front();
}

auto VSpaceExecutor::run(VmContext context, usize budget) noexcept -> bool {
    KASSERT(budget != 0);
    for (usize completed = 0; completed < budget; ++completed) {
        VSpace* const space = take();
        if (space == nullptr) {
            break;
        }
        const VSpaceServiceResult result = space->service(context);
        if (!result) {
            KPANIC((kernel::diag::FatalEvent{
                .facility = kernel::diag::Facility::Memory,
                .id = kernel::diag::EventId{0x70000001},
                .arguments = {
                    reinterpret_cast<usize>(space),
                    static_cast<usize>(result.error()),
                },
                .argument_count = 2,
            }));
        }
        if (result.value() == VSpaceServiceState::Retry
            || result.value() == VSpaceServiceState::Progress
            || space->work_ready()) {
            submit(*space);
        }
    }
    return pending();
}

auto VSpaceExecutor::pending() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return !queue_.empty();
}

void VSpaceExecutor::bind_notifier(Notifier notifier) noexcept {
    KASSERT(notifier);
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(!notifier_);
    notifier_ = notifier;
}

void VSpaceExecutor::unbind_notifier() noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    notifier_.reset();
}

} // namespace kernel::mm
