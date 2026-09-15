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
- user-space `init`, process supervision, UART, block, and FAT32 file services
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
tools/build/ninja.sh run-console-smp
```

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