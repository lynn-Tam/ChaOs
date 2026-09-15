#include <operation/vm_wait.hpp>

#include <mm/vspace.hpp>

namespace kernel::operation {

VmWait::VmWait(object::ObjectRef&& target, mm::VSpace& space) noexcept
    : target_(libk::move(target)), space_(&space),
      completion_(Completion::bind<VmWait, &VmWait::complete, &VmWait::read,
          &VmWait::release, &VmWait::cancel>(*this)) {}

VmWait::~VmWait() noexcept {
    KASSERT(!completion_.attached() && !hook_.is_linked());
}

void VmWait::start() noexcept {
    KASSERT(space_ != nullptr && completion_.attached());
    space_->wait_pending(*this);
}

auto VmWait::complete() const noexcept -> bool {
    return ready_.load<libk::MemoryOrder::Acquire>();
}

auto VmWait::read() noexcept -> Result {
    KASSERT(complete());
    return {};
}

auto VmWait::cancel() noexcept -> bool {
    return space_->cancel_wait(*this);
}

void VmWait::release() noexcept {
    KASSERT(!hook_.is_linked());
    space_ = nullptr;
    target_.reset();
}

} // namespace kernel::operation
