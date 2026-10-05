#pragma once

#include <boot/info.hpp>

struct CpuRuntime;
namespace mm { class Pmm; }

namespace test {
void run(const BootInfo&, const mm::Pmm&) noexcept;
void runtime(CpuRuntime&) noexcept;
}
