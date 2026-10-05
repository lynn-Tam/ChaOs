#pragma once

#include <expected>


#include <optional>
#include <arch/trap.hpp>
#include <cap/cspace.hpp>
#include <base/types.hpp>
#include <libk/span.hpp>
#include <mm/vspace.hpp>
#include <task/thread.hpp>

#include <uapi/status.h>

struct CpuLocal;
class Thread;

namespace syscall {

enum class Disposition : u8 {
    Return,
    Yield,
    Exit,
    Resume,
};

[[nodiscard]] auto handle(arch::TrapContext& ctx) noexcept
    -> Disposition;



struct Call final {
    CpuLocal& cpu;
    Thread* target;
    cap::CSpace& cspace;
    mm::VSpace& vspace;
    arch::TrapContext& trap;
};

struct Result final {
    myos_status_t status{MYOS_STATUS_OK};
    usize value{};
    Disposition disposition{Disposition::Return};
    usize value2{};
};

[[nodiscard]] constexpr auto returned(
    myos_status_t status,
    usize value = 0) noexcept -> Result {
    return Result{status, value, Disposition::Return, 0};
}

[[nodiscard]] constexpr auto returned(
    myos_status_t status,
    usize value,
    usize value2) noexcept -> Result {
    return Result{status, value, Disposition::Return, value2};
}

[[nodiscard]] auto cap_status(cap::CSpaceError error) noexcept
    -> myos_status_t;

[[nodiscard]] auto read_desc_bytes(
    Call& inv,
    cap::Handle handle,
    usize offset,
    libk::Span<byte> dest) noexcept
    -> std::expected<void, myos_status_t>;

template<typename Descriptor>
[[nodiscard]] auto read_desc(
    Call& inv,
    cap::Handle handle,
    usize offset) noexcept -> std::expected<Descriptor, myos_status_t> {
    Descriptor descriptor{};
    auto read = read_desc_bytes(
        inv,
        handle,
        offset,
        libk::Span<byte>{reinterpret_cast<byte*>(&descriptor), sizeof(descriptor)});
    if (!read) {
        return std::unexpected(read.error());
    }
    return (descriptor);
}
[[nodiscard]] auto vm_status(mm::VSpaceError error) noexcept
    -> myos_status_t;
[[nodiscard]] auto handle_of(usize raw) noexcept -> cap::Handle;
[[nodiscard]] auto rights_of(usize raw) noexcept
    -> std::optional<cap::Rights>;
[[nodiscard]] auto perms_of(usize raw) noexcept
    -> std::optional<mm::Perms>;
[[nodiscard]] auto types_of(usize raw) noexcept
    -> std::optional<mm::MemoryTypes>;
[[nodiscard]] auto range_of(usize base, usize size) noexcept
    -> std::optional<mm::VRange>;
[[nodiscard]] auto vm_context(CpuLocal& cpu) noexcept -> mm::VmCtx;

#define CALL(name, nr, entry, locus, unit) \
    template<usize op> [[nodiscard]] auto entry(Call&) noexcept -> Result;
#include <uapi/calls.def>
#undef CALL

} // namespace syscall
