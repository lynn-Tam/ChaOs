# ChaOs

ChaOs is an experimental capability-based microkernel for RISC-V, written in freestanding C++23.

It explores an operating-system model where authority, address spaces, execution, CPU time, memory, and communication are represented by explicit kernel objects rather than being implicitly bundled into a traditional process abstraction.

ChaOs currently targets RV64 on QEMU `virt`. The ABI is unstable and the system is not intended for production use.

## Current status

Implemented or under active development:

- RV64 boot, SMP, traps, Sv39 virtual memory, and physical memory management
- capability spaces, derivation, attenuation, revocation, and resource quota management
- threads, scheduling domains and contexts, timers, and CPU dispatch
- notifications, MCMP channels, native CALL/Reply, and asynchronous completion
- ubus dynamic discovery, self-described methods and a common capability-aware value codec
- user-space `init`, process supervision, UART, block, immutable initramfs bootfs, and writable data-volume services
- original ELF loading from a capability-authorized file view
- an interactive shell and basic user programs
- runtime firmware discovery, capability-authorized MMIO/IRQ controllers,
  independent I/O address spaces, and DMA retirement
- ordinary managed device/driver processes with reset and replacement
- a user-space GUI framework with independent host windows, a native VirtIO GPU/Input backend, shared pixel buffers and a libvterm terminal
- Doomgeneric as an ordinary windowed application with persistent configuration and saves

The kernel and userspace interfaces are still evolving. Hardware support currently
covers the QEMU `virt` RV64 configuration with PLIC, NS16550 UART, RISC-V IOMMU,
and modern PCI VirtIO block, GPU 2D, keyboard and tablet devices. Storage is optional.
The PCI launch configurations assign distinct INTx routes to interrupting devices; shared
INTx, 3D acceleration and audio are not implemented. This does
not imply arbitrary board, requester-alias, or hotplug support.

## Build

Requirements include Meson, Ninja, QEMU, Python 3, ccache, host json-c, and a `riscv64-unknown-elf` GCC toolchain. The normal host-window launcher also needs the native SDL2 development package.

```sh
CCACHE_DIR=build/ccache meson setup build/riscv64 \
    --cross-file cross/riscv64-gcc.ini \
    -Dbuildtype=plain -Db_ndebug=true

tools/build/ninja.sh kernel.elf bundle
```

Run the complete system under QEMU with:

```sh
tools/build/ninja.sh run
```

It starts the serial command line only, using one persistent data disk at
`build/riscv64/data.img`. Starting a graphical application opens its own native
host window; there is no QEMU desktop or second terminal at boot. `run-console`
and `run-gui` are aliases for this same launcher, system image and disk.
The host viewer needs SDL2 development files at build time and an available
X11/Wayland session at runtime (including WSLg). The viewer uses shared pixels
and a dedicated VirtIO serial stream; terminal input/logs remain separate.
New disks have a capacity of 256 MiB. On a new disk, run the shell `mkfs` command
once; running it again erases the data volume. Existing disks retain their contents.
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
all its members; Ctrl+Z pauses their execution domains. `jobs`, `fg`, `bg`, `kill`, and `wait` manage
whole pipelines; `COMMAND &` starts one in the background with stdin left open.
`tasks` shows programs and discovered devices; `view tasks`
opens it with read-only management endpoints. `tasks` has Programs and Devices pages;
use Tab or 1/2 to switch. The program list shows executable names, state, CPU and
resident memory. Enter opens details, `a` opens available actions, `/` searches,
and Esc returns. Sorting (`s`), the ownership tree (`t`), stopped programs (`h`),
and pausing display updates (Space) are optional. Raw kernel objects and capability
mutation are not part of this interface. Editors and task views retain state
and redraw after `fg`. `status` shows the last completed pipeline’s member results. Launch configuration explicitly grants the formatting command a Store Admin route; ordinary applications and `edit` receive their own restricted file views. Programs run by name, for example
`echo hello | cat`. `echo hello > greeting` replaces a file, and
`echo world >> greeting` appends to it; redirection runs ordinary `fs save`
program through the same two-task pipe. Successful commands return to the prompt without
an extra status line; failures print `error: STATUS`.
`svc` lists the explicitly granted namespace. `svc restart PATH` and `svc remove PATH` act through its owner; `svc spawn PATH PROGRAM [ARGS]` creates a deployment in that namespace, independently of the caller and alongside existing programs.
Acceptance precedes recovery and publication. Replacement drains only the owning
branch; old business endpoints close and accepted business mutations are never replayed.
Press Ctrl+A, then X to exit QEMU; later runs reuse the same data disk.

`svc start term` explicitly opens an optional graphical terminal running the
same shell, `edit`, `tasks` and file commands. The host window manager handles
moving, resizing, switching and closing each window. F11 toggles fullscreen.
Closing the last graphical window leaves the serial shell available. Replacing
`gui` reconnects the graphical terminal while preserving its shell and VT state;
ordinary applications own their own reconnect/exit policy. The host viewer is
supervised by the launcher and rebuilds windows after a disconnect. Window geometry
and raster dimensions are independent; a fixed raster can be scaled by the backend
with an optional aspect-ratio constraint. Doom keeps its 640×400 buffers in fullscreen.
The current framework implements 2D windows and input. It does not provide Wayland/RDP
wire compatibility, 3D, audio, clipboard, drag-and-drop or IME support.

To include a locally supplied Doom-compatible IWAD, configure its absolute path:

```sh
CCACHE_DIR=build/ccache meson configure build/riscv64 -Ddoom_wad=/absolute/path/to/game.wad
tools/build/ninja.sh run
```

Run `doom` in the shell; it opens its own host window. Initialization messages appear in the terminal while assets load; the window is shown with the first rendered frame. Use `doom &` to keep the command line available. An explicit `-iwad` overrides the bundled
`/boot/doom.wad`; relative game paths resolve in the writable `doom/` directory,
which also holds configuration and per-IWAD saves on the same data volume as other
applications. Default controls are arrow keys to move/turn, J to fire, Space to
use/open, Shift to run, comma/period to strafe, number keys to select weapons,
Tab for the map, F2/F3 to save/load, and F10 then Y to quit. Bindings in
`doom/default.cfg` use the game's standard DOS scan codes (J is 36).
A configured IWAD is included in the common `system.bundle` used by
every normal entry point. Game data is not downloaded by the build or included in
the repository. `run-gui-matrix` explicitly exercises the native VirtIO GPU/Input backend,
terminal and recovery path on 1/2/4 harts; when an IWAD is
configured it also saves, power cycles and loads Doom. Checks use independent
scratch disks and do not open the persistent user disk. Headless kernel/storage
test bundles retain their smaller deployment budgets and are used only by validation.

`init` supervises one root manager through the same `mgr` mechanism used by the
configured manager instances, device program and shell. Programs receive one opaque
environment capability, then request objects, receive delegated capabilities or
publish their own endpoints through ordinary IPC. Get can activate an idle
configured provider and waits for its actual publication. Bind and business calls
go directly to that provider. No global registry or business SDK is required.

`bus list`, `bus describe NAME`, and `bus call NAME METHOD FIELD=VALUE` use the
same Value codec, actual method descriptions and capability transport as any
other application. `bus call @control Configure name=example program=/package`
installs a lazy deployment; Configure accepts explicit exports and routes.
`Spawn` also requests an execution. Published objects can be found as
`example/EXPORT` in the authorized local environment. A manager commits dynamic
configuration snapshots to its direct parent before acknowledging changes;
recovery preserves recipes and applies the explicit restart policy. Consumed
linear capabilities are never duplicated or replayed. Lookup and Delegate return bounded namespace/deployment/execution capabilities; Close invalidates existing bindings and descendants. Read-only capabilities cannot resize resources or activate providers. Management roots are attached independently of business publication. Basic observation does not grant native object diagnostics; those require a separate explicit diagnostic permission. `debug bus call @control Inspect ...` is an optional configured diagnostic launch. Device inventory comes from its owner, and observational `Get activate=false` only resolves existing publications.

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
lib/        Native runtime/syscall SDK and ELF construction only
ubus/       Common values, dynamic methods, discovery and direct RPC
mgr/        Recursive environments, activation and deployment ownership
gui/        GUI protocol, client SDK, window/session mechanisms and presentation backends
apps/       Peer userspace programs, including managers and drivers
apps/posix/ Explicit C/POSIX subset adapter
tools/      Build and image tooling
test/       Tests and validation workloads
```
