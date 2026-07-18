#pragma once

namespace arch {

// Makes prior writes to executable memory visible to subsequent instruction
// fetches on this hart. Cross-hart obligations are coordinated by the kernel
// before a target can run newly sealed content.
void sync_instruction_stream() noexcept;

} // namespace arch
