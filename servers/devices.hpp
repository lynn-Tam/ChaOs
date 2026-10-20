#pragma once
#include <start.hpp>
#include <array>

inline constexpr rt_key dv_firmware{"firmware", ObjMem};
inline constexpr rt_key dv_physical{"physical", ObjPhys};
inline constexpr rt_key dv_interrupts{"interrupts", ObjIrqCtl};
inline constexpr rt_key dv_iommu{"iommu", ObjIommu};
inline constexpr rt_key dv_config{"boot", ObjMem};
inline constexpr rt_key dv_space{"device.dma", ObjIoSpace};
inline constexpr rt_key dv_irq{"device.irq", ObjIrq};
inline constexpr std::array<rt_key, 7> dv_regs{{
    {"bar0", ObjMem}, {"bar1", ObjMem}, {"bar2", ObjMem},
    {"bar3", ObjMem}, {"bar4", ObjMem}, {"bar5", ObjMem}, {"pci.config", ObjMem}}};
