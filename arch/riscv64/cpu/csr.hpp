#pragma once

// Typed RISC-V CSR accessors and field helpers. This file owns register
// semantics only; CPU-local runtime state lives behind the selected arch API.

#include "core/debug.hpp"
#include "core/types.hpp"
#include <libk/bits.hpp>
#include <libk/optional.hpp>

namespace arch::riscv64 {
enum class CsrId : u16 {
    Sstatus = 0x100,
    Sie = 0x104,
    Stvec = 0x105,
    Scounteren = 0x106,

    Sscratch = 0x140,
    Sepc = 0x141,
    Scause = 0x142,
    Stval = 0x143,
    Sip = 0x144,

    Satp = 0x180,
};

template <CsrId id> struct ReadOnlyCsr {
    static constexpr auto number = static_cast<u16>(id);
    static_assert(number <= 0xfff);

    static usize read() noexcept {
        usize value;
        asm volatile("csrr %0, %1" : "=r"(value) : "i"(number));
        return value;
    }
};

template <CsrId id> struct ReadWriteCsr : ReadOnlyCsr<id> {
    static constexpr u16 number = ReadOnlyCsr<id>::number;
    static void write(usize value) noexcept {
        asm volatile("csrw %0, %1" : : "i"(number), "r"(value) : "memory");
    }
};

template <CsrId id> struct BitCsr : ReadWriteCsr<id> {
    static constexpr u16 number = ReadOnlyCsr<id>::number;

    static void set_bits(usize mask) noexcept {
        asm volatile("csrs %0, %1" : : "i"(number), "r"(mask) : "memory");
    }

    static void clear_bits(usize mask) noexcept {
        asm volatile("csrc %0, %1" : : "i"(number), "r"(mask) : "memory");
    }

    [[nodiscard]] static auto read_and_clear_bits(usize mask) noexcept
        -> usize {
        usize previous;
        asm volatile(
            "csrrc %0, %1, %2"
            : "=r"(previous)
            : "i"(number), "r"(mask)
            : "memory");
        return previous;
    }
    static bool has_any(usize mask) noexcept {
        return libk::has_any(ReadOnlyCsr<id>::read(), mask);
    }

    static bool has_all(usize mask) noexcept {
        return libk::has_all(ReadOnlyCsr<id>::read(), mask);
    }
};

struct Sstatus : BitCsr<CsrId::Sstatus> {
    static constexpr usize SIE = libk::bit<usize>(1);
    static constexpr usize SPIE = libk::bit<usize>(5);
    static constexpr usize SPP = libk::bit<usize>(8);
    static constexpr usize SUM = libk::bit<usize>(18);
    static constexpr usize MXR = libk::bit<usize>(19);

    static void enable_interrupts() noexcept { set_bits(SIE); }
    static void disable_interrupts() noexcept { clear_bits(SIE); }

    static bool is_interrupts_enabled() noexcept { return has_any(SIE); }

    static void enable_user_memory_access() noexcept { set_bits(SUM); }

    static void disable_user_memory_access() noexcept { clear_bits(SUM); }
};

struct Sie : BitCsr<CsrId::Sie> {
    static constexpr usize SSIE = libk::bit<usize>(1);
    static constexpr usize STIE = libk::bit<usize>(5);
    static constexpr usize SEIE = libk::bit<usize>(9);

    static void enable_timer() noexcept { set_bits(STIE); }

    static void disable_timer() noexcept { clear_bits(STIE); }

    static void enable_software() noexcept { set_bits(SSIE); }

    static void disable_software() noexcept { clear_bits(SSIE); }

    static void enable_external() noexcept { set_bits(SEIE); }

    static void disable_external() noexcept { clear_bits(SEIE); }
};

struct Sip : ReadOnlyCsr<CsrId::Sip> {
    static constexpr usize SSIP = libk::bit<usize>(1);
    static constexpr usize STIP = libk::bit<usize>(5);
    static constexpr usize SEIP = libk::bit<usize>(9);

    static bool timer_pending() noexcept { return libk::has_any(read(), STIP); }

    static bool external_pending() noexcept { return libk::has_any(read(), SEIP); }

    static bool software_pending() noexcept { return libk::has_any(read(), SSIP); }

    // 只给 software interrupt 一个显式接口，别暴露通用 clear_bits。
    static void clear_software_pending() noexcept {
        asm volatile("csrc sip, %0" ::"r"(SSIP) : "memory");
    }
};

struct Stvec : ReadWriteCsr<CsrId::Stvec> {
    static constexpr usize MODE_SHIFT = 0;
    static constexpr usize MODE_WIDTH = 2;

    static constexpr usize MODE_MASK = libk::field_mask<usize>(MODE_SHIFT, MODE_WIDTH);
    static constexpr usize BASE_MASK = ~MODE_MASK;

    static constexpr usize DIRECT = 0;
    static constexpr usize VECTORED = 1;

    static constexpr usize make(usize base, usize mode) noexcept {
        return (base & BASE_MASK) | libk::encode_field(mode, MODE_SHIFT, MODE_WIDTH);
    }

    static void install_direct(void* entry) noexcept {
        KASSERT( ((reinterpret_cast<usize>(entry)) & MODE_MASK) == 0 );
        write(make(reinterpret_cast<usize>(entry), DIRECT));
    }

    static usize base() noexcept { return read() & BASE_MASK; }
};

struct Scause : ReadWriteCsr<CsrId::Scause> {
    static constexpr usize INTERRUPT = libk::bit<usize>((sizeof(usize) * 8) - 1);
    static constexpr usize CODE_MASK = ~INTERRUPT;

    enum class Exception : usize {
        InstructionAddressMisaligned = 0,
        InstructionAccessFault = 1,
        IllegalInstruction = 2,
        Breakpoint = 3,
        LoadAddressMisaligned = 4,
        LoadAccessFault = 5,
        StoreAddressMisaligned = 6,
        StoreAccessFault = 7,
        UserEnvCall = 8,
        SupervisorEnvCall = 9,
        InstructionPageFault = 12,
        LoadPageFault = 13,
        StorePageFault = 15,
    };

    enum class Interrupt : usize {
        SupervisorSoftware = 1,
        SupervisorTimer = 5,
        SupervisorExternal = 9,
    };
    static constexpr bool is_interrupt(usize value) noexcept {
        return libk::has_any(value, INTERRUPT);
    }

    static constexpr bool is_exception(usize value) noexcept { return !is_interrupt(value); }

    //返回scause的事件类型，调用前需要调用 is_interrupt/is_exception判断，然后类型转换为相应的枚举类型。
    static constexpr usize code(usize value) noexcept { return value & CODE_MASK; }
};

struct Sepc : ReadWriteCsr<CsrId::Sepc> {};

struct Stval : ReadWriteCsr<CsrId::Stval> {};

struct Sscratch : ReadWriteCsr<CsrId::Sscratch> {
    static usize swap(usize value) noexcept {
        asm volatile("csrrw %0, sscratch, %1" : "=r"(value) : "r"(value) : "memory");
        return value;
    }
};

struct Satp : ReadWriteCsr<CsrId::Satp> {
    static constexpr usize MODE_SHIFT = 60;
    static constexpr usize MODE_WIDTH = 4;

    static constexpr usize ASID_SHIFT = 44;
    static constexpr usize ASID_WIDTH = 16;

    static constexpr usize PPN_SHIFT = 0;
    static constexpr usize PPN_WIDTH = 44;

    static constexpr usize MODE_BARE = 0;
    static constexpr usize MODE_SV39 = 8;
    static constexpr usize MODE_SV48 = 9;
    static constexpr usize MODE_SV57 = 10;

    [[nodiscard]] static constexpr auto try_make_sv39(
        usize root_ppn,
        usize asid = 0) noexcept -> libk::optional<usize> {
        constexpr usize max_ppn =
            (usize{1} << PPN_WIDTH) - 1;
        constexpr usize max_asid =
            (usize{1} << ASID_WIDTH) - 1;
        if (root_ppn > max_ppn || asid > max_asid) {
            return libk::nullopt;
        }
        return libk::encode_field<usize>(MODE_SV39, MODE_SHIFT, MODE_WIDTH) |
               libk::encode_field<usize>(asid, ASID_SHIFT, ASID_WIDTH) |
               libk::encode_field<usize>(root_ppn, PPN_SHIFT, PPN_WIDTH);
    }

    static constexpr usize mode(usize value) noexcept {
        return libk::extract_field<usize>(value, MODE_SHIFT, MODE_WIDTH);
    }

    static constexpr usize asid(usize value) noexcept {
        return libk::extract_field<usize>(value, ASID_SHIFT, ASID_WIDTH);
    }

    static constexpr usize ppn(usize value) noexcept {
        return libk::extract_field<usize>(value, PPN_SHIFT, PPN_WIDTH);
    }
};

} // namespace arch::riscv64
