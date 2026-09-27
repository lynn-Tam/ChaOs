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
disk, run `mkfs` once in the shell; running it again erases the data volume.
`fs` is an ordinary disk application: `fs touch note`, `fs write note hello`,
`fs append note world`, `fs cat note`, `fs ls`, `fs stat note`, `fs mkdir dir`,
`fs mv note dir/note`, and `fs rm dir/note`. `fs copy README.TXT readme` copies
from the read-only FAT32 boot disk. Quote names and text containing spaces, for
example `fs write "my note" "hello world"`. `edit note` enters the shell's line editor:
enter replacement lines, then `.` to save or `:q` to discard. The shell owns
terminal input and the Admin authority needed by `mkfs`; ordinary `fs` has
only the Store directory capability. Programs run by name, for example
`echo hello | put greeting`. Successful commands return to the prompt without
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
