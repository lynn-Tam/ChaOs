Disk file service
Files are read through isolated asynchronous queues and a userspace block driver.
Applications are loaded from original ELF executables served from the initramfs boot directory.

Swap is managed by the userspace memory service and is off by default.
Use an exclusive, unused block volume; format overwrites its header.
For a 16 MiB volume named pci-10:

  swap format pci-10 --limit 8388608
  swap on pci-10 --limit 8388608
  swap status
  swap pause pci-10
  swap on pci-10
  swap off pci-10

pause stops new allocations on that volume; existing pages can still be read.
off drains its backing into RAM or another enabled volume. If resources are
insufficient it returns an error and retains the backing in Paused state.
Closing means the device connection is being retired; cancellation and re-enable
cannot undo this phase. Wait for Off, then enable the volume again.
Omit the volume to pause/off all volumes. Enable can also change an active
volume's limit: growth prepays metadata, shrink drains the excluded slots.
--offset and --bytes select an exclusive interval; --limit selects data capacity
within it (one additional 4096-byte page is used for the format header).
All byte sizes must be multiples of 4096. Higher --priority values are preferred.

  swap set resident 8388608
  swap set logical 67108864
  swap set swap 16777216
  swap set read-ahead 1
  swap set writeback-depth 6

resident is a reclaim target (0 disables it); logical limits admitted memory;
swap limits backing usage. These three settings accept an optional session ID,
defaulting to 0. Global settings are free-low/free-high (bytes), read-ahead (0/1)
and writeback-depth (1..6, change only while I/O is idle). RAM/Quota accounting
always measures real allocated resources; swap does not enlarge a RAM quota.
status reports reads/writes/released as page counts, capacity/usage as bytes,
and resident~= as a sampled estimate rather than exact instantaneous RSS.

The memory deployment setup accepts free-low, free-high, read-ahead,
writeback-depth, logical, resident, swap-limit and a volumes array. Each array
entry has volume and optional offset/bytes/limit/priority, as above. Startup
only enables previously formatted volumes; it never formats automatically.

For planned service replacement, complete swap off, then:

  bus call memory.admin Release
  svc restart memory

Release requires no backing or outstanding work. The original native Pager
survives the service instance, so existing clients continue with the new one.
Additional sessions return owner and scope capabilities through memory.Open;
pass both to memory.admin.Adopt after clean release. Keep the scope open through
the handoff. Closing it, or its owning environment, retires that session and
reclaims its backing after in-flight I/O exits. Session 0 is the default; other
session IDs are native Pager IDs and remain stable when another session retires.
While serving, the daemon owns each Pager through its execution Scope. An unclean stop closes the Pager
and fails its clients; resident management commands remain usable. Recreate
the affected sessions before starting new pageable clients. Transparent recovery
after a service crash or loss of the swap device is not provided.

Swap verification targets (same build graph): run-swap, run-swap-failure,
run-swap-write-error, run-swap-read-error, run-swap-device-loss,
run-swap-sessions and run-swap-physical. The physical pressure target uses a
separate test boot that owns real PMM pages, leaving
about 4 MiB free without reducing client Quotas; production has no pressure
injection hook. QEMU_RAM and QEMU_SMP override test console run parameters.
