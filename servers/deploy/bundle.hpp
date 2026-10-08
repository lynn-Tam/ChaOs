#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>
#include <sys/handle.hpp>
#include <servers/deploy/format.h>
#include <uapi/riscv64.h>
#include <uapi/boot_bundle.h>

namespace boot {

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
    template<size_t N>
    [[nodiscard]] auto equals(const char (&name)[N]) const noexcept -> bool {
        static_assert(N != 0);
        if (size_ != N - 1) {
            return false;
        }
        for (size_t index = 0; index < N - 1; ++index) {
            if (data_[index] != static_cast<uint8_t>(name[index])) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] constexpr auto equals(Bytes other) const noexcept -> bool {
        if (size_ != other.size_) {
            return false;
        }
        for (size_t index = 0; index < size_; ++index) {
            if (data_[index] != other.data_[index]) {
                return false;
            }
        }
        return true;
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
    [[nodiscard]] constexpr auto flags() const noexcept -> uint32_t {
        return flags_;
    }
    [[nodiscard]] constexpr auto bootable() const noexcept -> bool {
        return flags_ == BUNDLE_MODULE_BOOTABLE;
    }
    [[nodiscard]] constexpr auto data_module() const noexcept -> bool {
        return flags_ == BUNDLE_MODULE_DATA;
    }
    [[nodiscard]] auto entry() const noexcept -> uintptr_t { return entry_; }
    [[nodiscard]] auto data() const noexcept -> Bytes {
        return data_module() ? bytes_.slice(image_offset_, image_size_)
                             : Bytes{};
    }
    [[nodiscard]] auto segment_count() const noexcept -> size_t {
        return segment_count_;
    }
    [[nodiscard]] auto segment(size_t index, Segment& out) const noexcept
        -> bool {
        if (index >= segment_count_) {
            return false;
        }
        if (segment_first_ > segment_table_count_
            || index > segment_table_count_ - segment_first_) {
            return false;
        }
        const size_t relative = segment_first_ + index;
        if (relative > (bytes_.size() - segments_offset_)
                / BUNDLE_SEGMENT_SIZE) {
            return false;
        }
        const size_t offset = segments_offset_
            + relative * BUNDLE_SEGMENT_SIZE;
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
            || alignment == 0 || (alignment & (alignment - 1)) != 0
            || alignment < 4096
            || alignment > RISCV64_LOWER_CANONICAL_END
            || (address & 4095) != 0
            || address < RISCV64_LOW_GUARD_END
            || address >= RISCV64_LOWER_CANONICAL_END
            || memory_size > RISCV64_LOWER_CANONICAL_END - address
            || (access & ~static_cast<uint64_t>(
                BUNDLE_SEGMENT_READ | BUNDLE_SEGMENT_WRITE
                    | BUNDLE_SEGMENT_EXECUTE)) != 0
            || access == 0
            || ((access & BUNDLE_SEGMENT_WRITE) != 0
                && (access & BUNDLE_SEGMENT_READ) == 0)
            || ((access & BUNDLE_SEGMENT_WRITE) != 0
                && (access & BUNDLE_SEGMENT_EXECUTE) != 0)
            || file_offset > bytes_.size()
            || file_size > bytes_.size() - file_offset
            || file_offset < image_offset_
            || file_offset - image_offset_ > image_size_
            || file_size > image_size_ - (file_offset - image_offset_)) {
            return false;
        }
        const uint64_t rounded_memory =
            (memory_size + UINT64_C(4095)) / UINT64_C(4096)
            * UINT64_C(4096);
        if (rounded_memory > RISCV64_LOWER_CANONICAL_END - address) {
            return false;
        }
        if ((address % alignment)
            != ((file_offset - image_offset_) % alignment)) {
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
    uint32_t flags_{};
    size_t image_offset_{};
    size_t image_size_{};
    uintptr_t entry_{};
    size_t segments_offset_{};
    size_t segment_first_{};
    size_t segment_count_{};
    size_t segment_table_count_{};
};

class Bundle final {
public:
    [[nodiscard]] static auto parse(const void* data, size_t size) noexcept
        -> Bundle {
        Bundle result{};
        const Bytes bytes{data, size};
        if (data == nullptr && size != 0) {
            return result;
        }
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
            || magic != BUNDLE_MAGIC
            || major != BUNDLE_MAJOR || minor > BUNDLE_MINOR
            || header_size != BUNDLE_HEADER_SIZE
            || total_size != size
            || architecture != BUNDLE_ARCH_RISCV64
            || abi != BUNDLE_ABI_RISCV_LP64
            || features != 0 || reserved != 0 || checksum != 0
            || modules_count == 0 || modules_count > 32
            || root_index >= modules_count
            || modules_offset > size
            || modules_count > (size - modules_offset) / BUNDLE_MODULE_SIZE
            || segments_offset > size
            || segments_count
                > (size - segments_offset) / BUNDLE_SEGMENT_SIZE) {
            return {};
        }
        result.bytes_ = bytes;
        result.modules_offset_ = static_cast<size_t>(modules_offset);
        result.module_count_ = static_cast<size_t>(modules_count);
        result.root_index_ = static_cast<size_t>(root_index);
        result.segments_offset_ = static_cast<size_t>(segments_offset);
        result.segment_count_ = static_cast<size_t>(segments_count);
        result.minor_ = static_cast<uint16_t>(minor);
        if (!result.validate_modules()) {
            return {};
        }
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
    template<size_t N>
    [[nodiscard]] auto root_is(const char (&name)[N]) const noexcept -> bool {
        Module root{};
        return module(root_index_, root) && root.name().equals(name);
    }
    [[nodiscard]] auto module(size_t index, Module& out) const noexcept
        -> bool {
        if (index >= module_count_) {
            return false;
        }
        const size_t offset = modules_offset_
            + index * BUNDLE_MODULE_SIZE;
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
            || (flags != BUNDLE_MODULE_BOOTABLE
                && flags != BUNDLE_MODULE_DATA)
            || name_size == 0 || tls_offset != 0 || tls_size != 0
            || name_offset > bytes_.size()
            || name_size > bytes_.size() - name_offset
            || image_offset > bytes_.size()
            || image_size == 0
            || image_size > bytes_.size() - image_offset
            || segment_first > segment_count_
            || segment_count > segment_count_ - segment_first) {
            return false;
        }
        for (size_t byte = 0; byte < name_size; ++byte) {
            if (bytes_.data()[name_offset + byte] == 0) {
                return false;
            }
        }
        Module decoded{};
        decoded.bytes_ = bytes_;
        decoded.name_ = bytes_.slice(name_offset, name_size);
        decoded.flags_ = static_cast<uint32_t>(flags);
        decoded.image_offset_ = static_cast<size_t>(image_offset);
        decoded.image_size_ = static_cast<size_t>(image_size);
        decoded.entry_ = static_cast<uintptr_t>(entry);
        decoded.segments_offset_ = segments_offset_;
        decoded.segment_first_ = static_cast<size_t>(segment_first);
        decoded.segment_count_ = static_cast<size_t>(segment_count);
        decoded.segment_table_count_ = segment_count_;
        out = decoded;
        return true;
    }
    template<size_t N>
    [[nodiscard]] auto find(const char (&name)[N], Module& out) const noexcept
        -> bool {
        for (size_t index = 0; index < module_count_; ++index) {
            Module candidate{};
            if (!module(index, candidate) || !candidate.name().equals(name)) {
                continue;
            }
            out = candidate;
            return true;
        }
        return false;
    }
    [[nodiscard]] auto find(Bytes name, Module& out) const noexcept -> bool {
        if (!name) {
            return false;
        }
        for (size_t index = 0; index < module_count_; ++index) {
            Module candidate{};
            if (!module(index, candidate) || !candidate.name().equals(name)) {
                continue;
            }
            out = candidate;
            return true;
        }
        return false;
    }

private:
    [[nodiscard]] auto validate_modules() const noexcept -> bool {
        size_t expected_segment{};
        size_t data_modules{};
        for (size_t module_index = 0; module_index < module_count_;
             ++module_index) {
            Module candidate{};
            if (!module(module_index, candidate)) {
                return false;
            }
            if (candidate.data_module()) {
                if (minor_ == 0) {
                    return false;
                }
                ++data_modules;
                if (candidate.entry() != 0 || candidate.segment_count() != 0
                    || candidate.segment_first_ != expected_segment
                    || candidate.data().size() == 0) {
                    return false;
                }
                continue;
            }
            if (candidate.segment_count() == 0
                || candidate.segment_count() > 32
                || candidate.segment_count()
                    > segment_count_ - expected_segment
                || candidate.segment_first_ != expected_segment) {
                return false;
            }
            expected_segment += candidate.segment_count();
            size_t previous_end{};
            bool entry_covered{};
            for (size_t index = 0; index < candidate.segment_count(); ++index) {
                Segment segment{};
                if (!candidate.segment(index, segment)) {
                    return false;
                }
                const size_t rounded =
                    (segment.memory_size + 4095) & ~size_t{4095};
                if (index != 0 && segment.address < previous_end) {
                    return false;
                }
                previous_end = segment.address + rounded;
                if (candidate.entry() >= segment.address
                    && candidate.entry() - segment.address
                        < segment.memory_size
                    && (segment.access & BUNDLE_SEGMENT_EXECUTE) != 0) {
                    entry_covered = true;
                }
            }
            if (!entry_covered) {
                return false;
            }
        }
        Module root{};
        return expected_segment == segment_count_ && data_modules <= 1
            && module(root_index_, root) && root.bootable();
    }

    Bytes bytes_{};
    size_t modules_offset_{};
    size_t module_count_{};
    size_t root_index_{};
    size_t segments_offset_{};
    size_t segment_count_{};
    uint16_t minor_{};
};

} // namespace boot

namespace deploy {
[[nodiscard]] constexpr auto committed(status_t status) noexcept -> bool {
    return status == STATUS_OK || status == STATUS_PENDING;
}

[[nodiscard]] constexpr auto retryable(status_t status) noexcept -> bool {
    return status == STATUS_BUSY || status == STATUS_RETRY;
}

struct Window final {
    word_t address{};
    word_t size{};

    /* Mapping callers use this checked rounding operation before constructing
     * a page-aligned window.  Zero represents overflow or an empty request. */
    [[nodiscard]] static constexpr auto round_size(word_t value) noexcept
        -> word_t {
        constexpr word_t page_size = DEPLOY_PAGE_SIZE;
        return value <= static_cast<word_t>(-1) - (page_size - 1)
            ? (value + page_size - 1) & ~(page_size - 1)
            : 0;
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return address != 0 && size != 0
            && (address % DEPLOY_PAGE_SIZE) == 0
            && (size % DEPLOY_PAGE_SIZE) == 0
            && size <= ~word_t{} - address;
    }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return address == 0 && size == 0;
    }

    [[nodiscard]] constexpr auto end() const noexcept -> word_t {
        return valid() ? address + size : 0;
    }
};

[[nodiscard]] constexpr auto windows_disjoint(
    Window first, Window second) noexcept -> bool {
    if (first.empty() || second.empty()) {
        return true;
    }
    if (!first.valid() || !second.valid()) {
        return false;
    }
    return first.end() <= second.address || second.end() <= first.address;
}

enum class MapState : uint8_t { Empty, Ready, Mapped, Closing, Closed };

template<typename B = sys::cap::SyscallBackend>
class Map final {
    using Owner = sys::cap::BasicOwnedCap<B>;
    Owner region_{};
    Window window_{};
    word_t mapped_size_{};
    MapState state_{};
public:
    Map() noexcept = default;
    Map(const Map&) = delete;
    auto operator=(const Map&) -> Map& = delete;
    Map(Map&& other) noexcept
        : region_(std::move(other.region_)), window_(other.window_),
          mapped_size_(std::exchange(other.mapped_size_, 0)),
          state_(std::exchange(other.state_, MapState::Closed)) {}
    auto operator=(Map&& other) noexcept -> Map& {
        if (this == &other) return *this;
        const auto status = close();
        if (status != STATUS_OK) B::ownership_fault(status);
        region_ = std::move(other.region_);
        window_ = other.window_;
        mapped_size_ = std::exchange(other.mapped_size_, 0);
        state_ = std::exchange(other.state_, MapState::Closed);
        return *this;
    }
    ~Map() noexcept {
        const auto status = close();
        if (status != STATUS_OK) B::ownership_fault(status);
    }
    auto open(sys::cap::CapRef root, Window window, Window forbidden = {},
              word_t perms = VM_READ | VM_WRITE) noexcept -> status_t {
        if (region_) return STATUS_BUSY;
        if (!root || root.cspace || !window.valid() || !windows_disjoint(window, forbidden))
            return STATUS_BAD_ARGS;
        const auto created = B::vm_slice(root, window.address, window.size, perms,
                                         RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
        if (created.value) region_ = Owner{{created.value, 0}};
        if (region_) { window_ = window; state_ = MapState::Ready; }
        return created.status == STATUS_OK && !region_ ? STATUS_INVALID_CAP : created.status;
    }
    auto map(sys::cap::CapRef memory, word_t first, word_t size, word_t perms) noexcept -> status_t {
        if (state_ != MapState::Ready || !memory || memory.cspace || !size
            || size % DEPLOY_PAGE_SIZE || size > window_.size
            || !perms || (perms & ~(VM_READ | VM_WRITE)) || !(perms & VM_READ))
            return STATUS_BAD_ARGS;
        const auto status = B::vm_map(region_.reference(), memory, window_.address, size, first, perms);
        if (!committed(status)) return status;
        mapped_size_ = size;
        state_ = MapState::Mapped;
        return STATUS_OK;
    }
    auto unmap() noexcept -> status_t {
        if (state_ != MapState::Mapped) return STATUS_BAD_ARGS;
        const auto status = B::vm_unmap(region_.reference(), window_.address, mapped_size_);
        if (!committed(status)) return status;
        mapped_size_ = 0;
        state_ = MapState::Ready;
        return STATUS_OK;
    }
    auto close() noexcept -> status_t {
        if (!region_) return STATUS_OK;
        if (mapped()) {
            const auto status = unmap();
            if (status != STATUS_OK) return status;
        }
        if (state_ == MapState::Ready) {
            const auto status = B::vm_clear(region_.reference());
            if (!committed(status)) return status;
            state_ = MapState::Closing;
        }
        const auto status = region_.close();
        if (status == STATUS_OK) state_ = MapState::Closed;
        return status;
    }
    auto phase() const noexcept -> MapState { return state_; }
    auto mapped() const noexcept -> bool { return state_ == MapState::Mapped; }
    auto reusable() const noexcept -> bool { return state_ == MapState::Ready; }
    auto address() const noexcept -> word_t { return mapped() ? window_.address : 0; }
};

template<typename B = sys::cap::SyscallBackend>
class BundleMap final {
    Map<B> map_{};
    boot::Bundle view_{};
    size_t size_{};
public:
    auto open(sys::cap::CapRef root, sys::cap::CapRef memory, Window window,
              size_t size, Window forbidden = {}) noexcept -> status_t {
        if (!size || size > window.size) return STATUS_BAD_ARGS;
        auto status = map_.open(root, window, forbidden, VM_READ);
        if (status != STATUS_OK) return status;
        status = map_.map(memory, 0, window.size, VM_READ);
        if (status != STATUS_OK) return status;
        view_ = boot::Bundle::parse(reinterpret_cast<const void*>(window.address), size);
        size_ = size;
        return view_ ? STATUS_OK : STATUS_BAD_ARGS;
    }
    auto close() noexcept -> status_t { view_ = {}; return map_.close(); }
    auto view() const noexcept -> const boot::Bundle* { return map_.mapped() && view_ ? &view_ : nullptr; }
    auto size() const noexcept -> size_t { return map_.mapped() ? size_ : 0; }
    auto phase() const noexcept -> MapState { return map_.phase(); }
};

} // namespace deploy
