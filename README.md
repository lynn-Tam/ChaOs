# ChaOs

ChaOs is an experimental capability-based microkernel for RISC-V, written in freestanding C++23.

It explores an operating-system model where authority, address spaces, execution, CPU time, memory, and communication are represented by explicit kernel objects rather than being implicitly bundled into a traditional process abstraction.

ChaOs currently targets RV64 on QEMU `virt`. The ABI is unstable and the system is not intended for production use.

## Current status

Implemented or under active development:

- RV64 boot, SMP, traps, Sv39 virtual memory, and physical memory management
- capability spaces, derivation, attenuation, revocation, and resource accounting
- threads, scheduling domains and contexts, timers, and CPU dispatch
- notifications, MCMP channels, native CALL/Reply, and asynchronous completion
- ubus dynamic discovery, self-described methods and a common capability-aware value codec
- user-space `init`, process supervision, UART, block, immutable initramfs bootfs, and writable data-volume services
- original ELF loading from a capability-authorized file view
- an interactive shell and basic user programs
- runtime firmware discovery, capability-authorized MMIO/IRQ controllers,
  independent I/O address spaces, and DMA retirement
- ordinary managed device/driver processes with reset and replacement

The kernel and userspace interfaces are still evolving. Hardware support currently
covers the QEMU `virt` RV64 configuration with PLIC, NS16550 UART, RISC-V IOMMU,
and modern PCI virtio-block with one data volume. Storage is optional. This does
not imply arbitrary board, requester-alias, or hotplug support.

## Build

Requirements include Meson, Ninja, QEMU, Python 3, ccache, and a `riscv64-unknown-elf` GCC toolchain.

```sh
CCACHE_DIR=build/ccache meson setup build/riscv64 \
    --cross-file cross/riscv64-gcc.ini \
    -Dbuildtype=plain -Db_ndebug=true

tools/build/ninja.sh kernel.elf bundle
```

Run the native userspace environment under QEMU with:

```sh
tools/build/ninja.sh run-console
```

The console boots with a persistent data disk at `build/riscv64/data.img`. On a new
disk, run the shell `mkfs` command once; running it again erases the data volume.
The immutable initramfs is mounted at `/boot`; a configured data disk is mounted at `/`.
The boot mount and shell remain usable when storage is absent or stopped. Use `create note`,
`write note hello`, `append note world`, `cat note`, `ls`, `stat note`,
`mkdir dir`, `mv note dir/note`, and `rm dir/note`. `ls /boot` shows boot files;
`cat /boot/README.TXT` reads one; `cp /boot/README.TXT readme` copies it to the
data disk. `cat`, `ls`, and `cp` are ordinary programs using the userland VFS service;
the other file commands launch the ordinary `fs` program, also available as
`fs COMMAND`. Applications resolve files through their explicitly granted bus view. Quote names and text containing spaces,
for example `write "my note" "hello world"`. `edit note` opens a screen editor:
type to insert, move with the arrow keys, save with Ctrl+O, and exit with
Ctrl+X (which asks whether to save changes). The buffer grows with the process heap. The shell brokers terminal input for the foreground pipeline. Ctrl+C terminates
all its members; Ctrl+Z suspends them. `jobs`, `fg`, `bg`, `kill`, and `wait` manage
whole pipelines; `COMMAND &` starts one in the background with stdin left open.
`tasks` displays authorized task, device and capability snapshots; `view tasks`
opens it with read-only management endpoints. Editors and task views retain state
and redraw after `fg`. `status` shows the last completed pipeline’s member results. Only the trusted shell receives explicit Store Admin authority;
ordinary `fs` and `edit` cannot format. Programs run by name, for example
`echo hello | cat`. `echo hello > greeting` replaces a file, and
`echo world >> greeting` appends to it; redirection runs ordinary `fs save`
program through the same two-task pipe. Successful commands return to the prompt without
an extra status line; failures print `error: STATUS`.
`svc` lists supervised children; `svc restart NAME` changes their owner’s desired state.
Acceptance precedes recovery and publication. Registry or subsystem manager loss rebuilds
the owned root subtree; old business endpoints close and accepted mutations are never replayed.
Press Ctrl+A, then X to exit QEMU; later runs reuse the same data disk.

`init` owns one root execution. `system`, `devices`, `session`, and shell jobs use
recursive process/resource ownership; the independent `ubusd` directory knows no
executable names or business methods. Ordinary programs publish their own endpoints
in a delegated domain and call each other directly after Bind. All methods share
one value codec and derive Describe and validation from the same method table.
`bus list`, `bus describe NAME`, and `bus call NAME METHOD FIELD=VALUE` work without
a service SDK. `bus call session Spawn name=example program=/path/to/program` starts
an installed package dynamically; its publications appear below `providers/example/`.

Deployment bundles contain standard ELF files with debug information removed;
the original ELF files retain symbols beside the build objects. Test workloads
are included only in test bundles.

Additional test, audit, SMP matrix, debug, and proof targets are defined in `meson.build`.

## Repository

```text
arch/       Architecture-specific kernel code
kernel/     Kernel
libk/       Freestanding C++ support library
uapi/       User/kernel ABI
lib/        Native runtime, process construction and optional semantic clients
ubus/       Common values, dynamic methods, discovery and direct RPC
posix/      Explicit C/POSIX subset adapter
servers/    Userspace system services
programs/   User programs
tools/      Build and image tooling
test/       Tests and validation workloads
```
