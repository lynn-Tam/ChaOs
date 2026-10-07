#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/checked_arithmetic.hpp>
#include <expected>
#include <optional>
#include <uapi/capability.h>
#include <uapi/channel.h>
#include <uapi/start.h>
#include <servers/deploy/format.h>
#include <uapi/endpoint.h>
#include <uapi/object.h>
#include <uapi/ipc.h>
#include <uapi/thread.h>
#include <uapi/vm.h>

#include <servers/deploy/bundle.hpp>
#include <sys/start.hpp>


/*
 * Userspace admission relation for the public capability attenuation ABI.
 *
 * This is deliberately expressed only in the UAPI descriptor vocabulary.  It
 * is shared by ManifestView and the authority/import path; the kernel
 * still decodes and checks the actual source authority at every syscall.
 */



namespace deploy::attenuation {

enum class DescriptorForm : uint8_t {
    Ceiling,
    DuplicateRequest,
    TypedRequest,
};

[[nodiscard]] constexpr auto zero_words(
    const CapView& value,
    uint32_t first) noexcept -> bool {
    for (uint32_t index = first; index < 6; ++index) {
        if (value.words[index] != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr auto valid_kind(uint16_t kind) noexcept -> bool {
    return kind > OBJECT_KIND_INVALID && kind < OBJECT_KIND_COUNT && ((OBJECT_KINDS >> kind) & 1);
}

[[nodiscard]] constexpr auto valid_access(uint64_t access) noexcept -> bool {
    return (access & ~(uint64_t{VM_READ}
                       | uint64_t{VM_WRITE}
                       | uint64_t{VM_EXECUTE})) == 0
        && access != 0
        && ((access & VM_WRITE) == 0
            || (access & VM_READ) != 0);
}

[[nodiscard]] constexpr auto valid_rights(uint64_t rights) noexcept -> bool {
    return (rights & ~uint64_t{RIGHT_MASK}) == 0;
}

[[nodiscard]] constexpr auto valid_range(
    uint64_t base,
    uint64_t count) noexcept -> bool {
    return count != 0 && base <= UINT64_MAX - count;
}

[[nodiscard]] constexpr auto valid_descriptor(
    const CapView& value,
    DescriptorForm form = DescriptorForm::Ceiling) noexcept -> bool {
    if (value.version != CAP_ATTENUATION_VERSION_CURRENT
        || value.size != CAP_ATTENUATION_SIZE
        || !valid_kind(value.kind) || !valid_rights(value.rights)) {
        return false;
    }
    if (form == DescriptorForm::DuplicateRequest) {
        return zero_words(value, 0);
    }

    switch (value.kind) {
    case OBJECT_KIND_THREAD:
    case OBJECT_KIND_SCHED_CONTEXT:
    case OBJECT_KIND_SCHED_DOMAIN:
    case OBJECT_KIND_CSPACE:
    case OBJECT_KIND_NOTIFICATION:
    case OBJECT_KIND_IO_SPACE:
    case OBJECT_KIND_DEVICE:
    case OBJECT_KIND_IRQ:
    case OBJECT_KIND_PAGER:
        return zero_words(value, 0);
    case OBJECT_KIND_MEMORY:
        return valid_range(value.words[0], value.words[1])
            && valid_access(value.words[2])
            && zero_words(value, 3);
    case OBJECT_KIND_VSPACE:
        return value.words[0] != 0 && value.words[1] != 0
            && (value.words[0] % DEPLOY_PAGE_SIZE) == 0
            && (value.words[1] % DEPLOY_PAGE_SIZE) == 0
            && valid_range(value.words[0], value.words[1])
            && valid_access(value.words[2])
            && zero_words(value, 3);
    case OBJECT_KIND_RESOURCE_POOL: {
        constexpr uint64_t valid_mask =
            OBJECT_KINDS;
        return (value.words[2] & ~valid_mask) == 0
            && (value.words[2] & (uint64_t{1}
                                  << OBJECT_KIND_INVALID)) == 0
            && zero_words(value, 3);
    }
    case OBJECT_KIND_ENDPOINT:
        return (value.words[0] & ~value.words[1]) == 0
            && value.words[2] <= ENDPOINT_MAX_CAPS
            && zero_words(value, 3);
    case OBJECT_KIND_CHANNEL: {
        const bool unbound = value.words[1] == 0 && value.words[2] == 0;
        const bool exact = value.words[1] != 0
            && value.words[2] == UINT64_MAX;
        return value.words[0] <= CAP_CHANNEL_SIDE_B
            && (unbound || exact) && zero_words(value, 3);
    }
    case OBJECT_KIND_INVALID:
    case OBJECT_KIND_COUNT:
        return false;
    }
    return false;
}

/* Serialize one decoded descriptor through the canonical UAPI offsets.  The
 * deployment parser and the kernel both consume this fixed little-endian
 * representation; callers must not copy the native C++ object layout into a
 * descriptor MemoryObject. */
[[nodiscard]] constexpr auto rights_within(
    const CapView& requested,
    const CapView& ceiling) noexcept -> bool {
    return requested.kind == ceiling.kind
        && (requested.rights & ~ceiling.rights) == 0;
}

[[nodiscard]] constexpr auto range_within(
    uint64_t outer_base,
    uint64_t outer_count,
    uint64_t inner_base,
    uint64_t inner_count) noexcept -> bool {
    return valid_range(outer_base, outer_count)
        && valid_range(inner_base, inner_count)
        && inner_base >= outer_base
        && inner_count <= outer_base + outer_count - inner_base;
}

[[nodiscard]] constexpr auto data_within(
    const CapView& requested,
    const CapView& ceiling) noexcept -> bool {
    switch (requested.kind) {
    case OBJECT_KIND_THREAD:
    case OBJECT_KIND_SCHED_CONTEXT:
    case OBJECT_KIND_SCHED_DOMAIN:
    case OBJECT_KIND_CSPACE:
    case OBJECT_KIND_NOTIFICATION:
    case OBJECT_KIND_IO_SPACE:
    case OBJECT_KIND_DEVICE:
    case OBJECT_KIND_IRQ:
    case OBJECT_KIND_PAGER:
        return true;
    case OBJECT_KIND_MEMORY:
    case OBJECT_KIND_VSPACE:
        return range_within(
                   ceiling.words[0], ceiling.words[1], requested.words[0],
                   requested.words[1])
            && (requested.words[2] & ~ceiling.words[2]) == 0;
    case OBJECT_KIND_RESOURCE_POOL:
        return requested.words[0] <= ceiling.words[0]
            && requested.words[1] <= ceiling.words[1]
            && (requested.words[2] & ~ceiling.words[2]) == 0;
    case OBJECT_KIND_ENDPOINT:
        return (requested.words[1] & ceiling.words[1]) == ceiling.words[1]
            && (requested.words[0] & ceiling.words[1]) == ceiling.words[0]
            && requested.words[2] <= ceiling.words[2];
    case OBJECT_KIND_CHANNEL: {
        const bool ceiling_unbound =
            ceiling.words[1] == 0 && ceiling.words[2] == 0;
        const bool ceiling_exact =
            ceiling.words[1] != 0 && ceiling.words[2] == UINT64_MAX;
        const bool requested_unbound =
            requested.words[1] == 0 && requested.words[2] == 0;
        const bool requested_exact =
            requested.words[1] != 0 && requested.words[2] == UINT64_MAX;
        const bool same_side = requested.words[0] == ceiling.words[0];
        if (!same_side) {
            return false;
        }
        if (ceiling_unbound) {
            return requested_unbound;
        }
        return ceiling_exact && requested_exact
            && requested.words[1] == ceiling.words[1]
            && requested.words[2] == ceiling.words[2];
    }
    case OBJECT_KIND_INVALID:
    case OBJECT_KIND_COUNT:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr auto channel_mint_within(
    const CapView& requested,
    const CapView& ceiling) noexcept -> bool {
    return requested.kind == OBJECT_KIND_CHANNEL
        && ceiling.kind == OBJECT_KIND_CHANNEL
        && ceiling.words[0] == requested.words[0]
        && ceiling.words[1] == 0 && ceiling.words[2] == 0
        && requested.words[1] != 0 && requested.words[2] == UINT64_MAX;
}

/*
 * Compare one requested operation with a registered public ceiling.  The
 * mode distinction mirrors the three accepted import syscalls: Duplicate is
 * rights-only; TypedDelegate uses ordinary typed containment; ChannelMint is
 * the explicit unbound-to-exact same-side proof.
 */
[[nodiscard]] constexpr auto within(
    const CapView& requested,
    const CapView& ceiling,
    uint16_t mode) noexcept -> bool {
    if (!valid_descriptor(ceiling, DescriptorForm::Ceiling)
        || !rights_within(requested, ceiling)) {
        return false;
    }
    if (mode == DEPLOY_IMPORT_DUPLICATE) {
        return valid_descriptor(requested, DescriptorForm::DuplicateRequest);
    }
    if (mode == DEPLOY_IMPORT_CHANNEL_MINT) {
        return valid_descriptor(requested, DescriptorForm::TypedRequest)
            && channel_mint_within(requested, ceiling);
    }
    if (mode != DEPLOY_IMPORT_TYPED_DELEGATE
        || !valid_descriptor(requested, DescriptorForm::TypedRequest)) {
        return false;
    }
    return data_within(requested, ceiling);
}

} // namespace deploy::attenuation

namespace deploy {

static_assert(sizeof(IpcBinding) <= DEPLOY_PAGE_SIZE);

class ByteView final {
public:
    constexpr ByteView() noexcept = default;
    constexpr ByteView(const uint8_t* data, size_t size) noexcept
        : data_(data), size_(size) {}

    [[nodiscard]] constexpr auto data() const noexcept -> const uint8_t* {
        return data_;
    }
    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return size_;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return data_ != nullptr;
    }
    [[nodiscard]] constexpr auto operator[](size_t index) const noexcept
        -> uint8_t {
        return data_[index];
    }

    [[nodiscard]] constexpr auto equals(ByteView other) const noexcept -> bool {
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

private:
    const uint8_t* data_{};
    size_t size_{};
};

struct StringRef final {
    uint32_t offset{};
    uint32_t length{};
};

static_assert(sizeof(StringRef) == 8);

/*
 * Canonical typed projections of validated wire rows.  These are borrowed
 * views of ManifestView's bytes; DeploymentPlan copies them into its own
 * explicit storage.  Field offsets and widths are owned by ManifestView,
 * not by the decoded-plan layer.
 */
struct ManifestTaskRow final {
    StringRef name{};
    StringRef pool_key{};
    StringRef vspace_key{};
    StringRef cspace_key{};
    uint32_t image_first{};
    uint32_t image_count{};
    uint32_t mapping_first{};
    uint32_t mapping_count{};
    uint32_t object_first{};
    uint32_t object_count{};
    uint32_t execution_first{};
    uint32_t execution_count{};
    uint32_t import_first{};
    uint32_t import_count{};
    uint32_t dependency_first{};
    uint32_t dependency_count{};
    uint32_t export_first{};
    uint32_t export_count{};
    uint64_t pool_memory{};
    uint64_t pool_caps{};
    uint64_t kind_mask{};
    uint64_t critical_bytes{};
    uint32_t cspace_slots{};
    uint32_t cspace_pages{};
    uint32_t bootstrap_mapping{DEPLOY_NO_INDEX};
    uint32_t flags{};
    uint16_t readiness{};
    uint16_t terminal{};
    uint16_t restart{};
    uint64_t readiness_timeout_ns{};
    uint32_t bootstrap_first{};
    uint32_t bootstrap_count{};
    StringRef arguments{};
};

struct ManifestImageRow final {
    StringRef source{};
    uint16_t source_kind{};
    uint16_t flags{};
};

struct ManifestMappingRow final {
    StringRef produced{};
    StringRef pager{};
    StringRef region{};
    uint32_t image{DEPLOY_NO_INDEX};
    uint32_t segment{DEPLOY_NO_INDEX};
    uint16_t source{};
    uint16_t residency{};
    uint16_t critical{};
    uint16_t flags{};
    uint32_t access{};
    uint32_t pager_policy{};
    uint64_t address{};
    uint64_t size{};
};

struct ManifestObjectRow final {
    StringRef output{};
    StringRef output_b{};
    uint16_t kind{};
    uint16_t flags{};
    uint32_t refs[4]{};
    uint64_t args[6]{};
};

struct ManifestExecutionRow final {
    StringRef key{};
    StringRef sc{};
    StringRef domain{};
    uint32_t image{DEPLOY_NO_INDEX};
    uint32_t stack{DEPLOY_NO_INDEX};
    uint32_t bootstrap{DEPLOY_NO_INDEX};
    uint32_t ipc{DEPLOY_NO_INDEX};
    uint32_t control{DEPLOY_NO_INDEX};
    uint32_t event{DEPLOY_NO_INDEX};
    uint16_t model{};
    uint16_t flags{};
    uint16_t fault{};
    uint16_t terminal{};
    uint64_t entry{};
    uint64_t stack_top{};
    uint64_t sc_budget{};
    uint64_t sc_period{};
    uint32_t urgency{};
    uint32_t home_cpu{};
};

struct ManifestImportRow final {
    StringRef source{};
    StringRef destination{};
    uint16_t mode{};
    uint16_t selector{};
    uint32_t flags{};
    CapView attenuation{};
    uint16_t source_class{};
};

struct ManifestBootstrapRow final {
    uint32_t kind{};
    StringRef destination{};
    StringRef name{};
    uint32_t protocol{};
    uint16_t major{};
    uint16_t minor{};
    uint16_t object_kind{};
};

struct ManifestDependencyRow final {
    uint32_t target{DEPLOY_NO_INDEX};
    uint16_t kind{};
    uint16_t flags{};
    StringRef relation{};
};

struct ManifestExportRow final {
    StringRef source{};
    StringRef key{};
    uint16_t source_class{};
    uint16_t flags{};
    CapView ceiling{};
};

struct EffectiveMapping final {
    uint64_t address{};
    uint64_t size{};
    uint64_t access{};
    uint64_t critical{};
    uint64_t source{};
};

struct ManifestWorkspace final {
    ByteView keys[512]{};
    bool required_edges[DEPLOY_TASK_MAX][DEPLOY_TASK_MAX]{};
    EffectiveMapping mappings[DEPLOY_TASK_MAPPING_MAX]{};

    void reset() noexcept {
        for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]);
             ++index) {
            keys[index] = {};
        }
        for (size_t owner = 0; owner < DEPLOY_TASK_MAX; ++owner) {
            for (size_t target = 0; target < DEPLOY_TASK_MAX; ++target) {
                required_edges[owner][target] = false;
            }
        }
        for (size_t index = 0;
             index < sizeof(mappings) / sizeof(mappings[0]); ++index) {
            mappings[index] = {};
        }
    }
};

static_assert(sizeof(ManifestWorkspace)
    == sizeof(ByteView) * 512
        + sizeof(bool) * DEPLOY_TASK_MAX * DEPLOY_TASK_MAX
        + sizeof(EffectiveMapping) * DEPLOY_TASK_MAPPING_MAX);

enum class Error : uint16_t {
    Truncated,
    BadMagic,
    BadVersion,
    WrongTarget,
    UnsupportedFeatures,
    InvalidHeader,
    InvalidTable,
    InvalidString,
    InvalidEnum,
    InvalidReserved,
    InvalidReference,
    DuplicateKey,
    InvalidRange,
    InvalidRecord,
    DependencyCycle,
    CriticalBudget,
    InvalidBootBundle,
};

class ManifestView final {
public:
    struct Table final {
        size_t offset{};
        uint32_t count{};
        uint32_t stride{};
    };

    [[nodiscard]] static auto parse(
        const void* data,
        size_t size,
        ManifestWorkspace& workspace) noexcept
        -> std::expected<ManifestView, Error> {
        workspace.reset();
        ManifestView result{data, size};
        if (result.validate(workspace)) {
            return (result);
        }
        return std::unexpected(result.error_);
    }

    [[nodiscard]] constexpr auto bytes() const noexcept -> ByteView {
        return bytes_;
    }

    [[nodiscard]] auto validate_boot_bundle(
        const boot::Bundle& bundle,
        ManifestWorkspace& workspace) noexcept -> bool {
        workspace.reset();
        if (!bundle) {
            return fail(Error::InvalidBootBundle);
        }
        return validate_boot_bundle_records(bundle, workspace);
    }

    [[nodiscard]] constexpr auto table(uint32_t index) const noexcept
        -> Table {
        return tables_[index];
    }
    [[nodiscard]] constexpr auto task_count() const noexcept -> uint32_t {
        return tables_[DEPLOY_TABLE_TASK].count;
    }
    [[nodiscard]] constexpr auto image_count() const noexcept -> uint32_t {
        return tables_[DEPLOY_TABLE_IMAGE].count;
    }
    [[nodiscard]] constexpr auto mapping_count() const noexcept -> uint32_t {
        return tables_[DEPLOY_TABLE_MAPPING].count;
    }
    [[nodiscard]] constexpr auto object_count() const noexcept -> uint32_t {
        return tables_[DEPLOY_TABLE_OBJECT].count;
    }
    [[nodiscard]] constexpr auto execution_count() const noexcept -> uint32_t {
        return tables_[DEPLOY_TABLE_EXECUTION].count;
    }
    [[nodiscard]] constexpr auto import_count() const noexcept -> uint32_t {
        return tables_[DEPLOY_TABLE_IMPORT].count;
    }
    [[nodiscard]] constexpr auto dependency_count() const noexcept -> uint32_t {
        return tables_[DEPLOY_TABLE_DEPENDENCY].count;
    }
    [[nodiscard]] constexpr auto export_count() const noexcept -> uint32_t {
        return tables_[DEPLOY_TABLE_EXPORT].count;
    }
    [[nodiscard]] constexpr auto bootstrap_count() const noexcept -> uint32_t {
        return tables_[DEPLOY_TABLE_BOOTSTRAP].count;
    }

    [[nodiscard]] auto string(StringRef ref) const noexcept -> ByteView {
        const Table strings = tables_[DEPLOY_TABLE_STRING];
        if (ref.offset > strings.count || ref.length > strings.count - ref.offset) {
            return {};
        }
        const auto base = strings.offset + static_cast<size_t>(ref.offset);
        return ByteView{bytes_.data() + base, ref.length};
    }

    [[nodiscard]] auto string_table() const noexcept -> ByteView {
        const Table strings = tables_[DEPLOY_TABLE_STRING];
        if (strings.offset > bytes_.size()
            || strings.count > bytes_.size() - strings.offset) {
            return {};
        }
        return ByteView{bytes_.data() + strings.offset, strings.count};
    }

    [[nodiscard]] auto task_name(uint32_t index) const noexcept -> ByteView {
        uint64_t value{};
        return read(DEPLOY_TABLE_TASK, index, DEPLOY_TASK_NAME, 8, value)
            ? string(StringRef{static_cast<uint32_t>(value),
                              static_cast<uint32_t>(value >> 32)})
            : ByteView{};
    }

    [[nodiscard]] auto read(
        uint32_t table_index,
        uint32_t index,
        size_t field,
        size_t width,
        uint64_t& value) const noexcept -> bool {
        if (table_index >= DEPLOY_TABLE_COUNT
            || index >= tables_[table_index].count
            || width > sizeof(uint64_t)
            || field > tables_[table_index].stride
            || width > tables_[table_index].stride - field) {
            return false;
        }
        const auto table = tables_[table_index];
        const auto row = static_cast<size_t>(index) * table.stride;
        const auto offset = table.offset + row + field;
        if (offset > bytes_.size() || width > bytes_.size() - offset) {
            return false;
        }
        value = 0;
        for (size_t byte = 0; byte < width; ++byte) {
            value |= static_cast<uint64_t>(bytes_[offset + byte]) << (byte * 8);
        }
        return true;
    }

    [[nodiscard]] auto task_row(
        uint32_t index,
        ManifestTaskRow& output) const noexcept -> bool;
    [[nodiscard]] auto image_row(
        uint32_t index,
        ManifestImageRow& output) const noexcept -> bool;
    [[nodiscard]] auto mapping_row(
        uint32_t index,
        ManifestMappingRow& output) const noexcept -> bool;
    [[nodiscard]] auto object_row(
        uint32_t index,
        ManifestObjectRow& output) const noexcept -> bool;
    [[nodiscard]] auto execution_row(
        uint32_t index,
        ManifestExecutionRow& output) const noexcept -> bool;
    [[nodiscard]] auto import_row(
        uint32_t index,
        ManifestImportRow& output) const noexcept -> bool;
    [[nodiscard]] auto dependency_row(
        uint32_t index,
        ManifestDependencyRow& output) const noexcept -> bool;
    [[nodiscard]] auto export_row(
        uint32_t index,
        ManifestExportRow& output) const noexcept -> bool;
    [[nodiscard]] auto bootstrap_row(
        uint32_t index,
        ManifestBootstrapRow& output) const noexcept -> bool;

private:
    constexpr ManifestView(const void* data, size_t size) noexcept
        : bytes_{static_cast<const uint8_t*>(data), size} {}

    [[nodiscard]] auto fail(Error error) noexcept -> bool {
        error_ = error;
        return false;
    }

    [[nodiscard]] auto read_header() noexcept -> bool {
        uint64_t magic{};
        uint64_t major{};
        uint64_t minor{};
        uint64_t header_size{};
        uint64_t total_size{};
        uint64_t architecture{};
        uint64_t abi{};
        uint64_t features{};
        uint64_t flags{};
        uint64_t checksum{};
        uint64_t table_count{};
        uint64_t reserved{};
        if (!read_raw(DEPLOY_HEADER_MAGIC, 8, magic)
            || !read_raw(DEPLOY_HEADER_MAJOR, 2, major)
            || !read_raw(DEPLOY_HEADER_MINOR, 2, minor)
            || !read_raw(DEPLOY_HEADER_SIZE_FIELD, 4, header_size)
            || !read_raw(DEPLOY_HEADER_TOTAL_SIZE, 8, total_size)
            || !read_raw(DEPLOY_HEADER_ARCHITECTURE, 4, architecture)
            || !read_raw(DEPLOY_HEADER_ABI, 4, abi)
            || !read_raw(DEPLOY_HEADER_FEATURES, 8, features)
            || !read_raw(DEPLOY_HEADER_FLAGS, 8, flags)
            || !read_raw(DEPLOY_HEADER_CHECKSUM, 8, checksum)
            || !read_raw(DEPLOY_HEADER_TABLE_COUNT, 4, table_count)
            || !read_raw(DEPLOY_HEADER_RESERVED, 4, reserved)) {
            return fail(Error::Truncated);
        }
        if (magic != DEPLOY_MAGIC) {
            return fail(Error::BadMagic);
        }
        const bool current = header_size == DEPLOY_HEADER_SIZE
            && table_count == DEPLOY_TABLE_COUNT;
        /* The minor version binds the complete envelope shape.  This is an
         * internal wire migration; accepting an older shape would create a
         * hybrid interpretation of mapping rows and bootstrap roles. */
        if (major != DEPLOY_MAJOR || minor != DEPLOY_MINOR
            || !current) {
            return fail(Error::BadVersion);
        }
        header_size_ = static_cast<uint32_t>(header_size);
        table_count_ = static_cast<uint32_t>(table_count);
        if (total_size != bytes_.size()
            || architecture != DEPLOY_ARCH_GENERIC
            || abi != DEPLOY_ABI_ID) {
            return fail(Error::WrongTarget);
        }
        if (features != 0 || flags != 0 || checksum != 0) {
            return fail(Error::UnsupportedFeatures);
        }
        if (table_count != DEPLOY_TABLE_COUNT || reserved != 0) {
            return fail(Error::InvalidHeader);
        }
        return true;
    }

    [[nodiscard]] auto read_raw(
        size_t offset,
        size_t width,
        uint64_t& value) const noexcept -> bool {
        if (width > sizeof(uint64_t) || offset > bytes_.size()
            || width > bytes_.size() - offset) {
            return false;
        }
        value = 0;
        for (size_t byte = 0; byte < width; ++byte) {
            value |= static_cast<uint64_t>(bytes_[offset + byte]) << (byte * 8);
        }
        return true;
    }

    [[nodiscard]] auto read_table_descriptors() noexcept -> bool {
        constexpr uint32_t limits[DEPLOY_TABLE_COUNT] = {
            DEPLOY_TASK_MAX, DEPLOY_IMAGE_MAX,
            DEPLOY_MAPPING_MAX, DEPLOY_OBJECT_MAX,
            DEPLOY_EXECUTION_MAX, DEPLOY_IMPORT_MAX,
            DEPLOY_DEPENDENCY_MAX, DEPLOY_EXPORT_MAX,
            DEPLOY_STRING_MAX, DEPLOY_BOOTSTRAP_MAX,
        };
        constexpr uint32_t strides[DEPLOY_TABLE_COUNT] = {
            DEPLOY_TASK_STRIDE, DEPLOY_IMAGE_STRIDE,
            DEPLOY_MAPPING_STRIDE, DEPLOY_OBJECT_STRIDE,
            DEPLOY_EXECUTION_STRIDE, DEPLOY_IMPORT_STRIDE,
            DEPLOY_DEPENDENCY_STRIDE, DEPLOY_EXPORT_STRIDE, 1,
            DEPLOY_BOOTSTRAP_STRIDE,
        };
        for (uint32_t index = 0; index < table_count_; ++index) {
            const size_t descriptor = DEPLOY_HEADER_TABLES
                + static_cast<size_t>(index) * DEPLOY_TABLE_DESC_SIZE;
            uint64_t offset{};
            uint64_t count{};
            uint64_t stride{};
            if (!read_raw(descriptor + DEPLOY_TABLE_OFFSET, 8, offset)
                || !read_raw(
                    descriptor + DEPLOY_TABLE_COUNT_FIELD, 4, count)
                || !read_raw(descriptor + DEPLOY_TABLE_STRIDE, 4, stride)) {
                return fail(Error::Truncated);
            }
            if (count > limits[index] || stride != strides[index]) {
                return fail(Error::InvalidTable);
            }
            if (count == 0) {
                if (offset != 0) {
                    return fail(Error::InvalidTable);
                }
                tables_[index] = Table{0, 0, static_cast<uint32_t>(stride)};
                continue;
            }
            if (offset < header_size_
                || offset % DEPLOY_TABLE_ALIGNMENT != 0
                || offset > bytes_.size()) {
                return fail(Error::InvalidTable);
            }
            const auto span = libk::checked_multiply<size_t>(
                static_cast<size_t>(count), static_cast<size_t>(stride));
            if (!span || *span > bytes_.size() - static_cast<size_t>(offset)) {
                return fail(Error::InvalidTable);
            }
            if (index == DEPLOY_TABLE_STRING && count > DEPLOY_STRING_MAX) {
                return fail(Error::InvalidTable);
            }
            tables_[index] = Table{
                static_cast<size_t>(offset),
                static_cast<uint32_t>(count),
                static_cast<uint32_t>(stride),
            };
        }
        for (uint32_t index = table_count_; index < DEPLOY_TABLE_COUNT;
             ++index) {
            tables_[index] = Table{
                0,
                0,
                index == DEPLOY_TABLE_BOOTSTRAP
                    ? DEPLOY_BOOTSTRAP_STRIDE : 1,
            };
        }
        for (uint32_t first = 0; first < DEPLOY_TABLE_COUNT; ++first) {
            if (tables_[first].count == 0) {
                continue;
            }
            const size_t first_end = tables_[first].offset
                + static_cast<size_t>(tables_[first].count)
                    * tables_[first].stride;
            for (uint32_t second = first + 1;
                 second < DEPLOY_TABLE_COUNT;
                 ++second) {
                if (tables_[second].count == 0) {
                    continue;
                }
                const size_t second_end = tables_[second].offset
                    + static_cast<size_t>(tables_[second].count)
                        * tables_[second].stride;
                if (tables_[first].offset < second_end
                    && tables_[second].offset < first_end) {
                    return fail(Error::InvalidTable);
                }
            }
        }
        return true;
    }

    [[nodiscard]] auto read_string_ref(
        uint32_t table_index,
        uint32_t index,
        size_t field,
        ByteView& output) const noexcept -> bool {
        return read_key(table_index, index, field, output, true);
    }

    [[nodiscard]] auto read_key(
        uint32_t table_index,
        uint32_t index,
        size_t field,
        ByteView& output,
        bool required) const noexcept -> bool {
        uint64_t value{};
        if (!read(table_index, index, field, 8, value)) {
            return false;
        }
        const StringRef ref{
            static_cast<uint32_t>(value),
            static_cast<uint32_t>(value >> 32),
        };
        if (!required && ref.length == 0) {
            if (ref.offset != 0) {
                return false;
            }
            output = {};
            return true;
        }
        output = string(ref);
        if ((ref.length == 0 && required) || !output) {
            return false;
        }
        for (size_t byte = 0; byte < output.size(); ++byte) {
            if (output[byte] == 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto zero(
        uint32_t table_index,
        uint32_t index,
        size_t begin,
        size_t end) const noexcept -> bool {
        for (size_t field = begin; field < end; ++field) {
            uint64_t value{};
            if (!read(table_index, index, field, 1, value) || value != 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto value(
        uint32_t table_index,
        uint32_t index,
        size_t field,
        size_t width,
        uint64_t& result) const noexcept -> bool {
        return read(table_index, index, field, width, result);
    }

    [[nodiscard]] static constexpr auto valid_kind(uint64_t kind) noexcept
        -> bool {
        return kind > OBJECT_KIND_INVALID
            && kind < OBJECT_KIND_COUNT && ((OBJECT_KINDS >> kind) & 1);
    }

    [[nodiscard]] static constexpr auto valid_access(uint64_t access) noexcept
        -> bool {
        return (access & ~(VM_READ | VM_WRITE | VM_EXECUTE)) == 0
            && access != 0
            && ((access & VM_WRITE) == 0
                || (access & VM_READ) != 0)
            && (access & (VM_WRITE | VM_EXECUTE))
                != (VM_WRITE | VM_EXECUTE);
    }

    [[nodiscard]] auto validate_attenuation(
        uint32_t table_index,
        uint32_t index,
        size_t base,
        bool duplicate) const noexcept -> bool {
        uint64_t version{};
        uint64_t kind{};
        uint64_t size{};
        uint64_t rights{};
        if (!value(table_index, index, base + DEPLOY_ATTENUATION_VERSION,
                  2, version)
            || !value(table_index, index, base + DEPLOY_ATTENUATION_KIND,
                      2, kind)
            || !value(table_index, index, base + DEPLOY_ATTENUATION_SIZE,
                     4, size)
            || !value(table_index, index, base + DEPLOY_ATTENUATION_RIGHTS,
                     8, rights)) {
            return false;
        }
        if (version != DEPLOY_ATTENUATION_VERSION_CURRENT
            || size != DEPLOY_ATTENUATION_STRIDE
            || !valid_kind(kind)) {
            return false;
        }
        uint64_t words[6]{};
        for (uint32_t word = 0; word < 6; ++word) {
            uint64_t value_word{};
            if (!value(table_index, index,
                       base + DEPLOY_ATTENUATION_WORD0 + word * 8,
                       8, value_word)) {
                return false;
            }
            words[word] = value_word;
        }
        CapView descriptor{};
        descriptor.version = static_cast<uint16_t>(version);
        descriptor.kind = static_cast<uint16_t>(kind);
        descriptor.size = static_cast<uint32_t>(size);
        descriptor.rights = rights;
        for (uint32_t word = 0; word < 6; ++word) {
            descriptor.words[word] = words[word];
        }
        return attenuation::valid_descriptor(
            descriptor,
            duplicate ? attenuation::DescriptorForm::DuplicateRequest
                      : attenuation::DescriptorForm::TypedRequest);
    }

    [[nodiscard]] auto validate_tasks() noexcept -> bool {
        for (uint32_t task = 0; task < task_count(); ++task) {
            ByteView name{};
            ByteView pool{};
            ByteView vspace{};
            ByteView cspace{};
            if (!read_key(
                    DEPLOY_TABLE_TASK, task, DEPLOY_TASK_NAME, name,
                    true)
                || !read_key(DEPLOY_TABLE_TASK, task,
                             DEPLOY_TASK_POOL, pool, true)
                || !read_key(DEPLOY_TABLE_TASK, task,
                             DEPLOY_TASK_VSPACE, vspace, true)
                || !read_key(DEPLOY_TABLE_TASK, task,
                             DEPLOY_TASK_CSPACE, cspace, true)
                || !zero(DEPLOY_TABLE_TASK, task,
                         DEPLOY_TASK_RESERVED,
                         DEPLOY_TASK_RESERVED + 2)) {
                return fail(Error::InvalidRecord);
            }
            uint64_t argument_ref{};
            if (!value(DEPLOY_TABLE_TASK, task, DEPLOY_TASK_ARGUMENTS,
                    8, argument_ref)) return fail(Error::InvalidRecord);
            const StringRef arguments{static_cast<uint32_t>(argument_ref),
                static_cast<uint32_t>(argument_ref >> 32)};
            if (arguments.length == 0) {
                if (arguments.offset != 0) return fail(Error::InvalidRecord);
            } else {
                const auto bytes = string(arguments);
                boot::Args decoded;
                if (!bytes || !decoded.decode(
                    reinterpret_cast<const char*>(bytes.data()), bytes.size()))
                    return fail(Error::InvalidRecord);
            }
            uint64_t readiness{};
            uint64_t readiness_timeout_ns{};
            uint64_t terminal{};
            uint64_t restart{};
            uint64_t flags{};
            uint64_t bootstrap{};
            uint64_t kind_mask{};
            uint64_t pool_memory{};
            uint64_t pool_caps{};
            uint64_t critical_bytes{};
            uint64_t cspace_slots{};
            uint64_t cspace_pages{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_READINESS, 2, readiness)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_READINESS_TIMEOUT_NS, 8,
                          readiness_timeout_ns)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_TERMINAL, 2, terminal)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_RESTART, 2, restart)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_FLAGS, 4, flags)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_BOOTSTRAP_MAPPING, 4, bootstrap)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_KIND_MASK, 8, kind_mask)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_POOL_MEMORY, 8, pool_memory)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_POOL_CAPS, 8, pool_caps)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_CRITICAL_BYTES, 8,
                          critical_bytes)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_CSPACE_SLOTS, 4, cspace_slots)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_CSPACE_PAGES, 4, cspace_pages)
                || readiness > DEPLOY_READINESS_EXPLICIT
                || ((readiness == DEPLOY_READINESS_EXPLICIT)
                    != (readiness_timeout_ns != 0))
                || terminal > DEPLOY_TERMINAL_CLOSE
                || restart > DEPLOY_RESTART_ALWAYS
                || flags != 0
                || (kind_mask
                    & ~OBJECT_KINDS) != 0
                || (kind_mask
                    & (UINT64_C(1) << OBJECT_KIND_INVALID)) != 0
                || pool_memory == 0
                || (pool_memory % DEPLOY_PAGE_SIZE) != 0
                || pool_caps == 0
                || (critical_bytes % DEPLOY_PAGE_SIZE) != 0
                || critical_bytes == 0
                || critical_bytes > pool_memory || cspace_slots == 0
                || cspace_pages == 0) {
                return fail(Error::InvalidEnum);
            }
            const uint32_t firsts[7] = {
                DEPLOY_TASK_IMAGE_FIRST,
                DEPLOY_TASK_MAPPING_FIRST,
                DEPLOY_TASK_OBJECT_FIRST,
                DEPLOY_TASK_EXECUTION_FIRST,
                DEPLOY_TASK_IMPORT_FIRST,
                DEPLOY_TASK_DEPENDENCY_FIRST,
                DEPLOY_TASK_EXPORT_FIRST,
            };
            const uint32_t counts[7] = {
                DEPLOY_TASK_IMAGE_COUNT,
                DEPLOY_TASK_MAPPING_COUNT,
                DEPLOY_TASK_OBJECT_COUNT,
                DEPLOY_TASK_EXECUTION_COUNT,
                DEPLOY_TASK_IMPORT_COUNT,
                DEPLOY_TASK_DEPENDENCY_COUNT,
                DEPLOY_TASK_EXPORT_COUNT,
            };
            const uint32_t limits[7] = {
                DEPLOY_TASK_IMAGE_MAX,
                DEPLOY_TASK_MAPPING_MAX,
                DEPLOY_TASK_OBJECT_MAX,
                DEPLOY_TASK_EXECUTION_MAX,
                DEPLOY_TASK_IMPORT_MAX,
                DEPLOY_TASK_DEPENDENCY_MAX,
                DEPLOY_TASK_EXPORT_MAX,
            };
            for (uint32_t child = 0; child < 7; ++child) {
                uint64_t first{};
                uint64_t count{};
                if (!value(DEPLOY_TABLE_TASK, task, firsts[child], 4, first)
                    || !value(DEPLOY_TABLE_TASK, task, counts[child], 4, count)
                    || count > limits[child]
                    || first > tables_[child + 1].count
                    || count > tables_[child + 1].count - first) {
                    return fail(Error::InvalidReference);
                }
                if (child == 1 && bootstrap != DEPLOY_NO_INDEX
                    && (bootstrap < first || bootstrap >= first + count)) {
                    return fail(Error::InvalidReference);
                }
            }
            uint64_t bootstrap_first{};
            uint64_t bootstrap_count_value{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_BOOTSTRAP_FIRST, 4,
                       bootstrap_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_BOOTSTRAP_COUNT, 4,
                          bootstrap_count_value)
                || bootstrap_count_value > DEPLOY_TASK_BOOTSTRAP_MAX
                || bootstrap_first > tables_[DEPLOY_TABLE_BOOTSTRAP].count
                || bootstrap_count_value
                    > tables_[DEPLOY_TABLE_BOOTSTRAP].count
                        - bootstrap_first) {
                return fail(Error::InvalidReference);
            }
        }
        return true;
    }

    [[nodiscard]] auto validate_ranges() noexcept -> bool {
        const uint32_t task_tables[7] = {
            DEPLOY_TABLE_IMAGE, DEPLOY_TABLE_MAPPING,
            DEPLOY_TABLE_OBJECT, DEPLOY_TABLE_EXECUTION,
            DEPLOY_TABLE_IMPORT, DEPLOY_TABLE_DEPENDENCY,
            DEPLOY_TABLE_EXPORT,
        };
        const uint32_t first_fields[7] = {
            DEPLOY_TASK_IMAGE_FIRST, DEPLOY_TASK_MAPPING_FIRST,
            DEPLOY_TASK_OBJECT_FIRST, DEPLOY_TASK_EXECUTION_FIRST,
            DEPLOY_TASK_IMPORT_FIRST, DEPLOY_TASK_DEPENDENCY_FIRST,
            DEPLOY_TASK_EXPORT_FIRST,
        };
        const uint32_t count_fields[7] = {
            DEPLOY_TASK_IMAGE_COUNT, DEPLOY_TASK_MAPPING_COUNT,
            DEPLOY_TASK_OBJECT_COUNT, DEPLOY_TASK_EXECUTION_COUNT,
            DEPLOY_TASK_IMPORT_COUNT, DEPLOY_TASK_DEPENDENCY_COUNT,
            DEPLOY_TASK_EXPORT_COUNT,
        };
        for (uint32_t child = 0; child < 7; ++child) {
            uint64_t cursor{};
            for (uint32_t task = 0; task < task_count(); ++task) {
                uint64_t first{};
                uint64_t count{};
                if (!value(DEPLOY_TABLE_TASK, task, first_fields[child], 4, first)
                    || !value(DEPLOY_TABLE_TASK, task, count_fields[child], 4, count)
                    || first != cursor) {
                    return fail(Error::InvalidReference);
                }
                cursor += count;
            }
            if (cursor != tables_[task_tables[child]].count) {
                return fail(Error::InvalidReference);
            }
        }
        uint64_t bootstrap_cursor{};
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t first{};
            uint64_t count{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_BOOTSTRAP_FIRST, 4, first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_BOOTSTRAP_COUNT, 4, count)
                || first != bootstrap_cursor) {
                return fail(Error::InvalidReference);
            }
            bootstrap_cursor += count;
        }
        if (bootstrap_cursor != tables_[DEPLOY_TABLE_BOOTSTRAP].count) {
            return fail(Error::InvalidReference);
        }
        return true;
    }

    [[nodiscard]] auto validate_images() noexcept -> bool {
        for (uint32_t index = 0; index < image_count(); ++index) {
            ByteView source{};
            uint64_t kind{};
            uint64_t flags{};
            if (!read_string_ref(DEPLOY_TABLE_IMAGE, index,
                                 DEPLOY_IMAGE_SOURCE, source)
                || !value(DEPLOY_TABLE_IMAGE, index,
                          DEPLOY_IMAGE_SOURCE_KIND, 2, kind)
                || !value(DEPLOY_TABLE_IMAGE, index,
                          DEPLOY_IMAGE_FLAGS, 2, flags)
                || kind != DEPLOY_IMAGE_SOURCE_BOOT_BUNDLE
                || flags != 0
                || !zero(DEPLOY_TABLE_IMAGE, index,
                         DEPLOY_IMAGE_RESERVED, DEPLOY_IMAGE_STRIDE)) {
                return fail(Error::InvalidRecord);
            }
        }
        return true;
    }

    [[nodiscard]] auto validate_mappings() noexcept -> bool {
        for (uint32_t index = 0; index < mapping_count(); ++index) {
            ByteView produced{};
            ByteView pager_key{};
            ByteView region_key{};
            uint64_t source{};
            uint64_t residency{};
            uint64_t critical{};
            uint64_t flags{};
            uint64_t access{};
            uint64_t image{};
            uint64_t segment{};
            uint64_t address{};
            uint64_t size{};
            uint64_t pager_policy{};
            if (!read_key(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_PRODUCED, produced, true)
                || !read_key(DEPLOY_TABLE_MAPPING, index,
                             DEPLOY_MAPPING_PAGER, pager_key,
                             false)
                || !read_key(DEPLOY_TABLE_MAPPING, index,
                             DEPLOY_MAPPING_REGION, region_key,
                             false)
                || !value(DEPLOY_TABLE_MAPPING, index,
                       DEPLOY_MAPPING_SOURCE, 2, source)
                || !value(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_RESIDENCY, 2, residency)
                || !value(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_CRITICAL, 2, critical)
                || !value(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_FLAGS, 2, flags)
                || !value(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_ACCESS, 4, access)
                || !value(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_IMAGE, 4, image)
                || !value(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_SEGMENT, 4, segment)
                || !value(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_ADDRESS, 8, address)
                || !value(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_SIZE, 8, size)
                || !value(DEPLOY_TABLE_MAPPING, index,
                          DEPLOY_MAPPING_PAGER_POLICY, 4, pager_policy)
                || source > DEPLOY_MAPPING_SOURCE_PAGER
                || residency > DEPLOY_MAPPING_PAGEABLE
                || critical > DEPLOY_CRITICAL_IPC_HEADER
                || flags != 0 || pager_policy != 0
                || address > UINT64_MAX - size
                || !zero(DEPLOY_TABLE_MAPPING, index,
                         DEPLOY_MAPPING_RESERVED, DEPLOY_MAPPING_STRIDE)) {
                return fail(Error::InvalidRecord);
            }
            if (source == DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT) {
                if (image >= image_count() || segment == DEPLOY_NO_INDEX
                    || pager_key.size() != 0
                    || residency != DEPLOY_MAPPING_RESIDENT
                    || address != 0 || size != 0 || access != 0
                    || pager_policy != 0) {
                    return fail(Error::InvalidReference);
                }
            } else if (source == DEPLOY_MAPPING_SOURCE_ZERO) {
                if (image != DEPLOY_NO_INDEX
                    || segment != DEPLOY_NO_INDEX || pager_key.size() != 0
                    || residency != DEPLOY_MAPPING_RESIDENT
                    || size == 0 || (address % DEPLOY_PAGE_SIZE) != 0
                    || (size % DEPLOY_PAGE_SIZE) != 0
                    || !valid_access(access)) {
                    return fail(Error::InvalidReference);
                }
            } else if (source == DEPLOY_MAPPING_SOURCE_PAGER) {
                if (image != DEPLOY_NO_INDEX
                    || segment != DEPLOY_NO_INDEX || pager_key.size() == 0
                    || residency != DEPLOY_MAPPING_PAGEABLE
                    || size == 0 || (address % DEPLOY_PAGE_SIZE) != 0
                    || (size % DEPLOY_PAGE_SIZE) != 0
                    || !valid_access(access)) {
                    return fail(Error::InvalidReference);
                }
            }
            uint32_t owner{};
            if (!mapping_owner(index, owner)
                || (source == DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
                    && !in_task_range(
                        owner, DEPLOY_TABLE_IMAGE, image))) {
                return fail(Error::InvalidReference);
            }
            if (critical != DEPLOY_CRITICAL_NONE
                && (residency != DEPLOY_MAPPING_RESIDENT
                    || source == DEPLOY_MAPPING_SOURCE_PAGER)) {
                return fail(Error::InvalidRecord);
            }
            if (source != DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
                && size == 0) {
                return fail(Error::InvalidRange);
            }
        }
        return true;
    }

    [[nodiscard]] auto validate_objects() noexcept -> bool {
        for (uint32_t index = 0; index < object_count(); ++index) {
            ByteView output_key{};
            ByteView output_b_key{};
            uint64_t kind{};
            uint64_t flags{};
            if (!read_key(DEPLOY_TABLE_OBJECT, index,
                          DEPLOY_OBJECT_OUTPUT_A, output_key, true)
                || !read_key(DEPLOY_TABLE_OBJECT, index,
                             DEPLOY_OBJECT_OUTPUT_B, output_b_key,
                             false)
                || !value(DEPLOY_TABLE_OBJECT, index,
                          DEPLOY_OBJECT_KIND, 2, kind)
                || !value(DEPLOY_TABLE_OBJECT, index,
                          DEPLOY_OBJECT_FLAGS, 2, flags)
                || !valid_kind(kind)
                || !zero(DEPLOY_TABLE_OBJECT, index,
                         DEPLOY_OBJECT_RESERVED,
                         DEPLOY_OBJECT_RESERVED + 4)
                || !zero(DEPLOY_TABLE_OBJECT, index,
                         DEPLOY_OBJECT_RESERVED_TAIL,
                         DEPLOY_OBJECT_STRIDE)) {
                return fail(Error::InvalidRecord);
            }
            uint32_t owner{};
            if (!object_owner(index, owner)) {
                return fail(Error::InvalidReference);
            }
            uint64_t refs[4]{};
            for (size_t field = DEPLOY_OBJECT_REF0;
                 field <= DEPLOY_OBJECT_REF3;
                 field += sizeof(uint32_t)) {
                if (!value(DEPLOY_TABLE_OBJECT, index, field, 4,
                           refs[(field - DEPLOY_OBJECT_REF0) / 4])) {
                    return fail(Error::InvalidReference);
                }
            }
            uint64_t args[6]{};
            for (size_t arg = 0; arg < 6; ++arg) {
                if (!value(DEPLOY_TABLE_OBJECT, index,
                           DEPLOY_OBJECT_ARG0 + arg * 8, 8,
                           args[arg])) {
                    return fail(Error::InvalidRecord);
                }
            }
            const bool output_b_required = kind == OBJECT_KIND_CHANNEL;
            const uint16_t required_flags =
                kind == OBJECT_KIND_PAGER
                    ? DEPLOY_OBJECT_EPHEMERAL_TASK
                    : kind == OBJECT_KIND_ENDPOINT
                        ? DEPLOY_OBJECT_POST_MAPPING
                        : DEPLOY_OBJECT_FLAG_NONE;
            if ((output_b_required && output_b_key.size() == 0)
                || (!output_b_required && output_b_key.size() != 0)
                || flags != required_flags) {
                return fail(Error::InvalidEnum);
            }
            uint64_t kind_mask{};
            if (!value(DEPLOY_TABLE_TASK, owner,
                       DEPLOY_TASK_KIND_MASK, 8, kind_mask)
                || (kind_mask & (UINT64_C(1) << kind)) == 0) {
                return fail(Error::InvalidRecord);
            }
            if (kind == OBJECT_KIND_NOTIFICATION) {
                if (args[0] == 0 || args[1] != 0 || args[2] != 0
                    || args[3] != 0 || args[4] != 0 || args[5] != 0
                    || refs[0] != DEPLOY_NO_INDEX
                    || refs[1] != DEPLOY_NO_INDEX
                    || refs[2] != DEPLOY_NO_INDEX
                    || refs[3] != DEPLOY_NO_INDEX) {
                    return fail(Error::InvalidRecord);
                }
            } else if (kind == OBJECT_KIND_CHANNEL) {
                if (args[0] == 0 || args[0] > CHANNEL_MAX_QUEUE
                    || args[1] == 0 || args[1] > CHANNEL_MAX_WORDS
                    || args[2] > CHANNEL_MAX_CAPS
                    || args[3] == 0 || args[3] > CHANNEL_MAX_RELATIONS
                    || args[4] != 0 || args[5] != 0
                    || refs[0] != DEPLOY_NO_INDEX
                    || refs[1] != DEPLOY_NO_INDEX
                    || refs[2] != DEPLOY_NO_INDEX
                    || refs[3] != DEPLOY_NO_INDEX) {
                    return fail(Error::InvalidRecord);
                }
            } else if (kind == OBJECT_KIND_PAGER) {
                if (refs[0] != DEPLOY_NO_INDEX
                    || refs[1] != DEPLOY_NO_INDEX
                    || refs[2] != DEPLOY_NO_INDEX
                    || refs[3] != DEPLOY_NO_INDEX || args[0] != 0
                    || args[1] != 0 || args[2] != 0 || args[3] != 0
                    || args[4] != 0 || args[5] != 0) {
                    return fail(Error::InvalidRecord);
                }
            } else if (kind == OBJECT_KIND_ENDPOINT) {
                if (refs[0] == DEPLOY_NO_INDEX
                    || !in_task_range(owner, DEPLOY_TABLE_MAPPING,
                                      refs[0])
                    || refs[1] != DEPLOY_NO_INDEX
                    || refs[2] != DEPLOY_NO_INDEX
                    || refs[3] != DEPLOY_NO_INDEX
                    || args[1] != 0 || args[2] != 0 || args[3] != 0
                    || args[4] != 0 || args[5] != 0) {
                    return fail(Error::InvalidRecord);
                }
            } else {
                return fail(Error::InvalidEnum);
            }
        }
        return true;
    }

    [[nodiscard]] auto object_owner(
        uint32_t index,
        uint32_t& owner) const noexcept -> bool {
        uint64_t cursor{};
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t count{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_OBJECT_COUNT, 4, count)) {
                return false;
            }
            if (index >= cursor && index < cursor + count) {
                owner = task;
                return true;
            }
            cursor += count;
        }
        return false;
    }

    [[nodiscard]] auto in_task_range(
        uint32_t task,
        uint32_t table_index,
        uint64_t index) const noexcept -> bool {
        if (task >= task_count() || table_index < DEPLOY_TABLE_IMAGE
            || table_index > DEPLOY_TABLE_EXPORT) {
            return false;
        }
        const uint32_t first_fields[7] = {
            DEPLOY_TASK_IMAGE_FIRST, DEPLOY_TASK_MAPPING_FIRST,
            DEPLOY_TASK_OBJECT_FIRST, DEPLOY_TASK_EXECUTION_FIRST,
            DEPLOY_TASK_IMPORT_FIRST, DEPLOY_TASK_DEPENDENCY_FIRST,
            DEPLOY_TASK_EXPORT_FIRST,
        };
        const uint32_t count_fields[7] = {
            DEPLOY_TASK_IMAGE_COUNT, DEPLOY_TASK_MAPPING_COUNT,
            DEPLOY_TASK_OBJECT_COUNT, DEPLOY_TASK_EXECUTION_COUNT,
            DEPLOY_TASK_IMPORT_COUNT, DEPLOY_TASK_DEPENDENCY_COUNT,
            DEPLOY_TASK_EXPORT_COUNT,
        };
        const uint32_t child = table_index - DEPLOY_TABLE_IMAGE;
        uint64_t first{};
        uint64_t count{};
        return value(DEPLOY_TABLE_TASK, task, first_fields[child], 4, first)
            && value(DEPLOY_TABLE_TASK, task, count_fields[child], 4, count)
            && index >= first && index < first + count;
    }

    [[nodiscard]] auto validate_keys(ManifestWorkspace& workspace) noexcept
        -> bool {
        for (uint32_t task = 0; task < task_count(); ++task) {
            for (ByteView& key : workspace.keys) {
                key = {};
            }
            size_t key_count{};
            auto add = [&](ByteView key) noexcept -> bool {
                if (key.size() == 0) {
                    return false;
                }
                for (size_t index = 0; index < key_count; ++index) {
                    if (workspace.keys[index].equals(key)) {
                        return false;
                    }
                }
                if (key_count == sizeof(workspace.keys)
                        / sizeof(workspace.keys[0])) {
                    return false;
                }
                workspace.keys[key_count++] = key;
                return true;
            };
            const size_t roots[] = {
                DEPLOY_TASK_POOL, DEPLOY_TASK_VSPACE,
                DEPLOY_TASK_CSPACE,
            };
            for (const size_t field : roots) {
                ByteView key{};
                if (!read_key(DEPLOY_TABLE_TASK, task, field, key, true)
                    || !add(key)) {
                    return fail(Error::DuplicateKey);
                }
            }
            uint64_t first{};
            uint64_t count{};
            const uint32_t tables[] = {
                DEPLOY_TABLE_MAPPING, DEPLOY_TABLE_OBJECT,
                DEPLOY_TABLE_EXECUTION, DEPLOY_TABLE_IMPORT,
                DEPLOY_TABLE_EXPORT,
            };
            const size_t first_fields[] = {
                DEPLOY_TASK_MAPPING_FIRST,
                DEPLOY_TASK_OBJECT_FIRST,
                DEPLOY_TASK_EXECUTION_FIRST,
                DEPLOY_TASK_IMPORT_FIRST,
                DEPLOY_TASK_EXPORT_FIRST,
            };
            const size_t count_fields[] = {
                DEPLOY_TASK_MAPPING_COUNT,
                DEPLOY_TASK_OBJECT_COUNT,
                DEPLOY_TASK_EXECUTION_COUNT,
                DEPLOY_TASK_IMPORT_COUNT,
                DEPLOY_TASK_EXPORT_COUNT,
            };
            for (size_t group = 0; group < sizeof(tables) / sizeof(tables[0]);
                 ++group) {
                if (!value(DEPLOY_TABLE_TASK, task,
                           first_fields[group], 4, first)
                    || !value(DEPLOY_TABLE_TASK, task,
                              count_fields[group], 4, count)) {
                    return fail(Error::InvalidReference);
                }
                for (uint64_t row = first; row < first + count; ++row) {
                    size_t fields[4]{};
                    size_t field_count{};
                    switch (group) {
                    case 0:
                        fields[0] = DEPLOY_MAPPING_PRODUCED;
                        fields[1] = DEPLOY_MAPPING_REGION;
                        field_count = 2;
                        break;
                    case 1:
                        fields[0] = DEPLOY_OBJECT_OUTPUT_A;
                        fields[1] = DEPLOY_OBJECT_OUTPUT_B;
                        field_count = 2;
                        break;
                    case 2:
                        fields[0] = DEPLOY_EXECUTION_KEY;
                        fields[1] = DEPLOY_EXECUTION_SC;
                        field_count = 2;
                        break;
                    case 3:
                        fields[0] = DEPLOY_IMPORT_DESTINATION;
                        field_count = 1;
                        break;
                    default:
                        /* Export keys are external publication identities;
                         * they are bounded StringRefs but not Task-local
                         * produced entries. */
                        field_count = 0;
                        break;
                    }
                    for (size_t field = 0; field < field_count; ++field) {
                        ByteView key{};
                        const bool required = !((group == 0 && field == 1)
                            || (group == 1 && field == 1));
                        if (!read_key(tables[group], static_cast<uint32_t>(row),
                                      fields[field], key, required)) {
                            return fail(Error::InvalidReference);
                        }
                        if (!required) {
                            if (key.size() != 0 && !add(key)) {
                                return fail(Error::DuplicateKey);
                            }
                            continue;
                        }
                        if (!add(key)) {
                            return fail(Error::DuplicateKey);
                        }
                    }
                }
            }
            const uint32_t ref_tables[] = {
                DEPLOY_TABLE_IMPORT, DEPLOY_TABLE_EXPORT,
            };
            const size_t ref_fields[] = {
                DEPLOY_IMPORT_SOURCE, DEPLOY_EXPORT_SOURCE,
            };
            const size_t ref_first_fields[] = {
                DEPLOY_TASK_IMPORT_FIRST, DEPLOY_TASK_EXPORT_FIRST,
            };
            const size_t ref_count_fields[] = {
                DEPLOY_TASK_IMPORT_COUNT, DEPLOY_TASK_EXPORT_COUNT,
            };
            for (size_t group = 0; group < 2; ++group) {
                if (!value(DEPLOY_TABLE_TASK, task,
                           ref_first_fields[group], 4, first)
                    || !value(DEPLOY_TABLE_TASK, task,
                              ref_count_fields[group], 4, count)) {
                    return fail(Error::InvalidReference);
                }
                for (uint64_t row = first; row < first + count; ++row) {
                    ByteView source{};
                    if (!read_key(ref_tables[group], static_cast<uint32_t>(row),
                                  ref_fields[group], source, true)) {
                        return fail(Error::InvalidReference);
                    }
                }
            }
        }
        return true;
    }

    [[nodiscard]] auto validate_executions() noexcept -> bool {
        for (uint32_t index = 0; index < execution_count(); ++index) {
            ByteView execution_key{};
            ByteView sc_key{};
            ByteView domain_key{};
            uint64_t model{};
            uint64_t flags{};
            uint64_t fault{};
            uint64_t terminal{};
            uint64_t image{};
            uint64_t stack{};
            uint64_t bootstrap{};
            uint64_t ipc{};
            uint64_t control{};
            uint64_t event{};
            uint64_t entry{};
            uint64_t stack_top{};
            uint64_t sc_budget{};
            uint64_t sc_period{};
            uint64_t urgency{};
            uint64_t home_cpu{};
            if (!read_key(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_KEY, execution_key, true)
                || !read_key(DEPLOY_TABLE_EXECUTION, index,
                             DEPLOY_EXECUTION_SC, sc_key, true)
                || !read_key(DEPLOY_TABLE_EXECUTION, index,
                             DEPLOY_EXECUTION_DOMAIN, domain_key, true)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                       DEPLOY_EXECUTION_MODEL, 2, model)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_FLAGS, 2, flags)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_FAULT, 2, fault)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_TERMINAL, 2, terminal)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_IMAGE, 4, image)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_STACK, 4, stack)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_BOOTSTRAP, 4, bootstrap)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_IPC, 4, ipc)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_CONTROL, 4, control)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_EVENT, 4, event)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_ENTRY, 8, entry)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_STACK_TOP, 8, stack_top)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_SC_BUDGET, 8, sc_budget)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_SC_PERIOD, 8, sc_period)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_URGENCY, 4, urgency)
                || !value(DEPLOY_TABLE_EXECUTION, index,
                          DEPLOY_EXECUTION_HOME_CPU, 4, home_cpu)
                || model != DEPLOY_EXECUTION_THREAD
                || flags != 0
                || fault != DEPLOY_EXECUTION_FAULT_TERMINATE
                || terminal != DEPLOY_EXECUTION_TERMINAL_LEADER_EXIT
                || sc_budget == 0 || sc_period == 0 || sc_budget > sc_period
                || urgency > DEPLOY_URGENCY_MAX
                || (home_cpu != DEPLOY_HOME_CPU_ANY
                    && home_cpu >= DEPLOY_CPU_MAX)
                || image >= image_count()
                || stack == DEPLOY_NO_INDEX
                || bootstrap == DEPLOY_NO_INDEX
                || !zero(DEPLOY_TABLE_EXECUTION, index,
                         DEPLOY_EXECUTION_RESERVED,
                         DEPLOY_EXECUTION_STRIDE)) {
                return fail(Error::InvalidRecord);
            }
            uint32_t owner{};
            if (!execution_owner(index, owner)
                || !in_task_range(owner, DEPLOY_TABLE_IMAGE, image)
                || !in_task_range(owner, DEPLOY_TABLE_MAPPING, stack)
                || !in_task_range(owner, DEPLOY_TABLE_MAPPING, bootstrap)
                || !mapping_policy(
                    static_cast<uint32_t>(stack),
                    DEPLOY_CRITICAL_STACK,
                    VM_READ | VM_WRITE,
                    VM_EXECUTE)
                || !mapping_policy(
                    static_cast<uint32_t>(bootstrap),
                    DEPLOY_CRITICAL_BOOTSTRAP,
                    VM_READ,
                    VM_WRITE | VM_EXECUTE)
                || (ipc != DEPLOY_NO_INDEX
                    && !in_task_range(owner, DEPLOY_TABLE_MAPPING, ipc))
                || (control != DEPLOY_NO_INDEX
                    && !in_task_range(owner, DEPLOY_TABLE_MAPPING, control))
                || (event != DEPLOY_NO_INDEX
                    && !in_task_range(owner, DEPLOY_TABLE_MAPPING, event))) {
                return fail(Error::InvalidReference);
            }
            if (control != DEPLOY_NO_INDEX || event != DEPLOY_NO_INDEX) {
                return fail(Error::InvalidRecord);
            }
            if ((stack_top & 0xf) != 0 || stack_top == 0) {
                return fail(Error::InvalidRecord);
            }
            uint64_t task_bootstrap{};
            if (!value(DEPLOY_TABLE_TASK, owner,
                       DEPLOY_TASK_BOOTSTRAP_MAPPING, 4,
                       task_bootstrap)
                || task_bootstrap != bootstrap) {
                return fail(Error::InvalidReference);
            }
            (void)ipc;
            (void)control;
            (void)event;
            (void)entry;
            (void)stack_top;
        }
        return true;
    }

    [[nodiscard]] auto validate_relations() noexcept -> bool {
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t object_first{};
            uint64_t object_count_value{};
            uint64_t execution_count_value{};
            uint64_t import_first{};
            uint64_t import_count_value{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_OBJECT_FIRST, 4, object_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_OBJECT_COUNT, 4,
                          object_count_value)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_EXECUTION_COUNT, 4,
                          execution_count_value)) {
                return fail(Error::InvalidReference);
            }

            uint32_t notifications{};
            uint32_t endpoints{};
            for (uint64_t local = 0; local < object_count_value; ++local) {
                const uint64_t index = object_first + local;
                uint64_t kind{};
                if (index > UINT32_MAX
                    || !value(DEPLOY_TABLE_OBJECT,
                              static_cast<uint32_t>(index),
                              DEPLOY_OBJECT_KIND, 2, kind)) {
                    return fail(Error::InvalidReference);
                }
                if (kind == OBJECT_KIND_NOTIFICATION) {
                    ++notifications;
                } else if (kind == OBJECT_KIND_ENDPOINT) {
                    ++endpoints;
                }
            }

            /* The terminal relation is the sole Notification not named by a
             * service/readiness bootstrap import.  Resolve those role
             * sources from the manifest graph here so construction does not
             * depend on object-row order or badge values. */
            uint64_t bootstrap_first{};
            uint64_t bootstrap_count_value{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_BOOTSTRAP_FIRST, 4,
                       bootstrap_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_BOOTSTRAP_COUNT, 4,
                          bootstrap_count_value)) {
                return fail(Error::InvalidReference);
            }
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_IMPORT_FIRST, 4, import_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_IMPORT_COUNT, 4,
                          import_count_value)) {
                return fail(Error::InvalidReference);
            }
            ByteView service_source{};
            ByteView readiness_source{};
            bool service_role = false;
            bool readiness_role = false;
            for (uint64_t bootstrap = 0;
                 bootstrap < bootstrap_count_value; ++bootstrap) {
                uint64_t kind{};
                if (!value(DEPLOY_TABLE_BOOTSTRAP,
                           static_cast<uint32_t>(bootstrap_first + bootstrap),
                           DEPLOY_BOOTSTRAP_KIND, 4, kind)) {
                    return fail(Error::InvalidReference);
                }
                ByteView* source = nullptr;
                bool* role = nullptr;
                if (kind == BOOT_EVENTS) {
                    source = &service_source;
                    role = &service_role;
                } else if (kind
                           == BOOT_READY) {
                    source = &readiness_source;
                    role = &readiness_role;
                }
                if (source == nullptr) {
                    continue;
                }
                if (*role) {
                    return fail(Error::DuplicateKey);
                }
                *role = true;
                ByteView destination{};
                if (!read_key(
                        DEPLOY_TABLE_BOOTSTRAP,
                        static_cast<uint32_t>(bootstrap_first + bootstrap),
                        DEPLOY_BOOTSTRAP_DESTINATION,
                        destination, true)) {
                    return fail(Error::InvalidReference);
                }
                uint32_t matches = 0;
                for (uint64_t imported = 0;
                     imported < import_count_value; ++imported) {
                    const uint32_t import_index = static_cast<uint32_t>(
                        import_first + imported);
                    ByteView imported_destination{};
                    if (!read_key(DEPLOY_TABLE_IMPORT, import_index,
                                  DEPLOY_IMPORT_DESTINATION,
                                  imported_destination, true)
                        || !imported_destination.equals(destination)) {
                        continue;
                    }
                    uint64_t source_class{};
                    if (!value(DEPLOY_TABLE_IMPORT, import_index,
                               DEPLOY_IMPORT_SOURCE_CLASS, 2,
                               source_class)
                        || source_class
                            != DEPLOY_IMPORT_SOURCE_TASK_KEY
                        || !read_key(DEPLOY_TABLE_IMPORT, import_index,
                                     DEPLOY_IMPORT_SOURCE, *source,
                                     true)) {
                        return fail(Error::InvalidReference);
                    }
                    ++matches;
                }
                if (matches != 1) {
                    return fail(Error::InvalidReference);
                }
            }
            if (service_role && readiness_role
                && service_source.equals(readiness_source)) {
                return fail(Error::DuplicateKey);
            }
            uint32_t terminal_candidates = 0;
            uint32_t service_matches = 0;
            uint32_t readiness_matches = 0;
            for (uint64_t local = 0; local < object_count_value; ++local) {
                const uint32_t index = static_cast<uint32_t>(object_first + local);
                uint64_t kind{};
                if (!value(DEPLOY_TABLE_OBJECT, index,
                           DEPLOY_OBJECT_KIND, 2, kind)) {
                    return fail(Error::InvalidReference);
                }
                if (kind != OBJECT_KIND_NOTIFICATION) {
                    continue;
                }
                ByteView output{};
                if (!read_key(DEPLOY_TABLE_OBJECT, index,
                              DEPLOY_OBJECT_OUTPUT_A, output, true)) {
                    return fail(Error::InvalidReference);
                }
                const bool is_service = service_role
                    && output.equals(service_source);
                const bool is_readiness = readiness_role
                    && output.equals(readiness_source);
                if (is_service) {
                    ++service_matches;
                }
                if (is_readiness) {
                    ++readiness_matches;
                }
                if (!is_service && !is_readiness) {
                    ++terminal_candidates;
                }
            }
            if ((execution_count_value != 0
                 && (notifications == 0 || terminal_candidates != 1))
                || (notifications > 1 && !service_role && !readiness_role)
                || (service_role && service_matches != 1)
                || (readiness_role && readiness_matches != 1)
                || (endpoints != 0 && execution_count_value != 1)) {
                return fail(Error::InvalidRecord);
            }
        }
        return true;
    }

    [[nodiscard]] auto validate_imports() noexcept -> bool {
        for (uint32_t index = 0; index < import_count(); ++index) {
            ByteView source{};
            ByteView destination{};
            uint64_t mode{};
            uint64_t selector{};
            uint64_t flags{};
            uint64_t source_class{};
            uint64_t attenuation_kind{};
            if (!read_key(DEPLOY_TABLE_IMPORT, index,
                          DEPLOY_IMPORT_SOURCE, source, true)
                || !read_key(DEPLOY_TABLE_IMPORT, index,
                             DEPLOY_IMPORT_DESTINATION, destination,
                             true)
                || !value(DEPLOY_TABLE_IMPORT, index,
                          DEPLOY_IMPORT_MODE, 2, mode)
                || !value(DEPLOY_TABLE_IMPORT, index,
                          DEPLOY_IMPORT_SELECTOR, 2, selector)
                || !value(DEPLOY_TABLE_IMPORT, index,
                          DEPLOY_IMPORT_FLAGS, 4, flags)
                || !value(DEPLOY_TABLE_IMPORT, index,
                          DEPLOY_IMPORT_SOURCE_CLASS, 2,
                          source_class)
                || !value(DEPLOY_TABLE_IMPORT, index,
                          DEPLOY_IMPORT_ATTENUATION
                              + DEPLOY_ATTENUATION_KIND,
                          2, attenuation_kind)
                /* Move remains a reserved wire value while the current
                 * escrow ABI.  Reject it at the canonical manifest-policy
                 * boundary before DeploymentPlan/Task reservation. */
                || mode >= DEPLOY_IMPORT_MOVE
                || source_class > DEPLOY_IMPORT_SOURCE_TASK_KEY
                || selector != DEPLOY_SELECTOR_ALLOCATED_KEYED
                || flags != 0
                || !zero(DEPLOY_TABLE_IMPORT, index,
                         DEPLOY_IMPORT_RESERVED,
                         DEPLOY_IMPORT_STRIDE)
                || !validate_attenuation(
                    DEPLOY_TABLE_IMPORT, index,
                    DEPLOY_IMPORT_ATTENUATION,
                    mode == DEPLOY_IMPORT_DUPLICATE)
                || (mode == DEPLOY_IMPORT_CHANNEL_MINT
                    && attenuation_kind != OBJECT_KIND_CHANNEL)
                /* TypedDelegate accepts a Channel only when its
                 * source-relative side/badge/fixed schema is valid; the
                 * closed ChannelMint path remains the only badge-minting
                 * operation.  Do not reject the kind merely by name. */
                ) {
                return fail(Error::InvalidRecord);
            }
        }
        return true;
    }

    [[nodiscard]] auto validate_bootstrap() noexcept -> bool {
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t first{};
            uint64_t count{};
            uint64_t import_first{};
            uint64_t import_count_value{};
            uint64_t readiness{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_BOOTSTRAP_FIRST, 4, first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_BOOTSTRAP_COUNT, 4, count)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_IMPORT_FIRST, 4, import_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_IMPORT_COUNT, 4,
                          import_count_value)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_READINESS, 2, readiness)) {
                return fail(Error::InvalidReference);
            }
            uint32_t readiness_roles = 0;
            if (count > DEPLOY_TASK_BOOTSTRAP_MAX) return fail(Error::InvalidRange);
            for (uint64_t local = 0; local < count; ++local) {
                const uint32_t row = static_cast<uint32_t>(first + local);
                uint64_t kind{};
                ByteView destination{};
                if (!value(DEPLOY_TABLE_BOOTSTRAP, row,
                           DEPLOY_BOOTSTRAP_KIND, 4, kind)
                    || !read_key(DEPLOY_TABLE_BOOTSTRAP, row,
                                 DEPLOY_BOOTSTRAP_DESTINATION,
                                 destination, true)
                    || !zero(DEPLOY_TABLE_BOOTSTRAP, row,
                             DEPLOY_BOOTSTRAP_RESERVED,
                             DEPLOY_BOOTSTRAP_DESTINATION)
                    || !zero(DEPLOY_TABLE_BOOTSTRAP, row,
                             DEPLOY_BOOTSTRAP_TAIL, DEPLOY_BOOTSTRAP_STRIDE)) {
                    return fail(Error::InvalidRecord);
                }
                ManifestBootstrapRow binding{};
                if (!bootstrap_row(row, binding)) return fail(Error::InvalidRecord);
                const bool named = kind == 0;
                const ByteView name = string(binding.name);
                const obj_kind_t expected_kind = named ? binding.object_kind
                    : boot_kind(static_cast<uint32_t>(kind));
                if (!valid_kind(expected_kind)
                    || (named && (name.size() == 0
                        || name.size() >= BOOT_NAME_MAX
                        || binding.protocol == 0 || binding.major == 0))
                    || (!named && !zero(DEPLOY_TABLE_BOOTSTRAP, row,
                        DEPLOY_BOOTSTRAP_NAME, DEPLOY_BOOTSTRAP_STRIDE)))
                    return fail(Error::InvalidRecord);
                for (size_t i = 0; i < name.size(); ++i)
                    if (name[i] < 33 || name[i] > 126) return fail(Error::InvalidString);
                if (kind == BOOT_READY) {
                    ++readiness_roles;
                }
                for (uint64_t previous = 0; previous < local; ++previous) {
                    uint64_t previous_kind{};
                    if (!value(DEPLOY_TABLE_BOOTSTRAP,
                               static_cast<uint32_t>(first + previous),
                               DEPLOY_BOOTSTRAP_KIND, 4,
                               previous_kind)
                        || (!named && previous_kind == kind)) {
                        return fail(Error::DuplicateKey);
                    }
                    ManifestBootstrapRow previous_binding{};
                    if (!bootstrap_row(static_cast<uint32_t>(first + previous), previous_binding)
                        || (named && previous_kind == 0
                            && string(previous_binding.name).equals(name)))
                        return fail(Error::DuplicateKey);
                }
                uint32_t matches{};
                obj_kind_t matched_kind = OBJECT_KIND_INVALID;
                for (uint64_t imported = 0;
                     imported < import_count_value; ++imported) {
                    ByteView imported_destination{};
                    uint64_t imported_kind{};
                    if (!read_key(
                            DEPLOY_TABLE_IMPORT,
                            static_cast<uint32_t>(import_first + imported),
                            DEPLOY_IMPORT_DESTINATION,
                            imported_destination, true)) {
                        return fail(Error::InvalidReference);
                    }
                    if (imported_destination.equals(destination)) {
                        if (!value(
                                DEPLOY_TABLE_IMPORT,
                                static_cast<uint32_t>(import_first + imported),
                                DEPLOY_IMPORT_ATTENUATION
                                    + DEPLOY_ATTENUATION_KIND,
                                2, imported_kind)
                            || !valid_kind(imported_kind)) {
                            return fail(Error::InvalidReference);
                        }
                        if (expected_kind
                                == OBJECT_KIND_NOTIFICATION) {
                            uint64_t source_class{};
                            uint64_t mode{};
                            uint64_t rights{};
                            if (!value(
                                    DEPLOY_TABLE_IMPORT,
                                    static_cast<uint32_t>(
                                        import_first + imported),
                                    DEPLOY_IMPORT_SOURCE_CLASS,
                                    2, source_class)
                                || !value(
                                    DEPLOY_TABLE_IMPORT,
                                    static_cast<uint32_t>(
                                        import_first + imported),
                                    DEPLOY_IMPORT_MODE, 2, mode)
                                || !value(
                                    DEPLOY_TABLE_IMPORT,
                                    static_cast<uint32_t>(
                                        import_first + imported),
                                    DEPLOY_IMPORT_ATTENUATION
                                        + DEPLOY_ATTENUATION_RIGHTS,
                                    8, rights)) {
                                return fail(Error::InvalidReference);
                            }
                            if (kind
                                    == BOOT_READY
                                && (source_class
                                        != DEPLOY_IMPORT_SOURCE_TASK_KEY
                                    || mode != DEPLOY_IMPORT_DUPLICATE
                                    || rights != RIGHT_SIGNAL)) {
                                return fail(Error::InvalidReference);
                            }
                        }
                        ++matches;
                        matched_kind = static_cast<obj_kind_t>(
                            imported_kind);
                    }
                }
                if (matches != 1 || matched_kind != expected_kind) {
                    return fail(Error::InvalidReference);
                }
            }
            if ((readiness == DEPLOY_READINESS_EXPLICIT)
                    != (readiness_roles == 1)) {
                return fail(Error::InvalidReference);
            }
        }
        return true;
    }

    [[nodiscard]] auto validate_import_sources() noexcept -> bool {
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t import_first{};
            uint64_t import_count_value{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_IMPORT_FIRST, 4, import_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_IMPORT_COUNT, 4,
                          import_count_value)) {
                return fail(Error::InvalidReference);
            }
            for (uint64_t local = 0; local < import_count_value; ++local) {
                const uint32_t row = static_cast<uint32_t>(import_first + local);
                uint64_t source_class{};
                uint64_t attenuation_kind{};
                ByteView source{};
                if (!value(DEPLOY_TABLE_IMPORT, row,
                           DEPLOY_IMPORT_SOURCE_CLASS, 2,
                           source_class)
                    || !value(DEPLOY_TABLE_IMPORT, row,
                              DEPLOY_IMPORT_ATTENUATION
                                  + DEPLOY_ATTENUATION_KIND,
                              2, attenuation_kind)
                    || !read_key(DEPLOY_TABLE_IMPORT, row,
                                 DEPLOY_IMPORT_SOURCE, source, true)) {
                    return fail(Error::InvalidReference);
                }
                if (source_class != DEPLOY_IMPORT_SOURCE_TASK_KEY) {
                    continue;
                }
                /* A TaskKey is a produced source in the task's current CSpace.
                 * Import destinations live in the child CSpace and cannot be
                 * reused as syscall sources without a separate source-CSpace
                 * ABI.  Keep that unsupported relation out of the wire
                 * language instead of admitting a constructor-only failure. */
                const auto source_kind = task_source_kind(task, source);
                if (!source_kind
                    || *source_kind
                        != static_cast<obj_kind_t>(attenuation_kind)) {
                    return fail(Error::InvalidReference);
                }
            }
        }
        return true;
    }

    [[nodiscard]] auto validate_dependencies(ManifestWorkspace& workspace)
        noexcept -> bool {
        for (auto& edges : workspace.required_edges) {
            for (bool& edge : edges) {
                edge = false;
            }
        }
        for (uint32_t index = 0; index < dependency_count(); ++index) {
            ByteView relation{};
            uint64_t target{};
            uint64_t kind{};
            uint64_t flags{};
            uint32_t owner{};
            if (!read_key(DEPLOY_TABLE_DEPENDENCY, index,
                          DEPLOY_DEPENDENCY_RELATION, relation, false)
                || !value(DEPLOY_TABLE_DEPENDENCY, index,
                       DEPLOY_DEPENDENCY_TARGET, 4, target)
                || !value(DEPLOY_TABLE_DEPENDENCY, index,
                          DEPLOY_DEPENDENCY_KIND, 2, kind)
                || !value(DEPLOY_TABLE_DEPENDENCY, index,
                          DEPLOY_DEPENDENCY_FLAGS, 2, flags)
                || target >= task_count()
                || kind > DEPLOY_DEPENDENCY_OPTIONAL
                || (flags & ~DEPLOY_DEPENDENCY_FLAGS_VALID) != 0
                || !zero(DEPLOY_TABLE_DEPENDENCY, index,
                         DEPLOY_DEPENDENCY_RESERVED,
                         DEPLOY_DEPENDENCY_STRIDE)
                || !dependency_owner(index, owner)) {
                return fail(Error::InvalidRecord);
            }
            if (owner == target || flags == 0) {
                return fail(Error::InvalidRecord);
            }
            if (kind == DEPLOY_DEPENDENCY_REQUIRED) {
                if (workspace.required_edges[owner][target]) {
                    return fail(Error::InvalidRecord);
                }
                workspace.required_edges[owner][target] = true;
            }
        }
        bool complete[DEPLOY_TASK_MAX]{};
        uint32_t completed{};
        while (completed < task_count()) {
            bool progressed{};
            for (uint32_t task = 0; task < task_count(); ++task) {
                if (complete[task]) {
                    continue;
                }
                bool blocked{};
                for (uint32_t dependency = 0;
                     dependency < dependency_count(); ++dependency) {
                    uint64_t target{};
                    if (!value(DEPLOY_TABLE_DEPENDENCY, dependency,
                               DEPLOY_DEPENDENCY_TARGET, 4, target)) {
                        return fail(Error::InvalidReference);
                    }
                    /* Ownership is checked by validate_ranges; this pass only
                     * needs the target relation once rows are task-contiguous. */
                    uint32_t owner{};
                    if (!dependency_owner(dependency, owner)) {
                        return fail(Error::InvalidReference);
                    }
                    uint64_t kind{};
                    uint64_t flags{};
                    if (!value(DEPLOY_TABLE_DEPENDENCY, dependency,
                               DEPLOY_DEPENDENCY_KIND, 2, kind)
                        || !value(DEPLOY_TABLE_DEPENDENCY, dependency,
                                  DEPLOY_DEPENDENCY_FLAGS, 2, flags)) {
                        return fail(Error::InvalidReference);
                    }
                    if (kind == DEPLOY_DEPENDENCY_REQUIRED
                        && (flags & (DEPLOY_DEPENDENCY_STARTUP | DEPLOY_DEPENDENCY_READINESS))
                        && owner == task && !complete[target]) {
                        blocked = true;
                        break;
                    }
                }
                if (!blocked) {
                    complete[task] = true;
                    ++completed;
                    progressed = true;
                }
            }
            if (!progressed) {
                return fail(Error::DependencyCycle);
            }
        }
        return true;
    }

    [[nodiscard]] auto dependency_owner(
        uint32_t index,
        uint32_t& owner) const noexcept -> bool {
        uint64_t cursor{};
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t count{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_DEPENDENCY_COUNT, 4, count)) {
                return false;
            }
            if (index >= cursor && index < cursor + count) {
                owner = task;
                return true;
            }
            cursor += count;
        }
        return false;
    }

    [[nodiscard]] auto export_owner(
        uint32_t index,
        uint32_t& owner) const noexcept -> bool {
        uint64_t cursor{};
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t count{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_EXPORT_COUNT, 4, count)) {
                return false;
            }
            if (index >= cursor && index < cursor + count) {
                owner = task;
                return true;
            }
            cursor += count;
        }
        return false;
    }

    [[nodiscard]] auto task_source_kind(
        uint32_t task,
        ByteView target) const noexcept
        -> std::optional<obj_kind_t> {
        /* Import destinations live in the child CSpace and therefore cannot
         * be a construction-time PreparedKey source.  Check this namespace
         * explicitly before accepting an equal local symbol; otherwise a
         * colliding byte string could make the import destination reachable
         * through a different row and bypass the canonical admission rule. */
        uint64_t import_first{};
        uint64_t import_count_value{};
        if (!value(DEPLOY_TABLE_TASK, task,
                   DEPLOY_TASK_IMPORT_FIRST, 4, import_first)
            || !value(DEPLOY_TABLE_TASK, task,
                      DEPLOY_TASK_IMPORT_COUNT, 4,
                      import_count_value)) {
            return std::nullopt;
        }
        for (uint64_t index = 0; index < import_count_value; ++index) {
            ByteView destination{};
            if (!read_key(DEPLOY_TABLE_IMPORT,
                          static_cast<uint32_t>(import_first + index),
                          DEPLOY_IMPORT_DESTINATION, destination, true)) {
                return std::nullopt;
            }
            if (destination.equals(target)) {
                return std::nullopt;
            }
        }

        bool found{};
        obj_kind_t found_kind = OBJECT_KIND_INVALID;
        const auto consider = [&](ByteView key,
                                  obj_kind_t kind) noexcept -> bool {
            if (!key.equals(target)) {
                return true;
            }
            if (found) {
                /* validate_keys rejects duplicate task-local keys before this
                 * path, but keep a direct resolver query unambiguous. */
                return false;
            }
            found = true;
            found_kind = kind;
            return true;
        };

        const size_t roots[] = {
            DEPLOY_TASK_POOL, DEPLOY_TASK_VSPACE,
            DEPLOY_TASK_CSPACE,
        };
        const obj_kind_t root_kinds[] = {
            OBJECT_KIND_RESOURCE_POOL,
            OBJECT_KIND_VSPACE,
            OBJECT_KIND_CSPACE,
        };
        for (size_t index = 0; index < sizeof(roots) / sizeof(roots[0]);
             ++index) {
            ByteView key{};
            if (!read_key(DEPLOY_TABLE_TASK, task, roots[index], key,
                          true)
                || !consider(key, root_kinds[index])) {
                return std::nullopt;
            }
        }
        const uint32_t tables[] = {
            DEPLOY_TABLE_MAPPING, DEPLOY_TABLE_OBJECT,
            DEPLOY_TABLE_EXECUTION,
        };
        const size_t first_fields[] = {
            DEPLOY_TASK_MAPPING_FIRST,
            DEPLOY_TASK_OBJECT_FIRST,
            DEPLOY_TASK_EXECUTION_FIRST,
        };
        const size_t count_fields[] = {
            DEPLOY_TASK_MAPPING_COUNT,
            DEPLOY_TASK_OBJECT_COUNT,
            DEPLOY_TASK_EXECUTION_COUNT,
        };
        for (size_t group = 0; group < sizeof(tables) / sizeof(tables[0]);
             ++group) {
            uint64_t first{};
            uint64_t count{};
            if (!value(DEPLOY_TABLE_TASK, task, first_fields[group], 4,
                       first)
                || !value(DEPLOY_TABLE_TASK, task, count_fields[group], 4,
                          count)) {
                return std::nullopt;
            }
            for (uint64_t row = first; row < first + count; ++row) {
                size_t fields[] = {
                    group == 0
                        ? DEPLOY_MAPPING_PRODUCED
                        : group == 1
                            ? DEPLOY_OBJECT_OUTPUT_A
                            : DEPLOY_EXECUTION_KEY,
                    group == 0
                        ? DEPLOY_MAPPING_REGION
                        : group == 1
                            ? DEPLOY_OBJECT_OUTPUT_B
                            : DEPLOY_EXECUTION_SC,
                };
                const size_t actual_field_count = 2;
                for (size_t field = 0; field < actual_field_count; ++field) {
                    ByteView key{};
                    const bool required = !((group == 0 && field == 1)
                        || (group == 1 && field == 1));
                    if (!read_key(tables[group], static_cast<uint32_t>(row),
                                  fields[field], key, required)) {
                        return std::nullopt;
                    }
                    if (!required && key.size() == 0) {
                        continue;
                    }
                    obj_kind_t kind = OBJECT_KIND_INVALID;
                    if (group == 0) {
                        kind = field == 0 ? OBJECT_KIND_MEMORY
                                          : OBJECT_KIND_VSPACE;
                    } else if (group == 1) {
                        uint64_t object_kind{};
                        if (!value(DEPLOY_TABLE_OBJECT,
                                   static_cast<uint32_t>(row),
                                   DEPLOY_OBJECT_KIND, 2,
                                   object_kind)
                            || !valid_kind(object_kind)) {
                            return std::nullopt;
                        }
                        kind = static_cast<obj_kind_t>(object_kind);
                    } else {
                        uint64_t model{};
                        if (!value(DEPLOY_TABLE_EXECUTION,
                                   static_cast<uint32_t>(row),
                                   DEPLOY_EXECUTION_MODEL, 2, model)
                            || model != DEPLOY_EXECUTION_THREAD) {
                            return std::nullopt;
                        }
                        kind = field == 0
                            ? static_cast<obj_kind_t>(
                                  OBJECT_KIND_THREAD)
                            : static_cast<obj_kind_t>(
                                  OBJECT_KIND_SCHED_CONTEXT);
                    }
                    if (!consider(key, kind)) {
                        return std::nullopt;
                    }
                }
            }
        }
        if (!found) {
            return std::nullopt;
        }
        return std::optional<obj_kind_t>{found_kind};
    }

    [[nodiscard]] auto validate_exports() noexcept -> bool {
        for (uint32_t index = 0; index < export_count(); ++index) {
            ByteView source{};
            ByteView key{};
            uint64_t export_class{};
            uint64_t flags{};
            uint64_t ceiling_kind{};
            uint32_t owner{};
            if (!read_key(DEPLOY_TABLE_EXPORT, index,
                          DEPLOY_EXPORT_SOURCE, source, true)
                || !read_key(DEPLOY_TABLE_EXPORT, index,
                             DEPLOY_EXPORT_KEY, key, true)
                || !value(DEPLOY_TABLE_EXPORT, index,
                          DEPLOY_EXPORT_CLASS, 2, export_class)
                || !value(DEPLOY_TABLE_EXPORT, index,
                          DEPLOY_EXPORT_FLAGS, 2, flags)
                || !value(DEPLOY_TABLE_EXPORT, index,
                          DEPLOY_EXPORT_CEILING
                              + DEPLOY_ATTENUATION_KIND,
                          2, ceiling_kind)
                || export_class > DEPLOY_EXPORT_RUNTIME_READY
                || flags != 0
                || !zero(DEPLOY_TABLE_EXPORT, index,
                         DEPLOY_EXPORT_RESERVED,
                         DEPLOY_EXPORT_RESERVED
                             + 4)
                || !zero(DEPLOY_TABLE_EXPORT, index,
                         DEPLOY_EXPORT_RESERVED_TAIL,
                         DEPLOY_EXPORT_STRIDE)
                || !export_owner(index, owner)
                || !validate_attenuation(
                    DEPLOY_TABLE_EXPORT, index,
                    DEPLOY_EXPORT_CEILING, false)) {
                return fail(Error::InvalidRecord);
            }
            if (export_class == DEPLOY_EXPORT_PREPARED_KEY) {
                const auto source_kind = task_source_kind(owner, source);
                if (!source_kind
                    || *source_kind
                        != static_cast<obj_kind_t>(ceiling_kind)) {
                    return fail(Error::InvalidReference);
                }
            }
            for (uint32_t previous = 0; previous < index; ++previous) {
                ByteView previous_key{};
                if (!read_key(DEPLOY_TABLE_EXPORT, previous,
                              DEPLOY_EXPORT_KEY, previous_key, true)) {
                    return fail(Error::InvalidReference);
                }
                if (previous_key.equals(key)) {
                    return fail(Error::DuplicateKey);
                }
            }
        }
        return true;
    }

    [[nodiscard]] auto validate_overlap_and_budget() noexcept -> bool {
        uint64_t critical_bytes[DEPLOY_TASK_MAX]{};
        for (uint32_t first = 0; first < mapping_count(); ++first) {
            uint64_t source{};
            if (!value(DEPLOY_TABLE_MAPPING, first,
                       DEPLOY_MAPPING_SOURCE, 2, source)) {
                return fail(Error::InvalidRange);
            }
            if (source == DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT) {
                /* The effective PT_LOAD range is supplied by the strict
                 * BootBundle cross-validator; image wire ranges are zero. */
                continue;
            }
            uint64_t first_address{};
            uint64_t first_size{};
            if (!value(DEPLOY_TABLE_MAPPING, first,
                       DEPLOY_MAPPING_ADDRESS, 8, first_address)
                || !value(DEPLOY_TABLE_MAPPING, first,
                       DEPLOY_MAPPING_SIZE, 8, first_size)) {
                return fail(Error::InvalidRange);
            }
            const uint64_t first_end = first_address + first_size;
            uint32_t first_owner{};
            if (!mapping_owner(first, first_owner)) {
                return fail(Error::InvalidReference);
            }
            for (uint32_t second = first + 1;
                 second < mapping_count(); ++second) {
                uint64_t second_address{};
                uint64_t second_size{};
                uint64_t second_source{};
                if (!value(DEPLOY_TABLE_MAPPING, second,
                           DEPLOY_MAPPING_SOURCE, 2, second_source)) {
                    return fail(Error::InvalidRange);
                }
                if (second_source == DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT) {
                    continue;
                }
                if (!value(DEPLOY_TABLE_MAPPING, second,
                           DEPLOY_MAPPING_ADDRESS, 8, second_address)
                    || !value(DEPLOY_TABLE_MAPPING, second,
                              DEPLOY_MAPPING_SIZE, 8, second_size)) {
                    return fail(Error::InvalidRange);
                }
                const uint64_t second_end = second_address + second_size;
                uint32_t second_owner{};
                if (!mapping_owner(second, second_owner)) {
                    return fail(Error::InvalidReference);
                }
                if (first_owner == second_owner
                    && first_address < second_end
                    && second_address < first_end) {
                    return fail(Error::InvalidRange);
                }
            }
            uint64_t critical{};
            if (!value(DEPLOY_TABLE_MAPPING, first,
                       DEPLOY_MAPPING_CRITICAL, 2, critical)
                || critical == DEPLOY_CRITICAL_NONE) {
                continue;
            }
            uint32_t owner{};
            if (!mapping_owner(first, owner)) {
                return fail(Error::InvalidReference);
            }
            constexpr uint64_t page = DEPLOY_PAGE_SIZE;
            if (first_size > UINT64_MAX - (page - 1)) {
                return fail(Error::CriticalBudget);
            }
            const uint64_t rounded_pages =
                (first_size + page - 1) / page;
            if (rounded_pages > UINT64_MAX / page
                || critical_bytes[owner]
                    > UINT64_MAX - rounded_pages * page) {
                return fail(Error::CriticalBudget);
            }
            critical_bytes[owner] += rounded_pages * page;
            uint64_t budget{};
            if (!value(DEPLOY_TABLE_TASK, owner,
                       DEPLOY_TASK_CRITICAL_BYTES, 8, budget)
                || critical_bytes[owner] > budget) {
                return fail(Error::CriticalBudget);
            }
        }
        return true;
    }

    [[nodiscard]] auto mapping_owner(
        uint32_t index,
        uint32_t& owner) const noexcept -> bool {
        uint64_t cursor{};
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t count{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_MAPPING_COUNT, 4, count)) {
                return false;
            }
            if (index >= cursor && index < cursor + count) {
                owner = task;
                return true;
            }
            cursor += count;
        }
        return false;
    }

    [[nodiscard]] auto execution_owner(
        uint32_t index,
        uint32_t& owner) const noexcept -> bool {
        uint64_t cursor{};
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t count{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_EXECUTION_COUNT, 4, count)) {
                return false;
            }
            if (index >= cursor && index < cursor + count) {
                owner = task;
                return true;
            }
            cursor += count;
        }
        return false;
    }

    [[nodiscard]] auto mapping_policy(
        uint32_t index,
        uint64_t critical,
        uint64_t required_access,
        uint64_t forbidden_access) const noexcept -> bool {
        uint64_t residency{};
        uint64_t mapping_critical{};
        uint64_t access{};
        return value(DEPLOY_TABLE_MAPPING, index,
                     DEPLOY_MAPPING_RESIDENCY, 2, residency)
            && value(DEPLOY_TABLE_MAPPING, index,
                     DEPLOY_MAPPING_CRITICAL, 2, mapping_critical)
            && value(DEPLOY_TABLE_MAPPING, index,
                     DEPLOY_MAPPING_ACCESS, 4, access)
            && residency == DEPLOY_MAPPING_RESIDENT
            && mapping_critical == critical
            && (access & required_access) == required_access
            && (access & forbidden_access) == 0;
    }

    [[nodiscard]] auto validate_boot_bundle_records(
        const boot::Bundle& bundle,
        ManifestWorkspace& workspace) noexcept -> bool {
        const auto same_bytes = [](ByteView left,
                                   boot::Bytes right) noexcept {
            if (left.size() != right.size()) {
                return false;
            }
            for (size_t index = 0; index < left.size(); ++index) {
                if (left[index] != right.data()[index]) {
                    return false;
                }
            }
            return true;
        };
        const auto module_for_image = [&](uint32_t image,
                                          boot::Module& result) noexcept {
            ByteView source{};
            if (!read_key(DEPLOY_TABLE_IMAGE, image,
                          DEPLOY_IMAGE_SOURCE, source, true)) {
                return false;
            }
            uint32_t matches{};
            for (size_t module_index = 0; module_index < bundle.module_count();
                 ++module_index) {
                boot::Module candidate{};
                if (!bundle.module(module_index, candidate)
                    || !candidate.bootable()
                    || !same_bytes(source, candidate.name())) {
                    continue;
                }
                result = candidate;
                ++matches;
            }
            return matches == 1;
        };
        const auto effective_mapping = [&](uint32_t mapping,
                                           EffectiveMapping& result) noexcept {
            uint64_t source{};
            uint64_t image{};
            uint64_t segment_index{};
            uint64_t address{};
            uint64_t size{};
            uint64_t access{};
            uint64_t critical{};
            if (!value(DEPLOY_TABLE_MAPPING, mapping,
                       DEPLOY_MAPPING_SOURCE, 2, source)
                || !value(DEPLOY_TABLE_MAPPING, mapping,
                          DEPLOY_MAPPING_IMAGE, 4, image)
                || !value(DEPLOY_TABLE_MAPPING, mapping,
                          DEPLOY_MAPPING_SEGMENT, 4, segment_index)
                || !value(DEPLOY_TABLE_MAPPING, mapping,
                          DEPLOY_MAPPING_ADDRESS, 8, address)
                || !value(DEPLOY_TABLE_MAPPING, mapping,
                          DEPLOY_MAPPING_SIZE, 8, size)
                || !value(DEPLOY_TABLE_MAPPING, mapping,
                          DEPLOY_MAPPING_ACCESS, 4, access)
                || !value(DEPLOY_TABLE_MAPPING, mapping,
                          DEPLOY_MAPPING_CRITICAL, 2, critical)) {
                return false;
            }
            if (source == DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT) {
                boot::Module module{};
                boot::Segment segment{};
                if (!module_for_image(static_cast<uint32_t>(image), module)
                    || !module.segment(static_cast<size_t>(segment_index),
                                       segment)) {
                    return false;
                }
                result = EffectiveMapping{
                    static_cast<uint64_t>(segment.address),
                    static_cast<uint64_t>(segment.memory_size),
                    segment.access,
                    critical,
                    source,
                };
                return true;
            }
            result = EffectiveMapping{address, size, access, critical, source};
            return true;
        };
        const auto rounded_end = [](const EffectiveMapping& mapping,
                                    uint64_t& end) noexcept {
            if (mapping.size > UINT64_MAX - (DEPLOY_PAGE_SIZE - 1)) {
                return false;
            }
            const uint64_t rounded =
                (mapping.size + DEPLOY_PAGE_SIZE - 1)
                / DEPLOY_PAGE_SIZE * DEPLOY_PAGE_SIZE;
            if (mapping.address > UINT64_MAX - rounded) {
                return false;
            }
            end = mapping.address + rounded;
            return true;
        };
        const auto special_mapping = [&](uint32_t mapping,
                                         uint64_t critical,
                                         uint64_t access) noexcept {
            EffectiveMapping effective{};
            uint64_t residency{};
            uint64_t size{};
            if (!effective_mapping(mapping, effective)
                || !value(DEPLOY_TABLE_MAPPING, mapping,
                          DEPLOY_MAPPING_RESIDENCY, 2, residency)
                || !value(DEPLOY_TABLE_MAPPING, mapping,
                          DEPLOY_MAPPING_SIZE, 8, size)
                || effective.source != DEPLOY_MAPPING_SOURCE_ZERO
                || residency != DEPLOY_MAPPING_RESIDENT
                || effective.critical != critical || effective.size != size
                || size != DEPLOY_PAGE_SIZE
                || effective.access != access
                || (effective.address % DEPLOY_PAGE_SIZE) != 0) {
                return false;
            }
            return true;
        };
        for (uint32_t task = 0; task < task_count(); ++task) {
            uint64_t mapping_first{};
            uint64_t mapping_count_value{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_MAPPING_FIRST, 4, mapping_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_MAPPING_COUNT, 4,
                          mapping_count_value)) {
                return fail(Error::InvalidBootBundle);
            }
            for (uint64_t local = 0; local < mapping_count_value; ++local) {
                EffectiveMapping& mapping = workspace.mappings[local];
                if (!effective_mapping(static_cast<uint32_t>(mapping_first + local),
                                       mapping)
                    || mapping.size == 0
                    || mapping.address < RISCV64_LOW_GUARD_END
                    || mapping.address >= RISCV64_LOWER_CANONICAL_END
                    || mapping.size > RISCV64_LOWER_CANONICAL_END
                        - mapping.address
                    || mapping.address > UINT64_MAX - mapping.size) {
                    return fail(Error::InvalidBootBundle);
                }
                uint64_t first_end{};
                if (!rounded_end(mapping, first_end)
                    || first_end > RISCV64_LOWER_CANONICAL_END) {
                    return fail(Error::InvalidBootBundle);
                }
                for (uint64_t previous = 0; previous < local; ++previous) {
                    uint64_t previous_end{};
                    if (!rounded_end(workspace.mappings[previous], previous_end)
                        || (workspace.mappings[previous].address < first_end
                            && mapping.address < previous_end)) {
                        return fail(Error::InvalidBootBundle);
                    }
                }
                if (mapping.critical == DEPLOY_CRITICAL_CODE
                    && (mapping.access & VM_EXECUTE) == 0) {
                    return fail(Error::InvalidBootBundle);
                }
            }
            uint64_t critical_bytes{};
            for (uint64_t local = 0; local < mapping_count_value; ++local) {
                if (workspace.mappings[local].critical
                    == DEPLOY_CRITICAL_NONE) {
                    continue;
                }
                uint64_t end{};
                if (!rounded_end(workspace.mappings[local], end)
                    || end < workspace.mappings[local].address
                    || critical_bytes > UINT64_MAX
                        - (end - workspace.mappings[local].address)) {
                    return fail(Error::CriticalBudget);
                }
                critical_bytes += end - workspace.mappings[local].address;
            }
            uint64_t critical_budget{};
            uint64_t pool_memory{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_CRITICAL_BYTES, 8, critical_budget)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_POOL_MEMORY, 8, pool_memory)
                || critical_bytes > critical_budget
                || critical_budget > pool_memory) {
                return fail(Error::CriticalBudget);
            }
            uint64_t object_first{};
            uint64_t object_count_value{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_OBJECT_FIRST, 4, object_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_OBJECT_COUNT, 4,
                          object_count_value)) {
                return fail(Error::InvalidBootBundle);
            }
            for (uint64_t local_object = 0;
                 local_object < object_count_value; ++local_object) {
                const uint32_t object = static_cast<uint32_t>(
                    object_first + local_object);
                uint64_t kind{};
                uint64_t mapping{};
                uint64_t descriptor_offset{};
                if (!value(DEPLOY_TABLE_OBJECT, object,
                           DEPLOY_OBJECT_KIND, 2, kind)
                    || !value(DEPLOY_TABLE_OBJECT, object,
                              DEPLOY_OBJECT_REF0, 4, mapping)
                    || !value(DEPLOY_TABLE_OBJECT, object,
                              DEPLOY_OBJECT_ARG0, 8,
                              descriptor_offset)) {
                    return fail(Error::InvalidBootBundle);
                }
                if (kind == OBJECT_KIND_ENDPOINT) {
                    uint64_t descriptor_source{};
                    uint64_t descriptor_residency{};
                    if (mapping < mapping_first
                        || mapping >= mapping_first + mapping_count_value
                        || !value(DEPLOY_TABLE_MAPPING, mapping,
                                  DEPLOY_MAPPING_SOURCE, 2,
                                  descriptor_source)
                        || !value(DEPLOY_TABLE_MAPPING, mapping,
                                  DEPLOY_MAPPING_RESIDENCY, 2,
                                  descriptor_residency)) {
                        return fail(Error::InvalidBootBundle);
                    }
                    const EffectiveMapping& descriptor =
                        workspace.mappings[mapping - mapping_first];
                    if (descriptor_source != DEPLOY_MAPPING_SOURCE_ZERO
                        || descriptor_residency
                            != DEPLOY_MAPPING_RESIDENT
                        || descriptor.source
                            == DEPLOY_MAPPING_SOURCE_PAGER
                        || descriptor_offset > descriptor.size
                        || sizeof(EpDesc)
                            > descriptor.size - descriptor_offset) {
                        return fail(Error::InvalidBootBundle);
                    }
                }
            }
            uint64_t image_first{};
            uint64_t image_count_value{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_IMAGE_FIRST, 4, image_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_IMAGE_COUNT, 4, image_count_value)) {
                return fail(Error::InvalidBootBundle);
            }
            for (uint64_t image_local = 0; image_local < image_count_value;
                 ++image_local) {
                const uint32_t image = static_cast<uint32_t>(image_first
                    + image_local);
                boot::Module module{};
                if (!module_for_image(image, module)) {
                    return fail(Error::InvalidBootBundle);
                }
                for (size_t segment = 0; segment < module.segment_count();
                     ++segment) {
                    uint32_t matches{};
                    for (uint64_t local = 0; local < mapping_count_value;
                         ++local) {
                        uint64_t source{};
                        uint64_t mapping_image{};
                        uint64_t mapping_segment{};
                        if (!value(DEPLOY_TABLE_MAPPING,
                                   static_cast<uint32_t>(mapping_first + local),
                                   DEPLOY_MAPPING_SOURCE, 2, source)
                            || !value(DEPLOY_TABLE_MAPPING,
                                      static_cast<uint32_t>(mapping_first + local),
                                      DEPLOY_MAPPING_IMAGE, 4,
                                      mapping_image)
                            || !value(DEPLOY_TABLE_MAPPING,
                                      static_cast<uint32_t>(mapping_first + local),
                                      DEPLOY_MAPPING_SEGMENT, 4,
                                      mapping_segment)) {
                            return fail(Error::InvalidBootBundle);
                        }
                        if (source == DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
                            && mapping_image == image
                            && mapping_segment == segment) {
                            ++matches;
                        }
                    }
                    if (matches != 1) {
                        return fail(Error::InvalidBootBundle);
                    }
                }
            }
            uint64_t execution_first{};
            uint64_t execution_count_value{};
            if (!value(DEPLOY_TABLE_TASK, task,
                       DEPLOY_TASK_EXECUTION_FIRST, 4, execution_first)
                || !value(DEPLOY_TABLE_TASK, task,
                          DEPLOY_TASK_EXECUTION_COUNT, 4,
                          execution_count_value)) {
                return fail(Error::InvalidBootBundle);
            }
            for (uint64_t local_execution = 0;
                 local_execution < execution_count_value; ++local_execution) {
                const uint32_t execution = static_cast<uint32_t>(
                    execution_first + local_execution);
                uint64_t image{};
                uint64_t stack{};
                uint64_t bootstrap{};
                uint64_t ipc{};
                uint64_t control{};
                uint64_t event{};
                uint64_t model{};
                uint64_t entry{};
                uint64_t stack_top{};
                if (!value(DEPLOY_TABLE_EXECUTION, execution,
                           DEPLOY_EXECUTION_IMAGE, 4, image)
                    || !value(DEPLOY_TABLE_EXECUTION, execution,
                              DEPLOY_EXECUTION_STACK, 4, stack)
                    || !value(DEPLOY_TABLE_EXECUTION, execution,
                              DEPLOY_EXECUTION_BOOTSTRAP, 4, bootstrap)
                    || !value(DEPLOY_TABLE_EXECUTION, execution,
                              DEPLOY_EXECUTION_IPC, 4, ipc)
                    || !value(DEPLOY_TABLE_EXECUTION, execution,
                              DEPLOY_EXECUTION_CONTROL, 4, control)
                    || !value(DEPLOY_TABLE_EXECUTION, execution,
                              DEPLOY_EXECUTION_EVENT, 4, event)
                    || !value(DEPLOY_TABLE_EXECUTION, execution,
                              DEPLOY_EXECUTION_MODEL, 2, model)
                    || !value(DEPLOY_TABLE_EXECUTION, execution,
                              DEPLOY_EXECUTION_ENTRY, 8, entry)
                    || !value(DEPLOY_TABLE_EXECUTION, execution,
                              DEPLOY_EXECUTION_STACK_TOP, 8, stack_top)) {
                    return fail(Error::InvalidBootBundle);
                }
                if (stack < mapping_first
                    || stack >= mapping_first + mapping_count_value
                    || bootstrap < mapping_first
                    || bootstrap >= mapping_first + mapping_count_value) {
                    return fail(Error::InvalidBootBundle);
                }
                const EffectiveMapping& stack_mapping =
                    workspace.mappings[stack - mapping_first];
                const EffectiveMapping& bootstrap_mapping =
                    workspace.mappings[bootstrap - mapping_first];
                uint64_t stack_end{};
                if (!rounded_end(stack_mapping, stack_end)
                    || (stack_top & 0xf) != 0
                    || stack_top <= stack_mapping.address
                    || stack_top > stack_end
                    || bootstrap_mapping.source
                        == DEPLOY_MAPPING_SOURCE_PAGER
                    || bootstrap_mapping.critical
                        != DEPLOY_CRITICAL_BOOTSTRAP
                    || bootstrap_mapping.access != VM_READ) {
                    return fail(Error::InvalidBootBundle);
                }
                boot::Module module{};
                if (!module_for_image(static_cast<uint32_t>(image), module)) {
                    return fail(Error::InvalidBootBundle);
                }
                const uint64_t effective_entry =
                    entry == 0 ? static_cast<uint64_t>(module.entry()) : entry;
                bool entry_mapped{};
                for (uint64_t local = 0; local < mapping_count_value; ++local) {
                    uint64_t source{};
                    uint64_t mapping_image{};
                    uint64_t mapping_segment{};
                    if (!value(DEPLOY_TABLE_MAPPING,
                               static_cast<uint32_t>(mapping_first + local),
                               DEPLOY_MAPPING_SOURCE, 2, source)
                        || !value(DEPLOY_TABLE_MAPPING,
                                  static_cast<uint32_t>(mapping_first + local),
                                  DEPLOY_MAPPING_IMAGE, 4,
                                  mapping_image)
                        || !value(DEPLOY_TABLE_MAPPING,
                                  static_cast<uint32_t>(mapping_first + local),
                                  DEPLOY_MAPPING_SEGMENT, 4,
                                  mapping_segment)) {
                        return fail(Error::InvalidBootBundle);
                    }
                    if (source != DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
                        || mapping_image != image) {
                        continue;
                    }
                    boot::Segment segment{};
                    if (!module.segment(static_cast<size_t>(mapping_segment),
                                        segment)) {
                        return fail(Error::InvalidBootBundle);
                    }
                    if (effective_entry >= segment.address
                        && effective_entry - segment.address
                            < segment.memory_size
                        && (segment.access & VM_EXECUTE) != 0) {
                        entry_mapped = true;
                    }
                }
                if (!entry_mapped) {
                    return fail(Error::InvalidBootBundle);
                }
                if (ipc != DEPLOY_NO_INDEX
                    && (ipc < mapping_first
                        || ipc >= mapping_first + mapping_count_value
                        || !special_mapping(static_cast<uint32_t>(ipc),
                                            DEPLOY_CRITICAL_IPC_HEADER,
                                            VM_READ | VM_WRITE))) {
                    return fail(Error::InvalidBootBundle);
                }
            }
        }
        return true;
    }

    [[nodiscard]] auto validate(ManifestWorkspace& workspace) noexcept -> bool {
        if (bytes_.data() == nullptr || bytes_.size() > DEPLOY_MAX_SIZE
            || bytes_.size() < 208U) {
            error_ = Error::InvalidHeader;
            return false;
        }
        if (!read_header() || !read_table_descriptors()
            || !validate_tasks() || !validate_ranges()
            || !validate_keys(workspace)
            || !validate_images()
            || !validate_mappings() || !validate_objects()
            || !validate_executions() || !validate_relations()
            || !validate_imports()
            || !validate_import_sources()
            || !validate_bootstrap()
            || !validate_dependencies(workspace) || !validate_exports()
            || !validate_overlap_and_budget()) {
            if (error_ == Error{}) {
                error_ = Error::InvalidRecord;
            }
            return false;
        }
        return true;
    }

    ByteView bytes_{};
    Table tables_[DEPLOY_TABLE_COUNT]{};
    Error error_{Error::InvalidHeader};
    uint32_t header_size_{DEPLOY_HEADER_SIZE};
    uint32_t table_count_{DEPLOY_TABLE_COUNT};
};

namespace manifest_detail {

template<typename T>
[[nodiscard]] inline auto scalar(
    const ManifestView& view,
    uint32_t table,
    uint32_t index,
    size_t field,
    size_t width,
    T& output) noexcept -> bool {
    uint64_t value{};
    if (!view.read(table, index, field, width, value)) {
        return false;
    }
    output = static_cast<T>(value);
    return true;
}

[[nodiscard]] inline auto string_ref(
    const ManifestView& view,
    uint32_t table,
    uint32_t index,
    size_t field,
    bool required,
    StringRef& output) noexcept -> bool {
    uint64_t value{};
    if (!view.read(table, index, field, 8, value)) {
        return false;
    }
    output = StringRef{
        static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32)};
    if (!required && output.length == 0) {
        return output.offset == 0;
    }
    return output.length != 0 && static_cast<bool>(view.string(output));
}

[[nodiscard]] inline auto attenuation(
    const ManifestView& view,
    uint32_t table,
    uint32_t index,
    size_t base,
    CapView& output) noexcept -> bool {
    if (!scalar(view, table, index,
                base + DEPLOY_ATTENUATION_VERSION, 2, output.version)
        || !scalar(view, table, index,
                   base + DEPLOY_ATTENUATION_KIND, 2, output.kind)
        || !scalar(view, table, index,
                   base + DEPLOY_ATTENUATION_SIZE, 4, output.size)
        || !scalar(view, table, index,
                   base + DEPLOY_ATTENUATION_RIGHTS, 8, output.rights)) {
        return false;
    }
    for (size_t word = 0; word < 6; ++word) {
        if (!scalar(view, table, index,
                    base + DEPLOY_ATTENUATION_WORD0 + word * 8,
                    8, output.words[word])) {
            return false;
        }
    }
    return true;
}

} // namespace manifest_detail

inline auto ManifestView::task_row(
    uint32_t index,
    ManifestTaskRow& output) const noexcept -> bool {
    output = {};
    return manifest_detail::string_ref(
               *this, DEPLOY_TABLE_TASK, index, DEPLOY_TASK_NAME,
               true, output.name)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_TASK, index, DEPLOY_TASK_POOL,
               true, output.pool_key)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_TASK, index, DEPLOY_TASK_VSPACE,
               true, output.vspace_key)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_TASK, index, DEPLOY_TASK_CSPACE,
               true, output.cspace_key)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_IMAGE_FIRST, 4,
                                   output.image_first)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_IMAGE_COUNT, 4,
                                   output.image_count)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_MAPPING_FIRST, 4,
                                   output.mapping_first)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_MAPPING_COUNT, 4,
                                   output.mapping_count)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_OBJECT_FIRST, 4,
                                   output.object_first)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_OBJECT_COUNT, 4,
                                   output.object_count)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_EXECUTION_FIRST, 4,
                                   output.execution_first)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_EXECUTION_COUNT, 4,
                                   output.execution_count)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_IMPORT_FIRST, 4,
                                   output.import_first)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_IMPORT_COUNT, 4,
                                   output.import_count)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_DEPENDENCY_FIRST, 4,
                                   output.dependency_first)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_DEPENDENCY_COUNT, 4,
                                   output.dependency_count)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_EXPORT_FIRST, 4,
                                   output.export_first)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_EXPORT_COUNT, 4,
                                   output.export_count)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_POOL_MEMORY, 8,
                                   output.pool_memory)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_POOL_CAPS, 8,
                                   output.pool_caps)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_KIND_MASK, 8,
                                   output.kind_mask)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_CRITICAL_BYTES, 8,
                                   output.critical_bytes)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_CSPACE_SLOTS, 4,
                                   output.cspace_slots)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_CSPACE_PAGES, 4,
                                   output.cspace_pages)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_BOOTSTRAP_MAPPING, 4,
                                   output.bootstrap_mapping)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_FLAGS, 4, output.flags)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_READINESS, 2,
                                   output.readiness)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_TERMINAL, 2,
                                   output.terminal)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_RESTART, 2,
                                   output.restart)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_READINESS_TIMEOUT_NS, 8,
                                   output.readiness_timeout_ns)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_BOOTSTRAP_FIRST, 4,
                                   output.bootstrap_first)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_TASK, index,
                                   DEPLOY_TASK_BOOTSTRAP_COUNT, 4,
                                   output.bootstrap_count)
        && manifest_detail::string_ref(*this, DEPLOY_TABLE_TASK, index,
                                      DEPLOY_TASK_ARGUMENTS, false,
                                      output.arguments);
}

inline auto ManifestView::image_row(
    uint32_t index,
    ManifestImageRow& output) const noexcept -> bool {
    output = {};
    return manifest_detail::string_ref(
               *this, DEPLOY_TABLE_IMAGE, index,
               DEPLOY_IMAGE_SOURCE, true, output.source)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_IMAGE, index,
                                   DEPLOY_IMAGE_SOURCE_KIND, 2,
                                   output.source_kind)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_IMAGE, index,
                                   DEPLOY_IMAGE_FLAGS, 2, output.flags);
}

inline auto ManifestView::mapping_row(
    uint32_t index,
    ManifestMappingRow& output) const noexcept -> bool {
    output = {};
    return manifest_detail::string_ref(
               *this, DEPLOY_TABLE_MAPPING, index,
               DEPLOY_MAPPING_PRODUCED, true, output.produced)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_MAPPING, index,
               DEPLOY_MAPPING_PAGER, false, output.pager)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_MAPPING, index,
               DEPLOY_MAPPING_REGION, false, output.region)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_IMAGE, 4, output.image)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_SEGMENT, 4,
                                   output.segment)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_SOURCE, 2,
                                   output.source)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_RESIDENCY, 2,
                                   output.residency)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_CRITICAL, 2,
                                   output.critical)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_FLAGS, 2, output.flags)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_ACCESS, 4,
                                   output.access)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_PAGER_POLICY, 4,
                                   output.pager_policy)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_ADDRESS, 8,
                                   output.address)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_MAPPING, index,
                                   DEPLOY_MAPPING_SIZE, 8, output.size);
}

inline auto ManifestView::object_row(
    uint32_t index,
    ManifestObjectRow& output) const noexcept -> bool {
    output = {};
    if (!manifest_detail::string_ref(
            *this, DEPLOY_TABLE_OBJECT, index,
            DEPLOY_OBJECT_OUTPUT, true, output.output)
        || !manifest_detail::string_ref(
            *this, DEPLOY_TABLE_OBJECT, index,
            DEPLOY_OBJECT_OUTPUT_B, false, output.output_b)
        || !manifest_detail::scalar(*this, DEPLOY_TABLE_OBJECT, index,
                                    DEPLOY_OBJECT_KIND, 2, output.kind)
        || !manifest_detail::scalar(*this, DEPLOY_TABLE_OBJECT, index,
                                    DEPLOY_OBJECT_FLAGS, 2, output.flags)) {
        return false;
    }
    for (size_t ref = 0; ref < 4; ++ref) {
        if (!manifest_detail::scalar(
                *this, DEPLOY_TABLE_OBJECT, index,
                DEPLOY_OBJECT_REF0 + ref * 4, 4, output.refs[ref])) {
            return false;
        }
    }
    for (size_t arg = 0; arg < 6; ++arg) {
        if (!manifest_detail::scalar(
                *this, DEPLOY_TABLE_OBJECT, index,
                DEPLOY_OBJECT_ARG0 + arg * 8, 8, output.args[arg])) {
            return false;
        }
    }
    return true;
}

inline auto ManifestView::execution_row(
    uint32_t index,
    ManifestExecutionRow& output) const noexcept -> bool {
    output = {};
    return manifest_detail::string_ref(
               *this, DEPLOY_TABLE_EXECUTION, index,
               DEPLOY_EXECUTION_KEY, true, output.key)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_EXECUTION, index,
               DEPLOY_EXECUTION_SC, true, output.sc)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_EXECUTION, index,
               DEPLOY_EXECUTION_DOMAIN, true, output.domain)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_IMAGE, 4,
                                   output.image)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_STACK, 4,
                                   output.stack)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_BOOTSTRAP, 4,
                                   output.bootstrap)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_IPC, 4, output.ipc)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_CONTROL, 4,
                                   output.control)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_EVENT, 4,
                                   output.event)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_MODEL, 2,
                                   output.model)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_FLAGS, 2,
                                   output.flags)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_FAULT, 2,
                                   output.fault)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_TERMINAL, 2,
                                   output.terminal)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_ENTRY, 8,
                                   output.entry)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_STACK_TOP, 8,
                                   output.stack_top)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_SC_BUDGET, 8,
                                   output.sc_budget)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_SC_PERIOD, 8,
                                   output.sc_period)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_URGENCY, 4,
                                   output.urgency)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_EXECUTION, index,
                                   DEPLOY_EXECUTION_HOME_CPU, 4,
                                   output.home_cpu);
}

inline auto ManifestView::import_row(
    uint32_t index,
    ManifestImportRow& output) const noexcept -> bool {
    output = {};
    return manifest_detail::string_ref(
               *this, DEPLOY_TABLE_IMPORT, index,
               DEPLOY_IMPORT_SOURCE, true, output.source)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_IMPORT, index,
               DEPLOY_IMPORT_DESTINATION, true, output.destination)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_IMPORT, index,
                                   DEPLOY_IMPORT_MODE, 2, output.mode)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_IMPORT, index,
                                   DEPLOY_IMPORT_SELECTOR, 2,
                                   output.selector)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_IMPORT, index,
                                   DEPLOY_IMPORT_FLAGS, 4, output.flags)
        && manifest_detail::scalar(
               *this, DEPLOY_TABLE_IMPORT, index,
               DEPLOY_IMPORT_SOURCE_CLASS, 2, output.source_class)
        && manifest_detail::attenuation(
               *this, DEPLOY_TABLE_IMPORT, index,
               DEPLOY_IMPORT_ATTENUATION, output.attenuation);
}

inline auto ManifestView::dependency_row(
    uint32_t index,
    ManifestDependencyRow& output) const noexcept -> bool {
    output = {};
    return manifest_detail::scalar(
               *this, DEPLOY_TABLE_DEPENDENCY, index,
               DEPLOY_DEPENDENCY_TARGET, 4, output.target)
        && manifest_detail::scalar(
               *this, DEPLOY_TABLE_DEPENDENCY, index,
               DEPLOY_DEPENDENCY_KIND, 2, output.kind)
        && manifest_detail::scalar(
               *this, DEPLOY_TABLE_DEPENDENCY, index,
               DEPLOY_DEPENDENCY_FLAGS, 2, output.flags)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_DEPENDENCY, index,
               DEPLOY_DEPENDENCY_RELATION, false, output.relation);
}

inline auto ManifestView::export_row(
    uint32_t index,
    ManifestExportRow& output) const noexcept -> bool {
    output = {};
    return manifest_detail::string_ref(
               *this, DEPLOY_TABLE_EXPORT, index,
               DEPLOY_EXPORT_SOURCE, true, output.source)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_EXPORT, index,
               DEPLOY_EXPORT_KEY, true, output.key)
        && manifest_detail::scalar(
               *this, DEPLOY_TABLE_EXPORT, index,
               DEPLOY_EXPORT_CLASS, 2, output.source_class)
        && manifest_detail::scalar(
               *this, DEPLOY_TABLE_EXPORT, index,
               DEPLOY_EXPORT_FLAGS, 2, output.flags)
        && manifest_detail::attenuation(
               *this, DEPLOY_TABLE_EXPORT, index,
               DEPLOY_EXPORT_CEILING, output.ceiling);
}

inline auto ManifestView::bootstrap_row(
    uint32_t index,
    ManifestBootstrapRow& output) const noexcept -> bool {
    output = {};
    return manifest_detail::scalar(
               *this, DEPLOY_TABLE_BOOTSTRAP, index,
               DEPLOY_BOOTSTRAP_KIND, 4, output.kind)
        && manifest_detail::string_ref(
               *this, DEPLOY_TABLE_BOOTSTRAP, index,
               DEPLOY_BOOTSTRAP_DESTINATION, true,
               output.destination)
        && manifest_detail::string_ref(*this, DEPLOY_TABLE_BOOTSTRAP, index,
               DEPLOY_BOOTSTRAP_NAME, false, output.name)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_BOOTSTRAP, index,
               DEPLOY_BOOTSTRAP_PROTOCOL, 4, output.protocol)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_BOOTSTRAP, index,
               DEPLOY_BOOTSTRAP_MAJOR, 2, output.major)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_BOOTSTRAP, index,
               DEPLOY_BOOTSTRAP_MINOR, 2, output.minor)
        && manifest_detail::scalar(*this, DEPLOY_TABLE_BOOTSTRAP, index,
               DEPLOY_BOOTSTRAP_OBJECT_KIND, 2, output.object_kind);
}

} // namespace deploy
