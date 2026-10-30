#pragma once
#include <gui/protocol.hpp>

enum : uint32_t { dp_release = 1, dp_frame };
// Frame time is device command completion, never physical presentation time.
struct dp_event { uint64_t generation{}, sequence{}, time{}; uint32_t type{}, buffer{}; int32_t status{}; uint32_t reserved{}; };
static_assert(sizeof(dp_event) == 40);
