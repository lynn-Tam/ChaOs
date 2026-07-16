#pragma once

/*
 * Blocking operations return only after completion. MYOS_STATUS_PENDING is
 * reserved for a committed operation whose hardware retirement remains owned
 * by the kernel; capability revoke does not expose a polling completion path.
 */

#define MYOS_SYS_YIELD               0
#define MYOS_SYS_EXIT                1

#define MYOS_SYS_CAP_CLOSE          16
#define MYOS_SYS_CAP_DUPLICATE      17
#define MYOS_SYS_CAP_DELEGATE       18
#define MYOS_SYS_CAP_MOVE           19
#define MYOS_SYS_CAP_REVOKE         20
#define MYOS_SYS_OBJECT_DESTROY     21

#define MYOS_SYS_VM_MAP             32
#define MYOS_SYS_VM_UNMAP           33
#define MYOS_SYS_VM_PROTECT         34
#define MYOS_SYS_VM_CREATE_REGION   35
#define MYOS_SYS_VM_RESERVE         36
#define MYOS_SYS_VM_GUARD           37
#define MYOS_SYS_VM_DESTROY_REGION  38
