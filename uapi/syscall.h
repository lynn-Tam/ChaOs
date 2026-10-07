#pragma once

enum {
#define CALL(name, nr, entry, locus, unit) SYS_##name = nr,
#include <uapi/calls.def>
#undef CALL
};
