#pragma once
#include <cstdint>
struct in_event { uint16_t type{}, code{}; int32_t value{}; };
// SYN_REPORT terminates a hardware report. Chunks retain the same sequence;
// reset discards any partial report. Time is driver receipt, not hardware time.
struct in_batch { uint64_t sequence{}, time{}; uint32_t count{}, reset{}; in_event events[4]{}; };
static_assert(sizeof(in_batch) == 56);
