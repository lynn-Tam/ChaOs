#include <arch/instruction.hpp>

namespace arch {

void sync_instruction_stream() noexcept {
    asm volatile("fence.i" ::: "memory");
}

} // namespace arch
