#pragma once

#include <boot/info.hpp>

struct Cpu;
struct Boot;
namespace mm { class Pmm; }

namespace test {
extern Boot* boot;
void fail_ipis(usize count) noexcept;
void run(const BootInfo&, const mm::Pmm&) noexcept;
void runtime(Cpu&) noexcept;
}
