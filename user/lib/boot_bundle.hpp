#pragma once

#include <stddef.h>
#include <stdint.h>
#include <uapi/boot_bundle.h>

namespace myos::boot {

struct Segment final {
    uintptr_t address{};
    const uint8_t* file{};
    size_t file_size{};
    size_t memory_size{};
    size_t alignment{};
    uint32_t access{};
};

class Bytes final {
public:
    constexpr Bytes() noexcept = default;
    constexpr Bytes(const void* data, size_t size) noexcept
        : data_(static_cast<const uint8_t*>(data)), size_(size) {}

    [[nodiscard]] constexpr auto data() const noexcept -> const uint8_t* {
        return data_;
    }
    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return size_;
    }
    [[nodiscard]] auto read(size_t offset, size_t width, uint64_t& value)
        const noexcept -> bool {
        if (width > 8 || offset > size_ || width > size_ - offset) {
            return false;
        }
        value = 0;
        for (size_t index = 0; index < width; ++index) {
            value |= static_cast<uint64_t>(data_[offset + index])
                << (index * 8);
        }
        return true;
    }
    [[nodiscard]] auto slice(size_t offset, size_t size) const noexcept
        -> Bytes {
        return offset <= size_ && size <= size_ - offset
            ? Bytes{data_ + offset, size}
            : Bytes{};
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return data_ != nullptr;
    }

private:
    const uint8_t* data_{};
    size_t size_{};
};

class Bundle;

class Module final {
public:
    [[nodiscard]] auto name() const noexcept -> Bytes { return name_; }
    [[nodiscard]] auto entry() const noexcept -> uintptr_t { return entry_; }
    [[nodiscard]] auto segment_count() const noexcept -> size_t {
        return segment_count_;
    }
    [[nodiscard]] auto segment(size_t index, Segment& out) const noexcept
        -> bool {
        if (index >= segment_count_) {
            return false;
        }
        const size_t offset = segments_offset_
            + (segment_first_ + index) * MYOS_BOOT_SEGMENT_SIZE;
        uint64_t address{};
        uint64_t file_offset{};
        uint64_t file_size{};
        uint64_t memory_size{};
        uint64_t alignment{};
        uint64_t access{};
        uint64_t reserved{};
        if (!bytes_.read(offset, 8, address)
            || !bytes_.read(offset + 8, 8, file_offset)
            || !bytes_.read(offset + 16, 8, file_size)
            || !bytes_.read(offset + 24, 8, memory_size)
            || !bytes_.read(offset + 32, 8, alignment)
            || !bytes_.read(offset + 40, 4, access)
            || !bytes_.read(offset + 44, 4, reserved)
            || reserved != 0 || memory_size == 0
            || file_size > memory_size
            || file_offset > bytes_.size()
            || file_size > bytes_.size() - file_offset
            || file_offset < image_offset_
            || file_offset - image_offset_ > image_size_
            || file_size > image_size_ - (file_offset - image_offset_)) {
            return false;
        }
        out = Segment{
            .address = static_cast<uintptr_t>(address),
            .file = bytes_.data() + file_offset,
            .file_size = static_cast<size_t>(file_size),
            .memory_size = static_cast<size_t>(memory_size),
            .alignment = static_cast<size_t>(alignment),
            .access = static_cast<uint32_t>(access),
        };
        return true;
    }

private:
    friend class Bundle;
    Bytes bytes_{};
    Bytes name_{};
    size_t image_offset_{};
    size_t image_size_{};
    uintptr_t entry_{};
    size_t segments_offset_{};
    size_t segment_first_{};
    size_t segment_count_{};
};

class Bundle final {
public:
    [[nodiscard]] static auto parse(const void* data, size_t size) noexcept
        -> Bundle {
        Bundle result{};
        const Bytes bytes{data, size};
        uint64_t magic{};
        uint64_t major{};
        uint64_t minor{};
        uint64_t header_size{};
        uint64_t total_size{};
        uint64_t architecture{};
        uint64_t abi{};
        uint64_t features{};
        uint64_t modules_offset{};
        uint64_t modules_count{};
        uint64_t root_index{};
        uint64_t segments_offset{};
        uint64_t segments_count{};
        uint64_t reserved{};
        uint64_t checksum{};
        if (!bytes.read(0, 8, magic)
            || !bytes.read(8, 2, major)
            || !bytes.read(10, 2, minor)
            || !bytes.read(12, 4, header_size)
            || !bytes.read(16, 8, total_size)
            || !bytes.read(24, 4, architecture)
            || !bytes.read(28, 4, abi)
            || !bytes.read(32, 8, features)
            || !bytes.read(40, 8, modules_offset)
            || !bytes.read(48, 4, modules_count)
            || !bytes.read(52, 4, root_index)
            || !bytes.read(56, 8, segments_offset)
            || !bytes.read(64, 4, segments_count)
            || !bytes.read(68, 4, reserved)
            || !bytes.read(72, 8, checksum)
            || magic != MYOS_BOOT_MAGIC
            || major != MYOS_BOOT_MAJOR || minor > MYOS_BOOT_MINOR
            || header_size != MYOS_BOOT_HEADER_SIZE
            || total_size != size
            || architecture != MYOS_BOOT_ARCH_RISCV64
            || abi != MYOS_BOOT_ABI_RISCV_LP64
            || features != 0 || reserved != 0 || checksum != 0
            || modules_count == 0 || modules_count > 32
            || root_index >= modules_count
            || modules_offset > size
            || modules_count > (size - modules_offset) / MYOS_BOOT_MODULE_SIZE
            || segments_offset > size
            || segments_count
                > (size - segments_offset) / MYOS_BOOT_SEGMENT_SIZE) {
            return {};
        }
        result.bytes_ = bytes;
        result.modules_offset_ = static_cast<size_t>(modules_offset);
        result.module_count_ = static_cast<size_t>(modules_count);
        result.root_index_ = static_cast<size_t>(root_index);
        result.segments_offset_ = static_cast<size_t>(segments_offset);
        result.segment_count_ = static_cast<size_t>(segments_count);
        return result;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(bytes_);
    }
    [[nodiscard]] auto module_count() const noexcept -> size_t {
        return module_count_;
    }
    [[nodiscard]] auto root_index() const noexcept -> size_t {
        return root_index_;
    }
    [[nodiscard]] auto module(size_t index, Module& out) const noexcept
        -> bool {
        if (index >= module_count_) {
            return false;
        }
        const size_t offset = modules_offset_
            + index * MYOS_BOOT_MODULE_SIZE;
        uint64_t name_offset{};
        uint64_t name_size{};
        uint64_t flags{};
        uint64_t image_offset{};
        uint64_t image_size{};
        uint64_t entry{};
        uint64_t segment_first{};
        uint64_t segment_count{};
        uint64_t tls_offset{};
        uint64_t tls_size{};
        if (!bytes_.read(offset, 8, name_offset)
            || !bytes_.read(offset + 8, 4, name_size)
            || !bytes_.read(offset + 12, 4, flags)
            || !bytes_.read(offset + 16, 8, image_offset)
            || !bytes_.read(offset + 24, 8, image_size)
            || !bytes_.read(offset + 32, 8, entry)
            || !bytes_.read(offset + 40, 4, segment_first)
            || !bytes_.read(offset + 44, 4, segment_count)
            || !bytes_.read(offset + 48, 8, tls_offset)
            || !bytes_.read(offset + 56, 8, tls_size)
            || flags != MYOS_BOOT_MODULE_BOOTABLE
            || name_size == 0 || tls_offset != 0 || tls_size != 0
            || name_offset > bytes_.size()
            || name_size > bytes_.size() - name_offset
            || image_offset > bytes_.size()
            || image_size > bytes_.size() - image_offset
            || segment_first > segment_count_
            || segment_count > segment_count_ - segment_first) {
            return false;
        }
        Module decoded{};
        decoded.bytes_ = bytes_;
        decoded.name_ = bytes_.slice(name_offset, name_size);
        decoded.image_offset_ = static_cast<size_t>(image_offset);
        decoded.image_size_ = static_cast<size_t>(image_size);
        decoded.entry_ = static_cast<uintptr_t>(entry);
        decoded.segments_offset_ = segments_offset_;
        decoded.segment_first_ = static_cast<size_t>(segment_first);
        decoded.segment_count_ = static_cast<size_t>(segment_count);
        out = decoded;
        return true;
    }
    [[nodiscard]] auto find(const char* name, Module& out) const noexcept
        -> bool {
        size_t length{};
        while (name[length] != '\0') {
            ++length;
        }
        for (size_t index = 0; index < module_count_; ++index) {
            Module candidate{};
            if (!module(index, candidate) || candidate.name().size() != length) {
                continue;
            }
            size_t position{};
            while (position < length
                && candidate.name().data()[position]
                    == static_cast<uint8_t>(name[position])) {
                ++position;
            }
            if (position == length) {
                out = candidate;
                return true;
            }
        }
        return false;
    }

private:
    Bytes bytes_{};
    size_t modules_offset_{};
    size_t module_count_{};
    size_t root_index_{};
    size_t segments_offset_{};
    size_t segment_count_{};
};

} // namespace myos::boot
