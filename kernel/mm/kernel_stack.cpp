#include <mm/kernel_stack.hpp>

#include <libk/utility.hpp>
#include <mm/kernel_vspace.hpp>

namespace kernel {

auto KernelStack::create(kernel::mm::KernelVSpace& vspace) noexcept -> CreateResult {
    auto lease = vspace.acquire_stack();
    if (!lease) {
        switch (lease.error()) {
        case kernel::mm::KernelVSpace::StackError::OutOfMemory:
            return libk::unexpected(Error::OutOfMemory);
        case kernel::mm::KernelVSpace::StackError::AddressSpaceExhausted:
            return libk::unexpected(Error::AddressSpaceExhausted);
        case kernel::mm::KernelVSpace::StackError::MappingFailed:
            return libk::unexpected(Error::MappingFailed);
        }
        __builtin_unreachable();
    }
    const auto acquired = lease.value();
    return libk::expected(KernelStack{
        vspace, acquired.base, acquired.size});
}

KernelStack::KernelStack(
    kernel::mm::KernelVSpace& owner,
    usize base,
    usize size) noexcept
    : owner_(&owner),
      base_(base),
      size_(size) {
}

KernelStack::~KernelStack() noexcept {
    reset();
}

KernelStack::KernelStack(KernelStack&& other) noexcept
    : owner_(libk::exchange(other.owner_, nullptr)),
      base_(other.base_),
      size_(other.size_) {
    other.base_ = 0;
    other.size_ = 0;
}

auto KernelStack::operator=(KernelStack&& other) noexcept -> KernelStack& {
    if (this != &other) {
        reset();
        owner_ = libk::exchange(other.owner_, nullptr);
        base_ = other.base_;
        size_ = other.size_;
        other.base_ = 0;
        other.size_ = 0;
    }
    return *this;
}

void KernelStack::reset() noexcept {
    if (owner_ == nullptr) {
        return;
    }
    owner_->release_stack(base_);
    owner_ = nullptr;
    base_ = 0;
    size_ = 0;
}

auto KernelStack::contains(usize address) const noexcept -> bool {
    return address >= base_ && address < top();
}

} // namespace kernel
