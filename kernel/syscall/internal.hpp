#pragma once

#include <arch/trap.hpp>
#include <cap/cspace.hpp>
#include <core/types.hpp>
#include <libk/optional.hpp>
#include <mm/vspace.hpp>
#include <syscall/syscall.hpp>
#include <uapi/status.h>

namespace kernel {
struct CpuLocal;
class Thread;
}

namespace kernel::syscall {

struct Invocation final {
    CpuLocal& cpu;
    Thread& thread;
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

[[nodiscard]] auto handle_thread(
    usize operation,
    Invocation& invocation) noexcept -> Result;
[[nodiscard]] auto handle_capability(
    usize operation,
    Invocation& invocation) noexcept -> Result;
[[nodiscard]] auto handle_vm(
    usize operation,
    Invocation& invocation) noexcept -> Result;

} // namespace kernel::syscall
