#pragma once

#include <arch/trap.hpp>
#include <cap/cspace.hpp>
#include <core/types.hpp>
#include <libk/optional.hpp>
#include <libk/span.hpp>
#include <mm/vspace.hpp>
#include <execution/target.hpp>
#include <syscall/syscall.hpp>
#include <uapi/status.h>

namespace kernel {
struct CpuLocal;
class Thread;
}

namespace kernel::syscall {

struct Invocation final {
    CpuLocal& cpu;
    execution::Target target;
    cap::CSpace& cspace;
    kernel::mm::VSpace& vspace;
    arch::TrapContext& trap;
};

struct Result final {
    myos_status_t status{MYOS_STATUS_OK};
    usize value{};
    Disposition disposition{Disposition::Return};
};

[[nodiscard]] constexpr auto returned(
    myos_status_t status,
    usize value = 0) noexcept -> Result {
    return Result{status, value, Disposition::Return};
}

[[nodiscard]] auto cap_status(cap::CSpaceError error) noexcept
    -> myos_status_t;

[[nodiscard]] auto read_snapshot_bytes(
    Invocation& invocation,
    cap::CapHandle handle,
    usize offset,
    libk::Span<byte> destination) noexcept
    -> libk::Expected<void, myos_status_t>;

template<typename Descriptor>
[[nodiscard]] auto read_snapshot(
    Invocation& invocation,
    cap::CapHandle handle,
    usize offset) noexcept -> libk::Expected<Descriptor, myos_status_t> {
    Descriptor descriptor{};
    auto read = read_snapshot_bytes(
        invocation,
        handle,
        offset,
        libk::Span<byte>{reinterpret_cast<byte*>(&descriptor), sizeof(descriptor)});
    if (!read) {
        return libk::unexpected(read.error());
    }
    return libk::expected(descriptor);
}
[[nodiscard]] auto vm_status(kernel::mm::VSpaceError error) noexcept
    -> myos_status_t;
[[nodiscard]] auto operation_status(kernel::mm::VmStatus status) noexcept
    -> myos_status_t;
[[nodiscard]] auto handle_of(usize raw) noexcept -> cap::CapHandle;
[[nodiscard]] auto rights_of(usize raw) noexcept
    -> libk::optional<cap::Rights>;
[[nodiscard]] auto access_of(usize raw) noexcept
    -> libk::optional<kernel::mm::AccessMask>;
[[nodiscard]] auto types_of(usize raw) noexcept
    -> libk::optional<kernel::mm::MemoryTypes>;
[[nodiscard]] auto range_of(usize base, usize size) noexcept
    -> libk::optional<kernel::mm::VirtRange>;
[[nodiscard]] auto vm_context(CpuLocal& cpu) noexcept -> kernel::mm::VmContext;

[[nodiscard]] auto handle_execution(
    usize operation,
    Invocation& invocation) noexcept -> Result;
[[nodiscard]] auto handle_capability(
    usize operation,
    Invocation& invocation) noexcept -> Result;
[[nodiscard]] auto handle_vm(
    usize operation,
    Invocation& invocation) noexcept -> Result;
[[nodiscard]] auto handle_construction(
    usize operation,
    Invocation& invocation) noexcept -> Result;
[[nodiscard]] auto handle_object(
    usize operation,
    Invocation& invocation) noexcept -> Result;
[[nodiscard]] auto handle_notification(
    usize operation,
    Invocation& invocation) noexcept -> Result;
[[nodiscard]] auto handle_vproc(
    usize operation,
    Invocation& invocation) noexcept -> Result;
[[nodiscard]] auto handle_tunnel(
    usize operation,
    Invocation& invocation) noexcept -> Result;

} // namespace kernel::syscall
