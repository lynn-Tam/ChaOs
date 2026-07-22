#pragma once

/*
 * Blocking operations return only after completion. MYOS_STATUS_PENDING is
 * reserved for a committed operation whose hardware retirement remains owned
 * by the kernel; capability revoke does not expose a polling completion path.
 */

#define MYOS_SYS_YIELD               0
#define MYOS_SYS_EXIT                1
#define MYOS_SYS_SC_BIND             2 /* a0=SC, a1=Thread or Vproc */
#define MYOS_SYS_EXECUTION_START     3 /* a0=Thread or Vproc */

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

/*
 * Typed construction ABI:
 *   a0: ResourcePool capability
 *   a1-a3: type-specific scalar configuration
 * Returns a0=status and a1=new capability in the caller's CSpace.
 */
#define MYOS_SYS_RESOURCE_CREATE_CHILD 48 /* a1=memory, a2=caps, a3=kind mask */
#define MYOS_SYS_MEMORY_CREATE         49 /* a1=bytes, a2=access bits */
#define MYOS_SYS_VSPACE_CREATE         50
#define MYOS_SYS_CSPACE_CREATE         51 /* a1=slot quota, a2=page quota */
#define MYOS_SYS_SC_CREATE             52 /* a1=domain, a2=budget ns, a3=period ns, a4=urgency, a5=cpu */
#define MYOS_SYS_THREAD_CREATE         53 /* a1=VSpace, a2=CSpace, a3=start memory, a4=offset */
#define MYOS_SYS_NOTIFICATION_CREATE   54 /* a1=immutable signal badge */
#define MYOS_SYS_VPROC_CREATE          55 /* a1=VSpace, a2=CSpace, a3=start memory, a4=offset */
#define MYOS_SYS_TUNNEL_OPEN           56 /* a0=pool, a1=slot, a2=tag; target=current Vproc */
#define MYOS_SYS_ENDPOINT_CREATE        57 /* a1=VSpace, a2=CSpace, a3=descriptor memory, a4=offset */

#define MYOS_SYS_MEMORY_SEAL           64 /* a0=MemoryObject */
#define MYOS_SYS_RESOURCE_CLOSE        65 /* a0=child ResourcePool */

#define MYOS_SYS_NOTIFICATION_SIGNAL   80 /* a0=Notification; badge is in cap */
#define MYOS_SYS_NOTIFICATION_TAKE     81 /* a0=Notification */
#define MYOS_SYS_NOTIFICATION_WAIT     82 /* a0=Notification */
#define MYOS_SYS_NOTIFICATION_BIND_VPROC 83 /* a0=Notification, a1=slot, a2=tag */
#define MYOS_SYS_NOTIFICATION_UNBIND_VPROC 84 /* a0=Notification */

#define MYOS_SYS_VPROC_ARM             96 /* a0=descriptor MemoryObject, a1=byte offset */
#define MYOS_SYS_VPROC_RETURN          97 /* a0=active generation; resumes submitted context */
#define MYOS_SYS_VPROC_CHECKPOINT      98 /* safe delivery boundary; returns pending sequence */
#define MYOS_SYS_OPERATION_POLL        99 /* a0=OperationKey; non-consuming */
#define MYOS_SYS_OPERATION_CANCEL     100 /* a0=OperationKey; pre-commit only */
#define MYOS_SYS_OPERATION_FINISH     101 /* a0=OperationKey; consumes result */
#define MYOS_SYS_VPROC_PARK           102 /* a0=observed pending sequence */

#define MYOS_SYS_TUNNEL_CONNECT       104 /* a0=Connect authority; source=current Vproc */
#define MYOS_SYS_TUNNEL_INVOKE        105 /* a0=Tunnel Tx; caller must be source Vproc */
#define MYOS_SYS_TUNNEL_ACK           106 /* a0=receiver cap, a1=observed sequence */
#define MYOS_SYS_TUNNEL_CLOSE         107 /* a0=Tunnel Admin or Tx */

#define MYOS_SYS_ENDPOINT_CALL        112 /* a0=Endpoint, a1-a3=service words, a4=relative timeout ns (0=infinite) */
#define MYOS_SYS_ENDPOINT_REPLY       113 /* a0=status, a1=value */
#define MYOS_SYS_ENDPOINT_CLOSE       114 /* a0=Endpoint */
#define MYOS_SYS_ENDPOINT_MINT        115 /* a0=root, a1=dest CSpace, a2=badge, a3=cap limit, a4=rights */
#define MYOS_SYS_ENDPOINT_ABORT       116 /* a0=callee-defined abort detail */
