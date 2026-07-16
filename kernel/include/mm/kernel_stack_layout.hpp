#pragma once

#include <core/types.hpp>
#include <mm/addr.hpp>

namespace kernel::mm {

struct KernelStackLayout final {
    static constexpr usize GuardPages = 1;
    static constexpr usize StackPages = 4;
    static constexpr usize StackBytes = StackPages * page_size;
    static constexpr usize SlotPages = GuardPages + StackPages + GuardPages;
    static constexpr usize SlotBytes = SlotPages * page_size;
};

} // namespace kernel::mm
