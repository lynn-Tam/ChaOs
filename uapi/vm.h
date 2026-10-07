#pragma once

#define VM_READ    (1U << 0)
#define VM_WRITE   (1U << 1)
#define VM_EXECUTE (1U << 2)

// VM_MAP only: writes copy the source page into this mapping on first write.
#define VM_MAP_PRIVATE (1U << 8)

// Pager-backed private content may discard clean pages, but has no writeback
// destination. Dirty pages stay resident until their Mem is retired.
#define MEMORY_PAGER_PRIVATE (1U << 0)
