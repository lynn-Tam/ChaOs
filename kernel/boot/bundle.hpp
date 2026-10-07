#pragma once

#include <base/types.hpp>
#include <expected>
#include <libk/span.hpp>
#include <libk/string_view.hpp>
#include <mm/types.hpp>
#include <uapi/boot_bundle.h>

inline constexpr usize max_boot_segments = 16;
inline constexpr usize max_boot_modules = 32;

enum class BundleError : u8 {
    Truncated,
    BadMagic,
    BadVersion,
    WrongTarget,
    UnsupportedFeatures,
    InvalidTable,
    InvalidModule,
    InvalidSegment,
    InvalidEntry,
    InvalidRole,
};

struct BundleSegment final {
    usize virtual_address{};
    libk::ByteSpan file{};
    usize memory_size{};
    usize alignment{};
    mm::Perms perms{};
};

class BootBundle;

class BundleModule final {
public:
    [[nodiscard]] auto name() const noexcept -> libk::StrView {
        return name_;
    }
    [[nodiscard]] auto payload() const noexcept -> libk::ByteSpan {
        return payload_;
    }
    [[nodiscard]] auto flags() const noexcept -> u32 { return flags_; }
    [[nodiscard]] auto bootable() const noexcept -> bool {
        return flags_ == BUNDLE_MODULE_BOOTABLE;
    }
    [[nodiscard]] auto data_module() const noexcept -> bool {
        return flags_ == BUNDLE_MODULE_DATA;
    }
    [[nodiscard]] auto entry() const noexcept -> usize { return entry_; }
    [[nodiscard]] auto segment_count() const noexcept -> usize {
        return segment_count_;
    }
    [[nodiscard]] auto segment(usize index) const noexcept
        -> std::expected<BundleSegment, BundleError>;

private:
    friend class BootBundle;
    friend auto parse_bundle(libk::ByteSpan bytes) noexcept
        -> std::expected<BootBundle, BundleError>;

    BundleModule() noexcept = default;

    BundleModule(
        libk::ByteSpan bytes,
        libk::StrView name,
        libk::ByteSpan image,
        u32 flags,
        usize entry,
        usize segments_offset,
        usize segment_first,
        usize segment_count) noexcept
        : bytes_(bytes),
          name_(name),
          payload_(image),
          flags_(flags),
          entry_(entry),
          segments_offset_(segments_offset),
          segment_first_(segment_first),
          segment_count_(segment_count) {}

    [[nodiscard]] auto segment_first() const noexcept -> usize {
        return segment_first_;
    }

    libk::ByteSpan bytes_{};
    libk::StrView name_{};
    libk::ByteSpan payload_{};
    u32 flags_{};
    usize entry_{};
    usize segments_offset_{};
    usize segment_first_{};
    usize segment_count_{};
};

class BootBundle final {
public:
    [[nodiscard]] auto bytes() const noexcept -> libk::ByteSpan {
        return bytes_;
    }
    [[nodiscard]] auto root_name() const noexcept -> libk::StrView {
        return root_.name();
    }
    [[nodiscard]] auto root_bytes() const noexcept -> libk::ByteSpan {
        return root_.payload();
    }
    [[nodiscard]] auto entry() const noexcept -> usize { return root_.entry(); }
    [[nodiscard]] auto segment_count() const noexcept -> usize {
        return root_.segment_count();
    }
    [[nodiscard]] auto segment(usize index) const noexcept
        -> std::expected<BundleSegment, BundleError> {
        return root_.segment(index);
    }
    [[nodiscard]] auto module_count() const noexcept -> usize {
        return module_count_;
    }
    [[nodiscard]] auto root_index() const noexcept -> usize {
        return root_index_;
    }
    [[nodiscard]] auto module(usize index) const noexcept
        -> std::expected<BundleModule, BundleError>;

private:
    friend auto parse_bundle(libk::ByteSpan bytes) noexcept
        -> std::expected<BootBundle, BundleError>;

    libk::ByteSpan bytes_{};
    BundleModule root_{};
    usize modules_offset_{};
    usize module_count_{};
    usize root_index_{};
    usize segments_offset_{};
};

[[nodiscard]] auto parse_bundle(libk::ByteSpan bytes) noexcept
    -> std::expected<BootBundle, BundleError>;

