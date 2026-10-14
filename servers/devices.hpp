#pragma once
#include <start.hpp>
#include <array>

inline constexpr rt_req dv_firmware{"firmware", 0, ObjMem};
inline constexpr rt_req dv_physical{"physical", 0, ObjPhys};
inline constexpr rt_req dv_interrupts{"interrupts", 0, ObjIrqCtl};
inline constexpr rt_req dv_iommu{"iommu", 0, ObjIommu};
inline constexpr rt_req dv_config{"device.config", 0, ObjMem};
inline constexpr rt_req dv_space{"device.dma", 0, ObjIoSpace};
inline constexpr rt_req dv_irq{"device.irq", 0, ObjIrq};
inline constexpr std::array<rt_req, 7> dv_regs{{
    {"bar0", 0, ObjMem}, {"bar1", 0, ObjMem}, {"bar2", 0, ObjMem},
    {"bar3", 0, ObjMem}, {"bar4", 0, ObjMem}, {"bar5", 0, ObjMem}, {"pci.config", 0, ObjMem}}};
struct dv_boot { uint64_t firmware_bytes; uint32_t firmware_offset, firmware_format; };
