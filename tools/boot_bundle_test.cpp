#include <stddef.h>
#include <stdint.h>

#include <uapi/boot_bundle.h>
#include <libk/assert.hpp>

#include "../user/lib/boot_bundle.hpp"

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    __builtin_trap();
}
} // namespace libk

namespace {

void put(uint8_t* bytes, size_t offset, uint64_t value, size_t width) {
    for (size_t byte = 0; byte < width; ++byte) {
        bytes[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
    }
}

auto make_bundle(uint8_t* bytes, size_t segment_count) -> size_t {
    constexpr size_t modules_offset = MYOS_BOOT_HEADER_SIZE;
    constexpr size_t segments_offset = modules_offset + MYOS_BOOT_MODULE_SIZE;
    constexpr size_t name_offset = segments_offset + 2 * MYOS_BOOT_SEGMENT_SIZE;
    constexpr size_t image_offset = name_offset + 8;
    constexpr size_t total_size = image_offset + 8;
    for (size_t index = 0; index < total_size; ++index) {
        bytes[index] = 0;
    }
    put(bytes, 0, MYOS_BOOT_MAGIC, 8);
    put(bytes, 8, MYOS_BOOT_MAJOR, 2);
    put(bytes, 10, MYOS_BOOT_MINOR, 2);
    put(bytes, 12, MYOS_BOOT_HEADER_SIZE, 4);
    put(bytes, 16, total_size, 8);
    put(bytes, 24, MYOS_BOOT_ARCH_RISCV64, 4);
    put(bytes, 28, MYOS_BOOT_ABI_RISCV_LP64, 4);
    put(bytes, 40, modules_offset, 8);
    put(bytes, 48, 1, 4);
    put(bytes, 52, 0, 4);
    put(bytes, 56, segments_offset, 8);
    put(bytes, 64, segment_count, 4);

    put(bytes, modules_offset, name_offset, 8);
    put(bytes, modules_offset + 8, 1, 4);
    put(bytes, modules_offset + 12, MYOS_BOOT_MODULE_BOOTABLE, 4);
    put(bytes, modules_offset + 16, image_offset, 8);
    put(bytes, modules_offset + 24, segment_count * 4, 8);
    put(bytes, modules_offset + 32, 0x200000, 8);
    put(bytes, modules_offset + 40, 0, 4);
    put(bytes, modules_offset + 44, segment_count, 4);
    bytes[name_offset] = 'x';
    for (size_t index = 0; index < segment_count * 4; ++index) {
        bytes[image_offset + index] = static_cast<uint8_t>(index + 1);
    }
    for (size_t index = 0; index < segment_count; ++index) {
        const size_t offset = segments_offset
            + index * MYOS_BOOT_SEGMENT_SIZE;
        put(bytes, offset, 0x200000 + index * 0x1000, 8);
        put(bytes, offset + 8, image_offset + index * 4, 8);
        put(bytes, offset + 16, 4, 8);
        put(bytes, offset + 24, 0x1000, 8);
        put(bytes, offset + 32, 0x1000, 8);
        put(bytes, offset + 40,
            MYOS_BOOT_SEGMENT_READ | MYOS_BOOT_SEGMENT_EXECUTE, 4);
    }
    return total_size;
}

auto valid_bundle() -> bool {
    uint8_t bytes[256]{};
    const size_t size = make_bundle(bytes, 1);
    const auto bundle = myos::boot::Bundle::parse(bytes, size);
    myos::boot::Module module{};
    myos::boot::Segment segment{};
    return bundle && bundle.root_is("x") && bundle.module(0, module)
        && module.segment(0, segment) && segment.memory_size == 0x1000
        && segment.file_size == 4;
}

auto rejects_wx() -> bool {
    uint8_t bytes[256]{};
    const size_t size = make_bundle(bytes, 1);
    put(bytes, 144 + 40,
        MYOS_BOOT_SEGMENT_WRITE | MYOS_BOOT_SEGMENT_EXECUTE, 4);
    return !myos::boot::Bundle::parse(bytes, size);
}

auto rejects_entry_gap() -> bool {
    uint8_t bytes[256]{};
    const size_t size = make_bundle(bytes, 1);
    put(bytes, MYOS_BOOT_HEADER_SIZE + 32, 0x300000, 8);
    return !myos::boot::Bundle::parse(bytes, size);
}

auto rejects_overlap() -> bool {
    uint8_t bytes[256]{};
    const size_t size = make_bundle(bytes, 2);
    put(bytes, 144 + MYOS_BOOT_SEGMENT_SIZE, 0x200800, 8);
    return !myos::boot::Bundle::parse(bytes, size);
}

auto rejects_nul_name() -> bool {
    uint8_t bytes[256]{};
    const size_t size = make_bundle(bytes, 1);
    bytes[240] = 0;
    return !myos::boot::Bundle::parse(bytes, size);
}

} // namespace

int main() {
    return valid_bundle() && rejects_wx() && rejects_entry_gap()
            && rejects_overlap() && rejects_nul_name()
        ? 0
        : 1;
}
