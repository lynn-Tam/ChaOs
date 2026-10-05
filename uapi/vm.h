#pragma once

#define MYOS_VM_READ    (1U << 0)
#define MYOS_VM_WRITE   (1U << 1)
#define MYOS_VM_EXECUTE (1U << 2)

// VM_MAP only: writes copy the source page into this mapping on first write.
#define MYOS_VM_MAP_PRIVATE (1U << 8)

// Pager-backed private content may discard clean pages, but has no writeback
// destination. Dirty pages stay resident until their Mem is retired.
#define MYOS_MEMORY_PAGER_PRIVATE (1U << 0)
