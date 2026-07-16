#pragma once

#define MYOS_VM_READ    (1U << 0)
#define MYOS_VM_WRITE   (1U << 1)
#define MYOS_VM_EXECUTE (1U << 2)

#define MYOS_VM_NORMAL  (1U << 0)
#define MYOS_VM_UNCACHED (1U << 1)
#define MYOS_VM_DEVICE  (1U << 2)
