#pragma once
#include <base/types.hpp>

inline constexpr usize VirtUartBase = 0x1000'0000;
inline constexpr u32 VirtUartIrq = 10;
inline constexpr usize VirtPlicBase = 0x0c00'0000;
inline constexpr usize VirtPlicSize = 0x0040'0000;
inline constexpr usize VirtPciEcam = 0x3000'0000;
inline constexpr usize VirtPciMmio = 0x4000'0000;
inline constexpr usize VirtIommuBase = 0x0301'0000;
inline constexpr u32 VirtPciIrq = 32;
inline constexpr u32 VirtPciPins = 4;
inline constexpr u32 VirtIommuIrq = 37;
