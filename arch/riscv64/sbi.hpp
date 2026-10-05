#pragma once

#include <base/types.hpp>
#include <expected>

namespace sbi {
// SBI extension/function numbers are firmware ABI, independent of the board.
enum class Ext : usize {
    Base = 0x10,
    Hsm = 0x48534d,
    Timer = 0x54494d45,
    Ipi = 0x735049,
    Reset = 0x53525354,
    Console = 0x4442434e,
    PutChar = 1,
};
inline constexpr isize Unsupported = -2, Invalid = -3, BadAddr = -5, Available = -6, Started = -7;
auto call(Ext, usize fn, usize a0 = 0, usize a1 = 0, usize a2 = 0) noexcept -> std::expected<usize, isize>;
inline bool probe(Ext ext) noexcept {
    auto r = call(Ext::Base, 3, static_cast<usize>(ext));
    return r && *r != 0;
}
} // namespace sbi
