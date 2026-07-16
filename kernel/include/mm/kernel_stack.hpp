#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <mm/kernel_stack_layout.hpp>

namespace kernel::mm {
class KernelVSpace;
}

namespace kernel {

class KernelStack final {
public:
    enum class Error : u8 {
        OutOfMemory,
        AddressSpaceExhausted,
        MappingFailed,
    };

    using CreateResult = libk::Expected<KernelStack, Error>;

    KernelStack(const KernelStack&) = delete;
    auto operator=(const KernelStack&) -> KernelStack& = delete;

    KernelStack(KernelStack&& other) noexcept;
    auto operator=(KernelStack&& other) noexcept -> KernelStack&;
    ~KernelStack() noexcept;

    [[nodiscard]] static auto create(kernel::mm::KernelVSpace& vspace) noexcept
        -> CreateResult;

    [[nodiscard]] auto base() const noexcept -> usize { return base_; }
    [[nodiscard]] auto size() const noexcept -> usize { return size_; }
    [[nodiscard]] auto top() const noexcept -> usize { return base_ + size_; }
    [[nodiscard]] auto contains(usize address) const noexcept -> bool;
    [[nodiscard]] auto lower_guard() const noexcept -> usize {
        return base_ - kernel::mm::page_size;
    }
    [[nodiscard]] auto upper_guard() const noexcept -> usize {
        return top();
    }

private:
    KernelStack(kernel::mm::KernelVSpace& owner, usize base, usize size) noexcept;
    void reset() noexcept;

    kernel::mm::KernelVSpace* owner_{};
    usize base_{};
    usize size_{};
};

} // namespace kernel
