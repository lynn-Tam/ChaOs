# myos

> An AI-assisted, capability-based, object-oriented microkernel built with
> C++23.

`myos` is a learning and research-oriented operating-system kernel built to
explore a deliberately ambitious idea:

> C++ can do more than implement low-level kernel code. Its type system,
> ownership patterns, compile-time abstractions, and generic programming model
> can help express the architecture of a complex operating-system kernel
> clearly and safely.

The project develops a real freestanding microkernel from the machine upward:
boot, physical and virtual memory, traps, SMP, threads, scheduling, kernel
objects, capabilities, system calls, user execution, IPC, and eventually
user-space services, drivers, and filesystems.

It is not intended to imitate Unix internally or to hide hardware behind a
large framework. The goal is a small, comprehensible system in which authority,
resource ownership, execution flow, failure, and hardware state can be followed
directly through the source.

## What makes myos interesting?

### A capability system built around authority, not handles

Capabilities are not merely integer handles checked at syscall entry. The
kernel separates:

- object identity and lifetime;
- authority derivation and revocation;
- local capability naming and attenuation;
- the persistent relationships created by an authorized operation.

This makes it possible to reason about delegation, revocation, object
destruction, and hardware-visible effects without hiding several different
lifetime models inside one reference count.

### Object-oriented, without turning the kernel into a class hierarchy

In `myos`, *object-oriented* means that the system is composed from independent,
typed kernel objects with explicit lifetimes and capability-controlled
relationships. It does not mean that every object derives from a virtual base
class.

Threads, address spaces, capability spaces, memory objects, scheduling
contexts, endpoints, and scheduling domains are intended to remain independent
roots. User space may compose them into process-like or service-like structures,
but the kernel does not make a Unix-style `Process` object the hidden owner of
every resource.

### One truth owner for every mechanism

The kernel distinguishes canonical state from projections and hardware
materialization:

- a `MemoryObject` owns logical memory content and backing;
- a `VSpace` owns semantic virtual layout and mappings;
- page tables and TLB entries materialize that layout;
- an object store owns storage and lifecycle;
- a capability graph owns authority provenance;
- a dispatcher owns the committed execution state of a CPU.

Caches, reverse indexes, entry blocks, active-root tokens, and hardware
registers may reflect this state, but they must never become competing truth
sources. This rule drives both the architecture and the code organization.

### Revocation reaches the hardware

A capability-derived mapping is not considered revoked merely because a table
entry in the capability space disappeared. Revocation is designed to flow
through the semantic mapping, page-table removal, remote TLB shootdown, and
deferred resource retirement before completion becomes visible.

This connects high-level authority semantics to the physical reality of an SMP
machine.

### Execution, address space, authority, and CPU time are separate

The design does not make a thread the owner of everything needed to run. A
thread is a stable execution identity that can be bound to independent address,
capability, fault-handling, IPC, and scheduling resources.

The longer-term invocation model builds on this separation:

```text
Thread --------------------------- stable logical execution
  +-- ExecutionBinding ---------- address space, capabilities, fault/IPC view
  +-- SchedulingContext --------- capability-controlled CPU time
  +-- Activation chain ---------- synchronous cross-domain call state

Endpoint ------------------------ capability-addressable invocation target
CpuDispatcher ------------------- validated execution and time commit
```

Synchronous invocation is intended to move the same logical execution through
callee-owned activation state while continuing to consume caller-carried CPU
time. Asynchronous IPC remains a different operation: the receiver runs on its
own execution and scheduling resources. Fast paths may optimize these semantics,
but must not become a second state machine.

### Modern C++ as a kernel construction language

The kernel uses C++23 to make low-level contracts visible:

- RAII and move-only types for pages, roots, stacks, references, and leases;
- strong address, range, object, capability, CPU, and time types;
- `Expected`-style error handling without exceptions;
- concepts, templates, and static polymorphism for closed, high-frequency type
  sets;
- fixed-capacity and intrusive containers for allocation-aware data structures;
- compile-time layout and ABI checks;
- `[[nodiscard]]`, `noexcept`, and explicit state machines as real contracts.

`libk` is a purpose-built freestanding C++ foundation rather than an imitation
of the entire hosted standard library. Facilities enter the trusted kernel
surface only when their contracts can be audited and tested.

The kernel is built without exceptions or RTTI, and the final ELF is audited
for unwanted compiler runtime fallbacks, unwind dependencies, and accidental
virtual dispatch.

### SMP and failure are architectural concerns

SMP is not treated as a late optimization. CPU-local state, remote wakeups,
translation invalidation, resource retirement, scheduling, and panic handling
are designed around explicit publication and ownership rules.

Failure paths receive the same attention as success paths. The diagnostic
substrate includes typed fatal events, per-CPU panic snapshots, guarded runtime
kernel stacks, bounded backtraces, and best-effort peer stopping that cannot
wait forever for an unresponsive hart.

### Architecture boundaries without wrapper stacks

Architecture selection happens at the include path. Kernel code includes the
same `<arch/...>` contract on every target, while each architecture provides
that contract directly from `arch/<arch>/include/arch/`:

```text
kernel/                         architecture-independent mechanisms and policy
arch/<arch>/include/arch/       selected public architecture contract
arch/<arch>/                    private ISA, firmware, trap, context, and MMU code
platform/<board>/               future board-specific devices and drivers
```

There is no generic wrapper header followed by an `arch::backend` alias layer.
RISC-V-specific representations stay in `arch::riscv64`; the selected public
surface uses the names and contracts a future x86 implementation must also
provide.

Address facts are split by ownership: Sv39 canonical-address rules belong to
the architecture, the direct-map and user/kernel virtual layout belong to
kernel MM policy, and physical/virtual image relations come from linker symbols.
The `platform/` tree is intentionally unused for the current QEMU target; it is
reserved for real board-level device differences instead of becoming a bucket
for SBI calls or linker constants.

### AI-assisted, evidence-driven engineering

AI is used as a design partner, adversarial reviewer, implementation assistant,
and debugging aid. It helps compare prior art, challenge ownership models,
inspect cross-cutting invariants, and follow execution into the debugger.

AI output is not treated as truth. Architectural decisions must survive source
review, compiler audits, tests, QEMU execution, disassembly, and—when
necessary—GDB evidence. The project is as much an experiment in disciplined
AI-assisted systems engineering as it is an operating-system project.

## Architectural character

The project draws lessons from systems such as L4, seL4, QNX, Zircon,
HelenOS, HongMeng, scheduler activations, and capability-oriented research. It
also studies local mechanisms from larger kernels where they are useful.

It does not attempt to reproduce any one of them. Its architectural character
comes from combining:

- capability-derived authority with explicit persistent relations;
- independent object roots instead of a mandatory process aggregate;
- semantic virtual memory with hardware-complete revocation;
- caller-carried time for synchronous service invocation;
- kernel-enforced scheduling with room for user-space policy;
- modern C++ ownership and compile-time specialization;
- diagnostics designed as part of the kernel, not as an afterthought.

## Repository layout

```text
arch/       architecture-specific boot, trap, context, CPU, and MMU mechanisms
kernel/     kernel objects, capabilities, scheduling, VM, traps, and syscalls
libk/       audited freestanding C++ types, containers, formatting, and atomics
platform/   future board-specific devices and drivers
uapi/       C, C++, and assembly-safe user/kernel ABI declarations
user/       user-mode support and executable proof payloads
servers/    future user-space system services
test/       kernel tests and architectural verification
```

## Build and run

The supported development environment uses a RISC-V bare-metal GCC toolchain
and QEMU `riscv64` system emulation.

Typical prerequisites are:

- `riscv-none-elf-gcc` or `riscv64-unknown-elf-gcc`;
- matching GNU binutils;
- GNU `make`;
- `qemu-system-riscv64`;
- Clang for the optional cross-compiler syntax audit.

Build the kernel:

```sh
make
```

Build the test or user-execution proof profiles:

```sh
make test
make proof
```

Run under QEMU:

```sh
make run
make run-test-smp
make run-proof-smp
```

Inspect the panic paths or start a GDB session:

```sh
make run-panic-smp
make run-panic-degraded-smp
make debug
```

Useful static audits include:

```sh
make audit-symbols
make audit-clang
```

Build composition is explicit through `ARCH` and `PROFILE`; build artifacts are
isolated under `build/<arch>/<profile>/`.

## Who is this for?

`myos` is for readers who want to study operating-system mechanisms in real
code while also exploring what disciplined modern C++ can offer at the lowest
software layer.

The project favors source that can answer four questions:

1. Who owns this state or resource?
2. Which execution path may change it?
3. What invariant makes the operation safe?
4. What happens when the operation cannot complete?

It is an experimental learning kernel, not a production operating system or a
stable public ABI. Internal interfaces are expected to evolve when a cleaner
ownership model or dependency boundary is found.

## License

No license has been declared yet. Until one is added, the repository is
available for viewing and study but does not grant general permission to copy,
modify, or redistribute the code.
