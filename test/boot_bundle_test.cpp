#include <image/boot_bundle.hpp>
#include <libk/assert.hpp>
#include <test/test.hpp>
#include <uapi/boot_bundle.h>

namespace {

constexpr usize BundleSize = 200;
constexpr usize SegmentOffset = MYOS_BOOT_HEADER_SIZE + MYOS_BOOT_MODULE_SIZE;
constexpr usize NameOffset = SegmentOffset + MYOS_BOOT_SEGMENT_SIZE;
constexpr usize ImageOffset = NameOffset + 4;

void write_le(byte* output, usize& cursor, u64 value, usize width) noexcept {
    for (usize index = 0; index < width; ++index) {
        output[cursor++] = static_cast<byte>(value >> (index * 8));
    }
}

void build_bundle(byte (&bytes)[BundleSize]) noexcept {
    usize cursor{};
    write_le(bytes, cursor, MYOS_BOOT_MAGIC, 8);
    write_le(bytes, cursor, MYOS_BOOT_MAJOR, 2);
    write_le(bytes, cursor, MYOS_BOOT_MINOR, 2);
    write_le(bytes, cursor, MYOS_BOOT_HEADER_SIZE, 4);
    write_le(bytes, cursor, BundleSize, 8);
    write_le(bytes, cursor, MYOS_BOOT_ARCH_RISCV64, 4);
    write_le(bytes, cursor, MYOS_BOOT_ABI_RISCV_LP64, 4);
    write_le(bytes, cursor, 0, 8);
    write_le(bytes, cursor, MYOS_BOOT_HEADER_SIZE, 8);
    write_le(bytes, cursor, 1, 4);
    write_le(bytes, cursor, 0, 4);
    write_le(bytes, cursor, SegmentOffset, 8);
    write_le(bytes, cursor, 1, 4);
    write_le(bytes, cursor, 0, 4);
    write_le(bytes, cursor, 0, 8);

    write_le(bytes, cursor, NameOffset, 8);
    write_le(bytes, cursor, 4, 4);
    write_le(bytes, cursor, MYOS_BOOT_MODULE_BOOTABLE, 4);
    write_le(bytes, cursor, ImageOffset, 8);
    write_le(bytes, cursor, 4, 8);
    write_le(bytes, cursor, 0x20'0000, 8);
    write_le(bytes, cursor, 0, 4);
    write_le(bytes, cursor, 1, 4);
    write_le(bytes, cursor, 0, 8);
    write_le(bytes, cursor, 0, 8);

    write_le(bytes, cursor, 0x20'0000, 8);
    write_le(bytes, cursor, ImageOffset, 8);
    write_le(bytes, cursor, 4, 8);
    write_le(bytes, cursor, 8, 8);
    write_le(bytes, cursor, kernel::mm::page_size, 8);
    write_le(
        bytes, cursor,
        MYOS_BOOT_SEGMENT_READ | MYOS_BOOT_SEGMENT_EXECUTE, 4);
    write_le(bytes, cursor, 0, 4);

    bytes[cursor++] = 'i';
    bytes[cursor++] = 'n';
    bytes[cursor++] = 'i';
    bytes[cursor++] = 't';
    bytes[cursor++] = 0x13;
    bytes[cursor++] = 0;
    bytes[cursor++] = 0;
    bytes[cursor++] = 0;
    libk_assert(cursor == BundleSize);
}

bool test_bundle_view_accepts_valid_manifest(const TestContext&) noexcept {
    byte bytes[BundleSize]{};
    build_bundle(bytes);
    auto parsed = kernel::image::parse_bundle(
        libk::ByteSpan{bytes, sizeof(bytes)});
    if (!parsed) {
        return false;
    }
    const kernel::image::BootBundle& bundle = parsed.value();
    if (bundle.root_name() != "init"
        || bundle.root_image().size() != 4
        || bundle.entry() != 0x20'0000
        || bundle.segment_count() != 1) {
        return false;
    }
    auto segment = bundle.segment(0);
    if (!segment) {
        return false;
    }
    const kernel::image::BundleSegment& view = segment.value();
    return segment
        && view.virtual_address == 0x20'0000
        && view.file.size() == 4
        && view.memory_size == 8
        && view.access.contains(kernel::mm::Access::Read)
        && view.access.contains(kernel::mm::Access::Execute)
        && !view.access.contains(kernel::mm::Access::Write);
}

bool test_bundle_view_rejects_bad_envelopes(const TestContext&) noexcept {
    byte bytes[BundleSize]{};
    build_bundle(bytes);
    bytes[0] ^= 1;
    const auto bad_magic = kernel::image::parse_bundle(
        libk::ByteSpan{bytes, sizeof(bytes)});
    bytes[0] ^= 1;
    const auto truncated = kernel::image::parse_bundle(
        libk::ByteSpan{bytes, sizeof(bytes) - 1});
    bytes[24] = 0xff;
    const auto bad_target = kernel::image::parse_bundle(
        libk::ByteSpan{bytes, sizeof(bytes)});
    return !bad_magic && bad_magic.error() == kernel::image::BundleError::BadMagic
        && !truncated
        && !bad_target
        && bad_target.error() == kernel::image::BundleError::WrongTarget;
}

bool test_bundle_view_rejects_writable_code(const TestContext&) noexcept {
    byte bytes[BundleSize]{};
    build_bundle(bytes);
    bytes[SegmentOffset + 40] = MYOS_BOOT_SEGMENT_READ
        | MYOS_BOOT_SEGMENT_WRITE | MYOS_BOOT_SEGMENT_EXECUTE;
    const auto parsed = kernel::image::parse_bundle(
        libk::ByteSpan{bytes, sizeof(bytes)});
    return !parsed
        && parsed.error() == kernel::image::BundleError::InvalidSegment;
}

} // namespace

void register_boot_bundle_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "boot-bundle", "valid manifest is a bounded borrowed view",
        test_bundle_view_accepts_valid_manifest);
    (void)registry.add(
        "boot-bundle", "bad envelope is rejected before materialization",
        test_bundle_view_rejects_bad_envelopes);
    (void)registry.add(
        "boot-bundle", "writable executable segment is rejected",
        test_bundle_view_rejects_writable_code);
}
