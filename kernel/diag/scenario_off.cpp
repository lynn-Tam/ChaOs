#include <diag/scenario.hpp>

namespace kernel::diag::scenario {

extern const Id selected = Id::Off;

auto enabled(Id value) noexcept -> bool {
    return value == selected;
}

} // namespace kernel::diag::scenario
