#include <diag/scenario.hpp>

#ifndef MYOS_SCENARIO_ID
#define MYOS_SCENARIO_ID 0
#endif

namespace kernel::diag::scenario {

extern const Id selected = static_cast<Id>(MYOS_SCENARIO_ID);

auto enabled(Id value) noexcept -> bool {
    return value == selected;
}

} // namespace kernel::diag::scenario
