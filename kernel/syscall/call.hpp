#pragma once

#include <expected>


#include <optional>
#include <trap.hpp>
#include <cap/cspace.hpp>
#include <base/types.hpp>
#include <libk/span.hpp>
#include <mm/vspace.hpp>
#include <task/thread.hpp>

#include <uapi/abi.h>

struct CpuLocal;
class Thread;

namespace syscall {

enum class Disposition : u8 {
    Return,
    Yield,
    Exit,
    Resume,
};

[[nodiscard]] auto handle(arch::TrapCtx& ctx) noexcept
    -> Disposition;



struct Call final {
    CpuLocal& cpu;
    Thread* target;
    cap::CSpace& cspace;
    mm::VSpace& vspace;
    arch::TrapCtx& trap;
};

struct Result final {
    status_t status{STATUS_OK};
    usize value{};
    Disposition disposition{Disposition::Return};
    usize value2{};
};

[[nodiscard]] constexpr auto returned(
    status_t status,
    usize value = 0) noexcept -> Result {
    return Result{status, value, Disposition::Return, 0};
}

[[nodiscard]] constexpr auto returned(
    status_t status,
    usize value,
    usize value2) noexcept -> Result {
    return Result{status, value, Disposition::Return, value2};
}

[[nodiscard]] auto cap_status(cap::CSpaceError error) noexcept
    -> status_t;

[[nodiscard]] auto read_desc_bytes(
    Call& inv,
    cap::Handle handle,
    usize offset,
    libk::Span<byte> dest) noexcept
    -> std::expected<void, status_t>;

template<typename Descriptor>
[[nodiscard]] auto read_desc(
    Call& inv,
    cap::Handle handle,
    usize offset) noexcept -> std::expected<Descriptor, status_t> {
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
[[nodiscard]] auto mem_status(mm::MemErr error) noexcept -> status_t;
[[nodiscard]] auto vm_status(mm::VSpaceError error) noexcept
    -> status_t;
[[nodiscard]] auto handle_of(usize raw) noexcept -> cap::Handle;
[[nodiscard]] auto rights_of(usize raw) noexcept
    -> std::optional<cap::Rights>;
[[nodiscard]] auto perms_of(usize raw) noexcept
    -> std::optional<mm::Perms>;
[[nodiscard]] auto range_of(usize base, usize size) noexcept
    -> std::optional<mm::VRange>;
[[nodiscard]] auto vm_context(CpuLocal& cpu) noexcept -> mm::VmCtx;

#define CALL(name, nr, entry, locus, unit) \
    template<usize op> [[nodiscard]] auto entry(Call&) noexcept -> Result;
#include <uapi/calls.def>
#undef CALL

} // namespace syscall
