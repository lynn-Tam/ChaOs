#pragma once

#include <uapi/types.h>

// Tunnel has no payload queue. tag is immutable receiver metadata projected
// in the target Vproc event page; slot selects one bounded ingress position.
#define MYOS_TUNNEL_FLAGS_NONE 0U

