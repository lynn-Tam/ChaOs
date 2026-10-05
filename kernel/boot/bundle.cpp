#include <expected>
#include <utility>
#include <boot/bundle.hpp>
#include <bit>
#include <libk/byte_reader.hpp>
#include <libk/checked_arithmetic.hpp>
#include <mm/types.hpp>
#include <mm/table.hpp>
#include <uapi/boot_bundle.h>

namespace {

struct Header final {
    u64 total_size{};
    u16 minor{};
    u64 modules_offset{};
    u32 modules_count{};
    u32 root_module{};
    u64 segments_offset{};
    u32 segments_count{};
};

struct Module final {
    u64 name_offset{};
    u32 name_size{};
    u32 flags{};
    u64 image_offset{};
    u64 image_size{};
    u64 entry{};
    u32 segment_first{};
    u32 segment_count{};
};

[[nodiscard]] auto reader_at(libk::ByteSpan bytes, usize offset) noexcept
    -> std::expected<libk::ByteReader, BundleError> {
    libk::ByteReader reader{bytes.data(), bytes.size()};
    if (!reader.skip(offset)) {
        return std::unexpected(BundleError::Truncated);
    }
    return reader;
}

[[nodiscard]] auto bytes_at(
    libk::ByteSpan bytes,
    u64 offset,
    u64 size) noexcept -> std::expected<libk::ByteSpan, BundleError> {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        return std::unexpected(BundleError::InvalidTable);
    }
    return (bytes.slice(
        static_cast<usize>(offset), static_cast<usize>(size)));
}

[[nodiscard]] auto table_fits(
    usize bytes,
    u64 offset,
    u32 count,
    usize entry_size) noexcept -> bool {
    const auto table_size = libk::checked_multiply<usize>(count, entry_size);
    return table_size
        && offset <= bytes
        && *table_size <= bytes - static_cast<usize>(offset);
}

[[nodiscard]] auto read_header(libk::ByteSpan bytes) noexcept
    -> std::expected<Header, BundleError> {
    if (bytes.size() < MYOS_BOOT_HEADER_SIZE) {
        return std::unexpected(BundleError::Truncated);
    }
    libk::ByteReader reader{bytes.data(), bytes.size()};
    u64 magic{};
    u16 major{};
    u16 minor{};
    u32 header_size{};
    u32 architecture{};
    u32 abi{};
    u64 features{};
    Header header{};
    u32 reserved{};
    u64 checksum{};
    if (!reader.read_le64(magic)
        || !reader.read_le16(major)
        || !reader.read_le16(minor)
        || !reader.read_le32(header_size)
        || !reader.read_le64(header.total_size)
        || !reader.read_le32(architecture)
        || !reader.read_le32(abi)
        || !reader.read_le64(features)
        || !reader.read_le64(header.modules_offset)
        || !reader.read_le32(header.modules_count)
        || !reader.read_le32(header.root_module)
        || !reader.read_le64(header.segments_offset)
        || !reader.read_le32(header.segments_count)
        || !reader.read_le32(reserved)
        || !reader.read_le64(checksum)) {
        return std::unexpected(BundleError::Truncated);
    }
    header.minor = minor;
    if (magic != MYOS_BOOT_MAGIC) {
        return std::unexpected(BundleError::BadMagic);
    }
    if (major != MYOS_BOOT_MAJOR || minor > MYOS_BOOT_MINOR
        || header_size != MYOS_BOOT_HEADER_SIZE) {
        return std::unexpected(BundleError::BadVersion);
    }
    if (architecture != MYOS_BOOT_ARCH_RISCV64
        || abi != MYOS_BOOT_ABI_RISCV_LP64) {
        return std::unexpected(BundleError::WrongTarget);
    }
    if (features != 0) {
        return std::unexpected(BundleError::UnsupportedFeatures);
    }
    if (header.total_size != bytes.size() || reserved != 0 || checksum != 0
        || header.modules_count == 0
        || header.root_module >= header.modules_count
        || !table_fits(
            bytes.size(), header.modules_offset, header.modules_count,
            MYOS_BOOT_MODULE_SIZE)
        || !table_fits(
            bytes.size(), header.segments_offset, header.segments_count,
            MYOS_BOOT_SEGMENT_SIZE)) {
        return std::unexpected(BundleError::InvalidTable);
    }
    return (header);
}

[[nodiscard]] auto read_module(
    libk::ByteSpan bytes,
    usize offset) noexcept -> std::expected<Module, BundleError> {
    auto source = reader_at(bytes, offset);
    if (!source) {
        return std::unexpected(source.error());
    }
    libk::ByteReader reader = std::move(source).value();
    Module module{};
    u64 tls_offset{};
    u64 tls_size{};
    if (!reader.read_le64(module.name_offset)
        || !reader.read_le32(module.name_size)
        || !reader.read_le32(module.flags)
        || !reader.read_le64(module.image_offset)
        || !reader.read_le64(module.image_size)
        || !reader.read_le64(module.entry)
        || !reader.read_le32(module.segment_first)
        || !reader.read_le32(module.segment_count)
        || !reader.read_le64(tls_offset)
        || !reader.read_le64(tls_size)) {
        return std::unexpected(BundleError::Truncated);
    }
    if ((module.flags != MYOS_BOOT_MODULE_BOOTABLE
            && module.flags != MYOS_BOOT_MODULE_DATA)
        || module.name_size == 0 || tls_offset != 0 || tls_size != 0) {
        return std::unexpected(BundleError::InvalidRole);
    }
    return (module);
}

[[nodiscard]] auto decode_segment(
    libk::ByteSpan bytes,
    usize offset) noexcept
    -> std::expected<BundleSegment, BundleError> {
    auto source = reader_at(bytes, offset);
    if (!source) {
        return std::unexpected(source.error());
    }
    libk::ByteReader reader = std::move(source).value();
    u64 virtual_address{};
    u64 file_offset{};
    u64 file_size{};
    u64 memory_size{};
    u64 alignment{};
    u32 access_bits{};
    u32 reserved{};
    if (!reader.read_le64(virtual_address)
        || !reader.read_le64(file_offset)
        || !reader.read_le64(file_size)
        || !reader.read_le64(memory_size)
        || !reader.read_le64(alignment)
        || !reader.read_le32(access_bits)
        || !reader.read_le32(reserved)) {
        return std::unexpected(BundleError::Truncated);
    }
    constexpr u32 known_access = MYOS_BOOT_SEGMENT_READ
        | MYOS_BOOT_SEGMENT_WRITE | MYOS_BOOT_SEGMENT_EXECUTE;
    if (memory_size == 0 || file_size > memory_size || alignment == 0
        || !std::has_single_bit(static_cast<usize>(alignment))
        || alignment < mm::page_size
        || (virtual_address & (mm::page_size - 1)) != 0
        || alignment > mm::UserEnd
        || (access_bits & ~known_access) != 0
        || ((access_bits & MYOS_BOOT_SEGMENT_WRITE) != 0
            && (access_bits & MYOS_BOOT_SEGMENT_EXECUTE) != 0)
        || reserved != 0
        || virtual_address > mm::UserEnd
        || memory_size > mm::UserEnd - virtual_address
        || !mm::is_user(mm::Virt{virtual_address})
        || !mm::is_user(
            mm::Virt{virtual_address + memory_size - 1})) {
        return std::unexpected(BundleError::InvalidSegment);
    }
    auto file = bytes_at(bytes, file_offset, file_size);
    if (!file) {
        return std::unexpected(BundleError::InvalidSegment);
    }
    const mm::Perms access =
        mm::Perms::from_raw(static_cast<u8>(access_bits));
    if (!mm::valid_perms(access)) {
        return std::unexpected(BundleError::InvalidSegment);
    }
    return (BundleSegment{
        .virtual_address = static_cast<usize>(virtual_address),
        .file = file.value(),
        .memory_size = static_cast<usize>(memory_size),
        .alignment = static_cast<usize>(alignment),
        .access = access,
    });
}

} // namespace

auto BundleModule::segment(usize index) const noexcept
    -> std::expected<BundleSegment, BundleError> {
    if (index >= segment_count_) {
        return std::unexpected(BundleError::InvalidSegment);
    }
    const auto relative = libk::checked_add(segment_first_, index);
    const auto byte_offset = relative
        ? libk::checked_multiply(*relative, usize{MYOS_BOOT_SEGMENT_SIZE})
        : std::nullopt;
    const auto absolute = byte_offset
        ? libk::checked_add(segments_offset_, *byte_offset)
        : std::nullopt;
    if (!absolute) {
        return std::unexpected(BundleError::InvalidSegment);
    }
    return decode_segment(bytes_, *absolute);
}

auto BootBundle::module(usize index) const noexcept
    -> std::expected<BundleModule, BundleError> {
    if (index >= module_count_) {
        return std::unexpected(BundleError::InvalidModule);
    }
    const usize offset = modules_offset_ + index * MYOS_BOOT_MODULE_SIZE;
    auto parsed = read_module(bytes_, offset);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const Module raw = parsed.value();
    auto name = bytes_at(bytes_, raw.name_offset, raw.name_size);
    auto image = bytes_at(bytes_, raw.image_offset, raw.image_size);
    if (!name || !image) {
        return std::unexpected(BundleError::InvalidModule);
    }
    return (BundleModule{
        bytes_,
        libk::StrView{
            reinterpret_cast<const char*>(name.value().data()),
            name.value().size()},
        image.value(),
        raw.flags,
        static_cast<usize>(raw.entry),
        segments_offset_,
        raw.segment_first,
        raw.segment_count,
    });
}

auto parse_bundle(libk::ByteSpan bytes) noexcept
    -> std::expected<BootBundle, BundleError> {
    auto parsed_header = read_header(bytes);
    if (!parsed_header) {
        return std::unexpected(parsed_header.error());
    }
    const Header header = parsed_header.value();
    if (header.modules_count > max_boot_modules) {
        return std::unexpected(BundleError::InvalidTable);
    }

    BootBundle bundle{};
    bundle.bytes_ = bytes;
    bundle.modules_offset_ = static_cast<usize>(header.modules_offset);
    bundle.module_count_ = header.modules_count;
    bundle.root_index_ = header.root_module;
    bundle.segments_offset_ = static_cast<usize>(header.segments_offset);

    usize expected_segment{};
    usize data_modules{};
    for (usize module_index = 0;
         module_index < bundle.module_count();
         ++module_index) {
        auto decoded = bundle.module(module_index);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        const BundleModule module = decoded.value();
        if (module.data_module()) {
            if (header.minor == 0) {
                return std::unexpected(BundleError::BadVersion);
            }
            ++data_modules;
            if (module.entry() != 0 || module.segment_count() != 0
                || module.segment_first() != expected_segment
                || module.payload().empty()) {
                return std::unexpected(BundleError::InvalidModule);
            }
            if (module_index == bundle.root_index()) {
                return std::unexpected(BundleError::InvalidRole);
            }
            continue;
        }
        if (module.segment_first() != expected_segment
            || module.segment_count() == 0
            || module.segment_count() > max_boot_segments
            || module.segment_count()
                > header.segments_count - expected_segment) {
            return std::unexpected(BundleError::InvalidModule);
        }
        expected_segment += module.segment_count();

        bool entry_covered{};
        usize previous_end{};
        for (usize index = 0; index < module.segment_count(); ++index) {
            auto parsed_segment = module.segment(index);
            if (!parsed_segment) {
                return std::unexpected(parsed_segment.error());
            }
            const BundleSegment segment = parsed_segment.value();
            const usize file_offset = static_cast<usize>(
                segment.file.data() - bundle.bytes().data());
            const usize image_offset = static_cast<usize>(
                module.payload().data() - bundle.bytes().data());
            if (file_offset < image_offset
                || file_offset - image_offset > module.payload().size()
                || segment.file.size()
                    > module.payload().size() - (file_offset - image_offset)) {
                return std::unexpected(BundleError::InvalidSegment);
            }
            const usize rounded_size = (segment.memory_size
                + mm::page_size - 1)
                & ~(mm::page_size - 1);
            if (index != 0 && segment.virtual_address < previous_end) {
                return std::unexpected(BundleError::InvalidSegment);
            }
            previous_end = segment.virtual_address + rounded_size;
            if (module.entry() >= segment.virtual_address
                && module.entry() - segment.virtual_address
                    < segment.memory_size
                && segment.access.contains(mm::Perm::Execute)) {
                entry_covered = true;
            }
        }
        if (!entry_covered) {
            return std::unexpected(BundleError::InvalidEntry);
        }
        if (module_index == bundle.root_index()) {
            bundle.root_ = module;
        }
    }
    if (expected_segment != header.segments_count || data_modules > 1
        || !bundle.root_.bootable()) {
        return std::unexpected(BundleError::InvalidTable);
    }
    return (bundle);
}

