# Builtin test index

Builtin tests are registered by `registry.add(group, name, fn)` in the corresponding
`*_test.cpp` file. Those registrations are the executable truth; this file is a
reader-oriented index and should be updated with them.

Normal runs print only failed cases and the final passed/failed summary. A failure is
reported as `[FAIL] group: name`.

## libk

- Expected preserves value semantics and chain propagation
- optional supports monadic chaining without hidden fallback calls
- optional value_or selects contained or fallback value
- checked arithmetic reports overflow without asserting
- array and string views preserve empty and bounded contracts
- byte reader failures leave cursor and outputs unchanged
- inplace vector handles aliasing, moves, and zero capacity
- alignment and single-alternative variant contracts hold
- fmt remains bounded and independent of copy elision
- variant tracks its active alternative
- variant visit and equality follow the active payload
- variant moves a move-only payload
- atomic scalar operations preserve compare-exchange contract
- atomic ref synchronizes borrowed scalar storage
- intrusive AVL tree preserves ordered unique membership
- scope exit runs rollback once and supports explicit commit

## sync

- dependency graph spans multiword rows and reconstructs paths
- dependency graph leaves absent paths empty
- serialized reverse edge returns a typed cycle
- wait-cycle validation rejects changed and open snapshots
- owner word preserves CPU and acquisition generation
- LockSite compiler builtins capture the call site
- movable IRQ ownership restores the original state once
- untracked early IRQ ownership does not consume tracked depth
- ordered pair and try token own successful acquisitions once

## pmm

- owned page destruction releases its frame
- move transfers the only release authority
- metadata remains outside the free index
- boot reservations require explicit consumption
- dropped reservation authority can be taken again
- exact boot reservation handoff preserves other reservations
- boot reservation adoption transfers frame ownership
- reservation authority is bound to one owner
- discontiguous RAM forms independent arenas
- foreign pages and MMIO stay unmanaged
- exhaustion preserves ledger/index equivalence
- empty page-group move transfers authority
- page-group destruction rolls back same-arena pages
- empty page-group extension releases its borrow
- page-group extension rolls back only its new prefix
- page-group ownership chains cross arenas
- page-group detach and reattach preserve one frame owner
- direct map covers proven RAM independent of allocation state
- KernelRoot move transfers architecture ownership
- runtime editor separates private and shared table ownership
- Sv39 page-table initialization clears the complete frame
- Sv39 walk allocates only missing tables
- Sv39 mapping failures preserve allocations
- Sv39 missing branches roll back partial allocation
- Sv39 range helpers map and inspect contiguous leaves
- Sv39 range helper keeps mapped prefix on failure
- initial page-table exhaustion rolls back unpublished ownership
- direct-map policy rejects unrepresentable RAM
- direct-map construction rejects empty maps
- invalid regions are rejected
- direct-map construction rejects overlapping regions
- available RAM is required
- direct-map policy rejects RAM outside its window

## boot-map

- default BootInfo owns an empty map
- regions are valid and non-overlapping
- regions are ordered by address
- kernel image owns only persistent load regions
- pre-kernel RAM stays firmware-reserved
- FDT pages are reclaimable
- transitional tables are reclaimable
- region policy is explicit
- multiple RAM banks normalize into one inventory
- permanent reservations override reclaimable data
- adjacent reclaimable resources keep lifetime boundaries
- overlapping RAM banks are rejected
- an inventory requires RAM
- byte ranges state their page rounding
- FDT memory reservations stay within their block

## boot-bundle

- valid manifest is a bounded borrowed view
- bad envelope is rejected before materialization
- writable executable segment is rejected

## cpu-topology

- sparse IDs and firmware statuses populate canonical descriptors
- malformed CPU nodes are rejected
- boot hart matching requires one enabled entry
- builder rejects duplicate IDs and incomplete population
- record blocks cross legacy continuous-array thresholds
- logical CPU namespace has one explicit bound
- each stack/object allocation failure leaves runtime unpublished
- runtime metadata exhaustion leaves association unpublished
- secondary prepare failure preserves the prepared boot CPU
- prepare publishes one descriptor-backed CpuRuntime
- lifecycle publication and snapshots derive from canonical states
- shootdown acknowledgement controls detached-page retirement
- ObjectRef and typed pins share canonical reclaim state
- RemoteQueue retains failed kicks without stale-generation loss

## sched

- deadline queue orders fixed one-shot relations
- refill ledger conserves budget and reports overrun
- bounded refill merge delays but never advances budget
- refill model preserves every sampled sliding window
- SC configuration rejects invalid time and urgency bounds
- ObjectStore unpublished construction rolls back slab and payload
- ResourcePool refunds only after sponsored object reclaim
- child ResourcePool returns its delegated budget after reclaim
- ResourcePool close waits for construction and budget transactions
- kernel stacks use guarded reusable virtual slots
- domain admission rounds conservatively and rolls back failure
- ReadyQueue chooses highest urgency and preserves FIFO

## cap

- resolve composes authority and pins the typed target
- duplicate attenuates a shared grant without amplification
- delegation revoke blocks new use and drains old leases
- handles are CSpace-local and stale generations stay dead
- move preserves source when destination transaction fails
- IPC transfer publishes copy and move as one destination batch
- IPC transfer rolls back every reservation after source mutation
- CSpace retirement waits for reserved operations and pinned teardown
- sponsored CSpace refunds selector and table capacity at reuse
- attenuated ABI operations and revoke use effective slot authority
- revoked tombstones release their hidden target hold
- allocation transaction abort revokes its complete hidden lineage
- ResourcePool close revokes hidden roots without scanning CSpaces
- parent ResourcePool close recursively drains its child pool
- destroy authority enters the target ObjectAnchor retirement path
- Tunnel Connect authority cannot attenuate into source Tx rights

## memory

- anonymous sparse pages own zeroed resident frames
- physical backing borrows reserved RAM and device extents
- boot image distinguishes borrowed and owned frame release
- reverse attachment drives destroy invalidation completion
- executable seal closes writable attachment admission
- ObjectStore memory retirement waits for active page lease

## translation

- activation before mutation is captured by the ticket
- mutation before activation publishes the entering epoch
- aborts do not advance and tickets retain distinct epochs
- executable mutations retain an instruction visibility epoch

## vspace

- semantic map, protect and arbitrary unmap share one layout truth
- lazy fault materialization survives mapping split
- capability revoke waits for PTE and alias retirement
- child Region and capability publish in one transaction
- Memory retirement invalidates mapping and hardware projection
- ExecutionBinding blocks retirement of effective roots
- IPC binding blocks normal edits and follows strong invalidation
- sponsored table capacity follows physical retirement

## user

- RISC-V instruction parcels distinguish compressed completion
- UserStart rejects privilege and canonical-address forgery
- synthetic first frame lives only on the Thread home stack
- register ABI values remain explicit UAPI data

## ipc

- Notification ORs badges and one take wins the pending state
- Notification source rearm preserves level readiness
- Notification source has one receiver-owned binding
- Notification authority cannot rewrite its fixed badge
- Endpoint authority fixes caller badge and only narrows admission
