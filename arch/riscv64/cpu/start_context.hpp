#pragma once

#define RISCV64_CPU_START_PUBLICATION_OFFSET 0
#define RISCV64_CPU_START_HARDWARE_ID_OFFSET 8
#define RISCV64_CPU_START_SATP_OFFSET 16
#define RISCV64_CPU_START_STACK_TOP_OFFSET 24
#define RISCV64_CPU_START_RUNTIME_OFFSET 32
#define RISCV64_CPU_START_ENTRY_OFFSET 40
#define RISCV64_CPU_START_CONTEXT_SIZE 48
#define RISCV64_CPU_START_READY 0x43505552

#if !defined(__ASSEMBLER__)

#include <stddef.h>

#include <base/types.hpp>
#include <cpu/types.hpp>
#include <libk/sync/atomic.hpp>
#include <type_traits>

struct CpuRuntime;

namespace arch::riscv64 {

using SecondaryContinuation = void (*)(
    CpuRuntime*, usize) noexcept;

struct CpuStartContextLayout;

// Immutable input consumed by a secondary before a C++ stack exists. The
// publication word is only an ABI readiness gate; CpuDescriptor owns lifecycle.
class CpuStartContext final {
public:
    constexpr CpuStartContext() noexcept = default;

    CpuStartContext(const CpuStartContext&) = delete;
    auto operator=(const CpuStartContext&) -> CpuStartContext& = delete;
    CpuStartContext(CpuStartContext&&) = delete;
    auto operator=(CpuStartContext&&) -> CpuStartContext& = delete;

    void initialize(
        CpuHwId hardware_id,
        usize root,
        usize init_stack_top,
        CpuRuntime& runtime,
        SecondaryContinuation entry) noexcept;

    [[nodiscard]] auto ready() const noexcept -> bool;

private:
    friend struct CpuStartContextLayout;

    libk::Atomic<u32> publication_{};
    [[maybe_unused]] u32 padding_{};
    usize hardware_id_{};
    usize satp_{};
    usize init_stack_top_{};
    CpuRuntime* runtime_{};
    SecondaryContinuation entry_{};
};

struct CpuStartContextLayout final {
    static constexpr bool valid =
        offsetof(CpuStartContext, publication_)
                == RISCV64_CPU_START_PUBLICATION_OFFSET
        && offsetof(CpuStartContext, hardware_id_)
                == RISCV64_CPU_START_HARDWARE_ID_OFFSET
        && offsetof(CpuStartContext, satp_)
                == RISCV64_CPU_START_SATP_OFFSET
        && offsetof(CpuStartContext, init_stack_top_)
                == RISCV64_CPU_START_STACK_TOP_OFFSET
        && offsetof(CpuStartContext, runtime_)
                == RISCV64_CPU_START_RUNTIME_OFFSET
        && offsetof(CpuStartContext, entry_)
                == RISCV64_CPU_START_ENTRY_OFFSET;
};

static_assert(std::is_standard_layout_v<CpuStartContext>);
static_assert(std::is_trivially_destructible_v<CpuStartContext>);
static_assert(sizeof(CpuStartContext) == RISCV64_CPU_START_CONTEXT_SIZE);
static_assert(alignof(CpuStartContext) == alignof(usize));
static_assert(CpuStartContextLayout::valid);

} // namespace arch::riscv64

#endif
