# ChaOs

ChaOs is an experimental capability-based microkernel for RISC-V, written in freestanding C++23.

It explores an operating-system model where authority, address spaces, execution, CPU time, memory, and communication are represented by explicit kernel objects rather than being implicitly bundled into a traditional process abstraction.

ChaOs currently targets RV64 on QEMU `virt`. The ABI is unstable and the system is not intended for production use.

## Current status

Implemented or under active development:

- RV64 boot, SMP, traps, Sv39 virtual memory, and physical memory management
- capability spaces, derivation, attenuation, revocation, and resource accounting
- threads, Vprocs, scheduling contexts, timers, and CPU dispatch
- notifications, channels, endpoints, and asynchronous completion
- user-space `init`, process supervision, UART, block, read-only FAT32, and writable data-volume services
- application loading from disk and file-backed demand paging
- an interactive shell and basic user programs
- PCI, I/O-space, DMA, and IOMMU foundations

The kernel and userspace interfaces are still evolving.

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
disk, run the `mkfs` program once; running it again erases the data volume.
The data disk is `/`; the read-only FAT32 boot disk is `/boot`. Use `touch note`,
`write note hello`, `append note world`, `cat note`, `ls`, `stat note`,
`mkdir dir`, `mv note dir/note`, and `rm dir/note`. `ls /boot` shows boot files;
`cat /boot/README.TXT` reads one; `cp /boot/README.TXT readme` copies it to the
data disk. `cat` and `ls` are ordinary programs using the userland VFS service;
the other file commands launch the ordinary `fs` program, also available as
`fs COMMAND`. VFS grants read-only access to `cat`, `ls`, and `get`, and write
access to `fs`, `edit`, and `put`. Quote names and text containing spaces,
for example `write "my note" "hello world"`. `edit note` opens a screen editor:
type to insert, move with the arrow keys, save with Ctrl+O, and exit with
Ctrl+X (which asks whether to save changes). Files larger than 128 KiB use a
line editor instead: `:p` shows numbered lines, `:a TEXT` appends,
`:i N TEXT` inserts, `:r N TEXT` replaces, `:d N` deletes, `.` saves, and
`:q` discards. The shell
hands terminal input to a foreground program until it exits; background tasks
receive EOF instead. Only `mkfs.pkg` can receive Store Admin authority;
ordinary `fs` and `edit` cannot format. Programs run by name, for example
`echo hello | put greeting`. `echo hello > greeting` replaces a file, and
`echo world >> greeting` appends to it; redirection runs the ordinary `put`
program through the same two-task pipe. Successful commands return to the prompt without
an extra status line; failures print `error: STATUS`.
Press Ctrl+A, then X to exit QEMU; later runs reuse the same data disk.

Additional test, audit, SMP matrix, debug, and proof targets are defined in `meson.build`.

## Repository

```text
arch/       Architecture-specific kernel code
kernel/     Kernel
libk/       Freestanding C++ support library
uapi/       User/kernel ABI
user/       Userspace runtime and libraries
servers/    Userspace system services
programs/   User programs
tools/      Build and image tooling
test/       Tests and validation workloads
```
