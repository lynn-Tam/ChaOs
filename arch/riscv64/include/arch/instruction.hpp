#pragma once

#include <core/types.hpp>

namespace arch {

// RISC-V instruction parcels are 16 bits. Low bits 11 select a 32-bit (or
// longer) encoding; the current ISA contract excludes encodings longer than
// 32 bits, so breakpoint completion only needs this first parcel.
[[nodiscard]] constexpr auto instruction_size(u16 first) noexcept -> usize {
    return (first & 0x3U) == 0x3U ? 4 : 2;
}

// Makes prior writes to executable memory visible to subsequent instruction
// fetches on this hart. Cross-hart obligations are coordinated by the kernel
// before a target can run newly sealed content.
void sync_instruction_stream() noexcept;

} // namespace arch
