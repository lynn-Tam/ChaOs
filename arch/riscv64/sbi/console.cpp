#include <arch/console.hpp>

#include "call.hpp"

namespace arch::console {
namespace {

inline constexpr usize DebugConsoleExtension = 0x4442434e;
inline constexpr usize WriteByteFunction = 2;
inline constexpr usize LegacyPutCharExtension = 1;

} // namespace

void write(char character) noexcept {
    const auto result = riscv64::sbi::call(
        DebugConsoleExtension,
        WriteByteFunction,
        static_cast<unsigned char>(character));
    if (result.error == riscv64::sbi::success) {
        return;
    }

    // SBI 0.1 remains the early-console fallback for older firmware.
    static_cast<void>(riscv64::sbi::call(
        LegacyPutCharExtension,
        0,
        static_cast<unsigned char>(character)));
}

void write(libk::StrView text) noexcept {
    for (const char character : text) {
        write(character);
    }
}

} // namespace arch::console
