#pragma once
#include <boot/info.hpp>
#include <libk/manual_lifetime.hpp>

class CpuRegistry;

[[noreturn]] void start_kernel(
    libk::ManualLifetime<BootInfo>& source,
    libk::ManualLifetime<mm::RegionList>& memory) noexcept;

