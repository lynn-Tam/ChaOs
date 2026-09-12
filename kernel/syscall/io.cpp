#include "kernel/syscall/internal.hpp"

#include <io/space.hpp>
#include <ipc/buffer.hpp>
#include <object/io_space_pool.hpp>
#include <object/device_pool.hpp>
#include <object/memory_pool.hpp>
#include <uapi/io.h>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {
static_assert(static_cast<u8>(io::SpaceState::Empty) == MYOS_IO_SPACE_EMPTY);
static_assert(static_cast<u8>(io::SpaceState::Binding) == MYOS_IO_SPACE_BINDING);
static_assert(static_cast<u8>(io::SpaceState::Opening) == MYOS_IO_SPACE_OPENING);
static_assert(static_cast<u8>(io::SpaceState::Active) == MYOS_IO_SPACE_ACTIVE);
static_assert(static_cast<u8>(io::SpaceState::Closing) == MYOS_IO_SPACE_CLOSING);
static_assert(static_cast<u8>(io::SpaceState::Closed) == MYOS_IO_SPACE_CLOSED);
static_assert(static_cast<u8>(io::SpaceState::Failed) == MYOS_IO_SPACE_FAILED);

auto status(io::SpaceError error) noexcept -> myos_status_t {
    switch (error) {
    case io::SpaceError::InvalidState:
    case io::SpaceError::Busy: return MYOS_STATUS_BUSY;
    case io::SpaceError::InvalidAuthority: return MYOS_STATUS_BAD_RIGHTS;
    case io::SpaceError::InvalidRange:
    case io::SpaceError::UnsupportedMemory: return MYOS_STATUS_BAD_ARGS;
    case io::SpaceError::BackingUnavailable: return MYOS_STATUS_BACKING_FAILED;
    case io::SpaceError::OutOfMemory:
    case io::SpaceError::QuotaExceeded: return MYOS_STATUS_NO_MEMORY;
    case io::SpaceError::Cancelled: return MYOS_STATUS_CANCELED;
    }
    return MYOS_STATUS_INTERNAL;
}

auto bind(Invocation& invocation, cap::Resolved<io::Space>& space) noexcept -> Result {
    const auto& trap = invocation.trap;
    auto device = invocation.cspace.resolve<io::Device>(handle_of(trap.arg(1)),
        cap::Rights::of(cap::Right::Connect));
    auto memory = invocation.cspace.resolve<mm::MemoryObject>(handle_of(trap.arg(2)),
        cap::Rights::of(cap::Right::Map));
    if (!device || !memory) return returned(cap_status(!device ? device.error() : memory.error()));
    auto self = space.reference();
    if (!self) return returned(MYOS_STATUS_CLOSED);
    auto bound = space->bind(libk::move(self).value(), device.value(), memory.value(),
        {trap.arg(3), trap.arg(4)}, trap.arg(5));
    return returned(bound ? MYOS_STATUS_OK : status(bound.error()));
}

auto info(Invocation& invocation, io::Space& space) noexcept -> Result {
    auto snapshot = space.info();
    if (!snapshot) return returned(status(snapshot.error()));
    myos_io_info output{};
    output.version = MYOS_IO_INFO_VERSION;
    for (usize index = 0; index < 64; ++index)
        output.configuration[index] = snapshot.value().configuration[index];
    for (usize index = 0; index < 6; ++index)
        output.bar_sizes[index] = snapshot.value().bar_sizes[index];
    auto* buffer = invocation.target.ipc_buffer();
    if (buffer == nullptr) return returned(MYOS_STATUS_BAD_ARGS);
    auto access = buffer->access();
    if (!access) return returned(MYOS_STATUS_BAD_ARGS);
    if (!access.value().write(invocation.trap.arg(1),
        {reinterpret_cast<const byte*>(&output), sizeof(output)})) return returned(MYOS_STATUS_BAD_ARGS);
    return returned(MYOS_STATUS_OK);
}

auto install(Invocation& invocation,
    libk::Expected<cap::GrantRef, io::SpaceError>&& exported) noexcept -> Result {
    if (!exported) return returned(status(exported.error()));
    auto lease = exported.value().acquire();
    if (!lease) return returned(MYOS_STATUS_CLOSED);
    const auto ceiling = lease.value().ceiling();
    auto installed = invocation.cspace.insert(libk::move(exported).value(),
        {ceiling.rights, ceiling.data});
    return installed ? returned(MYOS_STATUS_OK, installed.value().raw())
                     : returned(cap_status(installed.error()));
}
} // namespace

auto handle_io(usize operation, Invocation& invocation) noexcept -> Result {
    const auto right = operation == MYOS_SYS_IO_SPACE_STATE || operation == MYOS_SYS_IO_SPACE_INFO
        ? cap::Right::Inspect : operation == MYOS_SYS_IO_SPACE_CLOSE ? cap::Right::Close : cap::Right::Connect;
    auto space = invocation.cspace.resolve<io::Space>(handle_of(invocation.trap.arg(0)),
        cap::Rights::of(right));
    if (!space) return returned(cap_status(space.error()));
    switch (operation) {
    case MYOS_SYS_IO_SPACE_BIND: return bind(invocation, space.value());
    case MYOS_SYS_IO_SPACE_STATE: return returned(MYOS_STATUS_OK, static_cast<u8>(space.value()->state()));
    case MYOS_SYS_IO_SPACE_INFO: return info(invocation, space.value().object());
    case MYOS_SYS_IO_SPACE_BAR: return install(invocation, space.value()->bar(invocation.trap.arg(1)));
    case MYOS_SYS_IO_SPACE_IRQ: return install(invocation, space.value()->interrupt());
    case MYOS_SYS_IO_SPACE_CLOSE:
        space.value()->close();
        return returned(MYOS_STATUS_OK);
    default: return returned(MYOS_STATUS_INVALID_OP);
    }
}
} // namespace kernel::syscall
