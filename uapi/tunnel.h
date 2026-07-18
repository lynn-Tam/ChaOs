#pragma once

#include <uapi/types.h>

// Tunnel has no payload queue. The receiver opens one listening object for its
// current Vproc; a source later connects using a delegated Connect authority.
// tag is immutable receiver metadata projected in the target event page.
#define MYOS_TUNNEL_FLAGS_NONE 0U
