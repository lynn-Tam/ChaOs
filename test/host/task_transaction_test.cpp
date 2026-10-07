#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <libk/assert.hpp>
#include <utility>
#include <uapi/cap.h>
#include <uapi/abi.h>
#include <servers/deploy/detail/task.hpp>

#include "deploy/golden.hpp"

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    __builtin_trap();
}
} // namespace libk

namespace {

struct FakeBackend final {
    static inline status_t next_close{STATUS_OK};
    static inline status_t next_resource_close{STATUS_OK};
    static inline size_t resource_close_busy_count{};
    static inline status_t next_vspace_status{STATUS_OK};
    static inline status_t next_execution_start{STATUS_OK};
    static inline size_t execution_start_count{};
    static inline uint64_t terminal_sequence{1};
    static inline status_t terminal_status{STATUS_OK};
    static inline bool terminal_visible{};
    static inline bool publish_terminal_on_start{};
    static inline word_t notification_value{};
    static inline cap_t next_cap{100};

    static void reset() noexcept {
        next_close = STATUS_OK;
        next_resource_close = STATUS_OK;
        resource_close_busy_count = 0;
        next_vspace_status = STATUS_OK;
        next_execution_start = STATUS_OK;
        execution_start_count = 0;
        terminal_sequence = 1;
        terminal_status = STATUS_OK;
        terminal_visible = false;
        publish_terminal_on_start = false;
        notification_value = 0;
        next_cap = 100;
    }

    [[noreturn]] static void ownership_fault(status_t) noexcept {
        __builtin_trap();
    }

    [[nodiscard]] static auto close(
        sys::cap::CapRef) noexcept -> status_t {
        const status_t status = next_close;
        next_close = STATUS_OK;
        return status;
    }

    [[nodiscard]] static auto resource_create_child(
        sys::cap::CapRef,
        word_t,
        word_t,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, 10, 0};
    }

    [[nodiscard]] static auto resource_close(
        sys::cap::CapRef) noexcept -> status_t {
        if (resource_close_busy_count != 0) {
            --resource_close_busy_count;
            return STATUS_BUSY;
        }
        const status_t status = next_resource_close;
        next_resource_close = STATUS_OK;
        return status;
    }

    [[nodiscard]] static auto vspace_create(
        sys::cap::CapRef) noexcept -> sys::SysResult {
        const status_t status = next_vspace_status;
        next_vspace_status = STATUS_OK;
        return {status,
                static_cast<cap_t>(status == STATUS_OK ? 11 : 0),
                0};
    }

    [[nodiscard]] static auto cspace_create(
        sys::cap::CapRef,
        word_t,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, 12, 0};
    }

    [[nodiscard]] static auto vm_slice(
        sys::cap::CapRef,
        word_t,
        word_t,
        word_t,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, 13, 0};
    }

    [[nodiscard]] static auto vm_map(
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t,
        word_t,
        word_t,
        word_t) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto vm_unmap(
        sys::cap::CapRef,
        word_t,
        word_t) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto vm_clear(
        sys::cap::CapRef) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto memory_create(
        sys::cap::CapRef,
        word_t,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto memory_create_pager(
        sys::cap::CapRef,
        word_t,
        word_t,
        sys::cap::CapRef) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto duplicate(
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto typed_delegate(
        sys::cap::CapRef,
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto channel_mint(
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto memory_seal(
        sys::cap::CapRef) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto memory_populate(
        sys::cap::CapRef, word_t) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto memory_write(
        void* destination,
        const uint8_t* source,
        size_t size) noexcept -> status_t {
        if (destination == nullptr) {
            return STATUS_BAD_ARGS;
        }
        auto* const bytes = static_cast<uint8_t*>(destination);
        for (size_t index = 0; index < size; ++index) {
            bytes[index] = source == nullptr ? 0 : source[index];
        }
        return STATUS_OK;
    }

    [[nodiscard]] static auto sc_create(
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t,
        word_t,
        word_t,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto sc_bind(
        sys::cap::CapRef,
        sys::cap::CapRef) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto thread_create(
        sys::cap::CapRef,
        sys::cap::CapRef,
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto notification_create(
        sys::cap::CapRef,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto notification_take(
        sys::cap::CapRef) noexcept -> sys::SysResult {
        return {STATUS_OK, notification_value, 0};
    }

    [[nodiscard]] static auto channel_create(
        sys::cap::CapRef,
        word_t,
        word_t,
        word_t,
        word_t) noexcept -> sys::SysResult {
        const cap_t first = next_cap++;
        const cap_t second = next_cap++;
        return {STATUS_OK, first, second};
    }

    [[nodiscard]] static auto pager_create(sys::cap::CapRef) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto endpoint_create(
        sys::cap::CapRef,
        sys::cap::CapRef,
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto exit_bind(
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto execution_start(
        sys::cap::CapRef) noexcept -> sys::SysResult {
        ++execution_start_count;
        if (publish_terminal_on_start) {
            terminal_visible = true;
        }
        return {next_execution_start, 0, 0};
    }

    [[nodiscard]] static auto exit_query(
        sys::cap::CapRef) noexcept -> sys::SysResult {
        return {STATUS_OK, terminal_visible ? terminal_sequence : 0,
                static_cast<word_t>(terminal_status)};
    }
};

/* This backend deliberately returns from ownership_fault so the host test can
 * observe that a destructor gate never clears a live Reservation on failure.
 * Production backends are fail-stop; this only makes the no-token-loss
 * invariant observable without terminating the test process. */
struct ReturningFaultBackend final {
    static inline size_t faults{};

    static void reset() noexcept {
        faults = 0;
        FakeBackend::reset();
    }

    static void ownership_fault(status_t) noexcept { ++faults; }

    [[nodiscard]] static auto close(
        sys::cap::CapRef reference) noexcept -> status_t {
        return FakeBackend::close(reference);
    }

    [[nodiscard]] static auto resource_create_child(
        sys::cap::CapRef pool,
        word_t memory,
        word_t caps,
        word_t kinds) noexcept -> sys::SysResult {
        return FakeBackend::resource_create_child(pool, memory, caps, kinds);
    }

    [[nodiscard]] static auto resource_close(
        sys::cap::CapRef pool) noexcept -> status_t {
        return FakeBackend::resource_close(pool);
    }

    [[nodiscard]] static auto vspace_create(
        sys::cap::CapRef pool) noexcept -> sys::SysResult {
        return FakeBackend::vspace_create(pool);
    }

    [[nodiscard]] static auto cspace_create(
        sys::cap::CapRef pool,
        word_t slots,
        word_t pages) noexcept -> sys::SysResult {
        return FakeBackend::cspace_create(pool, slots, pages);
    }

    [[nodiscard]] static auto vm_slice(
        sys::cap::CapRef vspace,
        word_t address,
        word_t size,
        word_t access,
        word_t rights) noexcept -> sys::SysResult {
        return FakeBackend::vm_slice(
            vspace, address, size, access, rights);
    }

    [[nodiscard]] static auto vm_map(
        sys::cap::CapRef region,
        sys::cap::CapRef memory,
        word_t address,
        word_t size,
        word_t offset,
        word_t access) noexcept -> status_t {
        return FakeBackend::vm_map(
            region, memory, address, size, offset, access);
    }

    [[nodiscard]] static auto vm_unmap(
        sys::cap::CapRef region,
        word_t address,
        word_t size) noexcept -> status_t {
        return FakeBackend::vm_unmap(region, address, size);
    }

    [[nodiscard]] static auto vm_clear(
        sys::cap::CapRef region) noexcept -> status_t {
        return FakeBackend::vm_clear(region);
    }
};

using Space = deploy::TaskSpace<8, 8, FakeBackend>;
using Record = deploy::TaskRecord<Space>;
using Completions = deploy::CompletionSet<2, 3>;
using Table = deploy::TaskTable<Record, Completions, 2, 3>;
using Builder = deploy::TaskBuilder<Table, Completions>;

using ConstructionSpace = deploy::TaskSpace<32, 8, FakeBackend>;
using ConstructionRecord = deploy::TaskRecord<ConstructionSpace>;
using ConstructionCompletions = deploy::CompletionSet<1, 3>;
using ConstructionTable = deploy::TaskTable<
    ConstructionRecord, ConstructionCompletions, 1, 3>;
using ConstructionBuilder = deploy::TaskBuilder<
    ConstructionTable, ConstructionCompletions>;
using ConstructionBundle = deploy::MappedBundle<FakeBackend>;
using ConstructionScratch = deploy::ScratchWindow<FakeBackend>;
using ConstructionAuthorities = deploy::AuthoritySet<2, 2>;
using ConstructionWorkspace = deploy::TaskConstructionWorkspace<
    ConstructionAuthorities>;

static_assert(deploy::Backend<FakeBackend>);
static_assert(deploy::ConstructionBackend<FakeBackend>);
static_assert(deploy::Backend<ReturningFaultBackend>);
static_assert(!std::is_copy_constructible_v<Record>);
static_assert(!std::is_copy_constructible_v<Table>);

struct Fixture final {
    uint8_t raw[deploy::host::kGoldenSize]{};
    deploy::ManifestWorkspace workspace{};
    deploy::PlanSet<1> plans{};
    deploy::DeploymentPlan plan{};
};

struct ExplicitFixture final {
    uint8_t raw[4096]{};
    size_t size{};
    deploy::ManifestWorkspace workspace{};
    deploy::PlanSet<1> plans{};
    deploy::DeploymentPlan plan{};
};

struct TableShape final {
    size_t offset{};
    uint32_t count{};
    uint32_t stride{};
};

[[nodiscard]] auto read_manifest_field(
    const uint8_t* bytes,
    size_t offset,
    size_t width) noexcept -> uint64_t {
    uint64_t value = 0;
    for (size_t index = 0; index < width; ++index) {
        value |= static_cast<uint64_t>(bytes[offset + index])
            << (index * 8);
    }
    return value;
}

void put_explicit(
    uint8_t* bytes,
    size_t offset,
    uint64_t value,
    size_t width) noexcept {
    for (size_t index = 0; index < width; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

void insert_explicit(
    ExplicitFixture& fixture,
    size_t offset,
    size_t count) noexcept {
    for (size_t index = fixture.size; index > offset; --index) {
        fixture.raw[index + count - 1] = fixture.raw[index - 1];
    }
    for (size_t index = 0; index < count; ++index) {
        fixture.raw[offset + index] = 0;
    }
    fixture.size += count;
}

/* Keep the explicit-readiness fixture on the same host wire writer as the
 * canonical golden input.  It adds one child-owned readiness Notification
 * beside the existing terminal Notification and rewires the single import to
 * that TaskKey; no production selector or test-only construction path is
 * introduced. */
[[nodiscard]] auto make_explicit_plan(ExplicitFixture& fixture) noexcept
    -> bool {
    constexpr size_t legacy_header = 224;
    constexpr size_t table_count = 9;
    fixture.size = deploy::host::kGoldenSize;
    for (size_t index = 0; index < fixture.size; ++index) {
        fixture.raw[index] = deploy::host::kGolden[index];
    }
    TableShape old[table_count]{};
    for (size_t index = 0; index < table_count; ++index) {
        const size_t descriptor = DEPLOY_HEADER_TABLES
            + index * DEPLOY_TABLE_DESC_SIZE;
        old[index] = TableShape{
            static_cast<size_t>(read_manifest_field(
                fixture.raw, descriptor + DEPLOY_TABLE_OFFSET, 8)),
            static_cast<uint32_t>(read_manifest_field(
                fixture.raw, descriptor + DEPLOY_TABLE_COUNT_FIELD, 4)),
            static_cast<uint32_t>(read_manifest_field(
                fixture.raw, descriptor + DEPLOY_TABLE_STRIDE, 4))};
    }

    /* Current manifests reserve the tenth descriptor in the 224-byte header;
     * move the legacy payload before extending the object table. */
    insert_explicit(fixture, legacy_header, 16);
    for (auto& table : old) {
        table.offset += 16;
    }
    const size_t extra_object = old[DEPLOY_TABLE_OBJECT].offset
        + old[DEPLOY_TABLE_OBJECT].stride;
    insert_explicit(fixture, extra_object,
                    old[DEPLOY_TABLE_OBJECT].stride);

    TableShape tables[DEPLOY_TABLE_COUNT]{};
    for (size_t index = 0; index < table_count; ++index) {
        tables[index] = old[index];
        if (index > DEPLOY_TABLE_OBJECT) {
            tables[index].offset += old[DEPLOY_TABLE_OBJECT].stride;
        }
    }
    tables[DEPLOY_TABLE_OBJECT].count = 2;
    const size_t bootstrap_offset = (fixture.size + 7) & ~size_t{7};
    tables[DEPLOY_TABLE_BOOTSTRAP] = TableShape{
        bootstrap_offset, 1, DEPLOY_BOOTSTRAP_STRIDE};
    while (fixture.size < bootstrap_offset) {
        fixture.raw[fixture.size++] = 0;
    }
    for (size_t index = 0; index < DEPLOY_BOOTSTRAP_STRIDE; ++index) {
        fixture.raw[fixture.size + index] = 0;
    }
    fixture.size += DEPLOY_BOOTSTRAP_STRIDE;

    constexpr uint64_t notify_key = UINT64_C(0x0000000600000026);
    constexpr uint64_t authority_key = UINT64_C(0x0000000900000046);
    constexpr uint64_t import_key = UINT64_C(0x000000060000003a);
    const size_t task = tables[DEPLOY_TABLE_TASK].offset;
    put_explicit(
        fixture.raw,
        task + DEPLOY_TASK_OBJECT_COUNT,
        2,
        4);
    put_explicit(
        fixture.raw,
        task + DEPLOY_TASK_READINESS,
        DEPLOY_READINESS_EXPLICIT,
        2);
    put_explicit(fixture.raw, task + DEPLOY_TASK_READINESS_TIMEOUT_NS,
        10'000'000'000, 8);
    put_explicit(
        fixture.raw,
        task + DEPLOY_TASK_BOOTSTRAP_FIRST,
        0,
        4);
    put_explicit(
        fixture.raw,
        task + DEPLOY_TASK_BOOTSTRAP_COUNT,
        1,
        4);

    const size_t terminal = tables[DEPLOY_TABLE_OBJECT].offset
        + DEPLOY_OBJECT_STRIDE;
    put_explicit(
        fixture.raw,
        terminal + DEPLOY_OBJECT_OUTPUT_A,
        authority_key,
        8);
    put_explicit(
        fixture.raw,
        terminal + DEPLOY_OBJECT_KIND,
        OBJECT_KIND_NOTIFICATION,
        2);
    put_explicit(
        fixture.raw,
        terminal + DEPLOY_OBJECT_ARG0,
        2,
        8);
    for (size_t field = DEPLOY_OBJECT_REF0;
         field <= DEPLOY_OBJECT_REF3;
         field += sizeof(uint32_t)) {
        put_explicit(
            fixture.raw,
            terminal + field,
            DEPLOY_NO_INDEX,
            4);
    }

    const size_t import = tables[DEPLOY_TABLE_IMPORT].offset;
    put_explicit(
        fixture.raw,
        import + DEPLOY_IMPORT_SOURCE,
        notify_key,
        8);
    put_explicit(
        fixture.raw,
        import + DEPLOY_IMPORT_DESTINATION,
        import_key,
        8);
    put_explicit(
        fixture.raw,
        import + DEPLOY_IMPORT_ATTENUATION
            + DEPLOY_ATTENUATION_KIND,
        OBJECT_KIND_NOTIFICATION,
        2);
    put_explicit(
        fixture.raw,
        import + DEPLOY_IMPORT_ATTENUATION
            + DEPLOY_ATTENUATION_RIGHTS,
        RIGHT_SIGNAL,
        8);
    put_explicit(
        fixture.raw,
        import + DEPLOY_IMPORT_SOURCE_CLASS,
        DEPLOY_IMPORT_SOURCE_TASK_KEY,
        2);

    const size_t bootstrap = tables[DEPLOY_TABLE_BOOTSTRAP].offset;
    put_explicit(
        fixture.raw,
        bootstrap + DEPLOY_BOOTSTRAP_KIND,
        BOOT_READY,
        4);
    put_explicit(
        fixture.raw,
        bootstrap + DEPLOY_BOOTSTRAP_DESTINATION,
        import_key,
        8);

    put_explicit(
        fixture.raw,
        DEPLOY_HEADER_MAGIC,
        DEPLOY_MAGIC,
        8);
    put_explicit(
        fixture.raw,
        DEPLOY_HEADER_MAJOR,
        DEPLOY_MAJOR,
        2);
    put_explicit(
        fixture.raw,
        DEPLOY_HEADER_MINOR,
        DEPLOY_MINOR,
        2);
    put_explicit(
        fixture.raw,
        DEPLOY_HEADER_SIZE_FIELD,
        DEPLOY_HEADER_SIZE,
        4);
    put_explicit(
        fixture.raw,
        DEPLOY_HEADER_TOTAL_SIZE,
        fixture.size,
        8);
    put_explicit(
        fixture.raw,
        DEPLOY_HEADER_ARCHITECTURE,
        DEPLOY_ARCH_GENERIC,
        4);
    put_explicit(
        fixture.raw,
        DEPLOY_HEADER_ABI,
        DEPLOY_ABI_ID,
        4);
    put_explicit(
        fixture.raw,
        DEPLOY_HEADER_TABLE_COUNT,
        DEPLOY_TABLE_COUNT,
        4);
    for (size_t index = 0; index < DEPLOY_TABLE_COUNT; ++index) {
        const size_t descriptor = DEPLOY_HEADER_TABLES
            + index * DEPLOY_TABLE_DESC_SIZE;
        put_explicit(
            fixture.raw,
            descriptor + DEPLOY_TABLE_OFFSET,
            tables[index].count == 0 ? 0 : tables[index].offset,
            8);
        put_explicit(
            fixture.raw,
            descriptor + DEPLOY_TABLE_COUNT_FIELD,
            tables[index].count,
            4);
        put_explicit(
            fixture.raw,
            descriptor + DEPLOY_TABLE_STRIDE,
            tables[index].stride,
            4);
    }
    auto parsed = deploy::ManifestView::parse(
        fixture.raw, fixture.size, fixture.workspace);
    if (!parsed) {
        return false;
    }
    auto decoded = deploy::DeploymentPlan::decode(
        parsed.value(), fixture.plans);
    if (!decoded) {
        return false;
    }
    fixture.plan = std::move(decoded.value());
    return fixture.plan.task_count() == 1
        && fixture.plan.object_count() == 2
        && fixture.plan.bootstrap_count() == 1
        && fixture.plan.task(0) != nullptr
        && fixture.plan.task(0)->readiness
            == DEPLOY_READINESS_EXPLICIT;
}

struct DependencyFixture final {
    uint8_t raw[1400]{};
    deploy::ManifestWorkspace workspace{};
    deploy::PlanSet<1> plans{};
    deploy::DeploymentPlan plan{};
};

alignas(4096) uint8_t construction_bundle[8192]{};
alignas(4096) uint8_t construction_scratch[16384]{};
ConstructionWorkspace construction_workspace{};

void put_bundle(
    size_t offset,
    uint64_t value,
    size_t width) noexcept {
    for (size_t byte = 0; byte < width; ++byte) {
        construction_bundle[offset + byte] = static_cast<uint8_t>(
            value >> (byte * 8));
    }
}

[[nodiscard]] auto make_construction_bundle() noexcept -> size_t {
    constexpr size_t modules_offset = BUNDLE_HEADER_SIZE;
    constexpr size_t segments_offset =
        modules_offset + BUNDLE_MODULE_SIZE;
    constexpr size_t name_offset =
        segments_offset + BUNDLE_SEGMENT_SIZE;
    constexpr size_t image_offset = name_offset + 4;
    constexpr size_t image_size = 0x1000;
    constexpr size_t total_size = image_offset + image_size;
    for (size_t index = 0; index < total_size; ++index) {
        construction_bundle[index] = 0;
    }
    put_bundle(0, BUNDLE_MAGIC, 8);
    put_bundle(8, BUNDLE_MAJOR, 2);
    put_bundle(10, BUNDLE_MINOR, 2);
    put_bundle(12, BUNDLE_HEADER_SIZE, 4);
    put_bundle(16, total_size, 8);
    put_bundle(24, BUNDLE_ARCH_RISCV64, 4);
    put_bundle(28, BUNDLE_ABI_RISCV_LP64, 4);
    put_bundle(40, modules_offset, 8);
    put_bundle(48, 1, 4);
    put_bundle(56, segments_offset, 8);
    put_bundle(64, 1, 4);
    put_bundle(modules_offset, name_offset, 8);
    put_bundle(modules_offset + 8, 4, 4);
    put_bundle(modules_offset + 12, BUNDLE_MODULE_BOOTABLE, 4);
    put_bundle(modules_offset + 16, image_offset, 8);
    put_bundle(modules_offset + 24, image_size, 8);
    put_bundle(modules_offset + 32, 0x200000, 8);
    put_bundle(modules_offset + 40, 0, 4);
    put_bundle(modules_offset + 44, 1, 4);
    construction_bundle[name_offset + 0] = 'i';
    construction_bundle[name_offset + 1] = 'n';
    construction_bundle[name_offset + 2] = 'i';
    construction_bundle[name_offset + 3] = 't';
    put_bundle(segments_offset, 0x200000, 8);
    put_bundle(segments_offset + 8, image_offset, 8);
    put_bundle(segments_offset + 16, 16, 8);
    put_bundle(segments_offset + 24, image_size, 8);
    put_bundle(segments_offset + 32, 0x1000, 8);
    put_bundle(segments_offset + 40,
        BUNDLE_SEGMENT_READ | BUNDLE_SEGMENT_EXECUTE, 4);
    for (size_t index = 0; index < 16; ++index) {
        construction_bundle[image_offset + index] =
            static_cast<uint8_t>(0xa0 + index);
    }
    return total_size;
}

template<typename BuilderT>
[[nodiscard]] auto prepare_empty_task(
    BuilderT& builder,
    ConstructionAuthorities& authorities) noexcept -> bool {
    const size_t bundle_size = make_construction_bundle();
    ConstructionBundle bundle{};
    ConstructionScratch scratch{};
    const sys::cap::CapRef root{1, 0};
    const word_t bundle_address = static_cast<word_t>(
        reinterpret_cast<uintptr_t>(construction_bundle));
    const word_t scratch_address = static_cast<word_t>(
        reinterpret_cast<uintptr_t>(construction_scratch));
    if (bundle.open(
            root,
            sys::cap::CapRef{2, 0},
            deploy::Window{bundle_address, 8192},
            bundle_size)
        != STATUS_OK
        || scratch.open(
               root,
               deploy::Window{scratch_address, 16384})
            != STATUS_OK) {
        return false;
    }
    deploy::TaskAuthorityBindings bindings{};
    deploy::TaskConstructionInput<
        FakeBackend, ConstructionAuthorities> input{
        .parent_pool = root,
        .bundle = &bundle,
        .scratch = &scratch,
        .bootstrap = nullptr,
        .bootstrap_size = 0,
        .runtime_cpu_count = 1,
        .bindings = &bindings,
        .workspace = construction_workspace,
    };
    return builder.construct(input, authorities) == STATUS_OK
        && construction_workspace.empty();
}

void put_manifest(
    uint8_t* bytes,
    size_t offset,
    uint64_t value,
    size_t width) noexcept {
    for (size_t byte = 0; byte < width; ++byte) {
        bytes[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
    }
}

[[nodiscard]] auto make_plan(Fixture& fixture) noexcept -> bool {
    for (size_t index = 0; index < sizeof(fixture.raw); ++index) {
        fixture.raw[index] = deploy::host::kGolden[index];
    }
    auto parsed = deploy::ManifestView::parse(
        fixture.raw,
        sizeof(fixture.raw),
        fixture.workspace);
    if (!parsed) {
        return false;
    }
    auto decoded = deploy::DeploymentPlan::decode(
        parsed.value(), fixture.plans);
    if (!decoded) {
        return false;
    }
    fixture.plan = std::move(decoded.value());
    for (uint8_t& byte : fixture.raw) {
        byte = 0;
    }
    return fixture.plan.id() == deploy::PlanId{0, 1}
        && fixture.plan.task_count() == 1
        && fixture.plan.mapping_count() == 3;
}

[[nodiscard]] auto make_dependency_plan(
    DependencyFixture& fixture) noexcept -> bool {
    constexpr size_t shift = DEPLOY_TASK_STRIDE;
    constexpr size_t dependency_offset = deploy::host::kGoldenSize + DEPLOY_TASK_STRIDE;
    const size_t size = dependency_offset + 2 * DEPLOY_DEPENDENCY_STRIDE;
    for (size_t index = 0; index < size; ++index) {
        fixture.raw[index] = 0;
    }
    for (size_t index = 0; index < deploy::host::kGoldenSize; ++index) {
        fixture.raw[index] = deploy::host::kGolden[index];
    }
    for (size_t index = deploy::host::kGoldenSize; index > 0x188;
         --index) {
        fixture.raw[index - 1 + shift]
            = deploy::host::kGolden[index - 1];
    }
    for (size_t index = 0; index < DEPLOY_TASK_STRIDE; ++index) {
        fixture.raw[0xe0 + shift + index] = fixture.raw[0xe0 + index];
    }

    put_manifest(
        fixture.raw, DEPLOY_HEADER_TOTAL_SIZE, size, 8);
    put_manifest(
        fixture.raw,
        DEPLOY_HEADER_TABLES
            + DEPLOY_TABLE_TASK * DEPLOY_TABLE_DESC_SIZE
            + DEPLOY_TABLE_COUNT_FIELD,
        2,
        4);
    const uint64_t old_offsets[DEPLOY_TABLE_COUNT] = {
        0xe0, 0x188, 0x1a8, 0x298, 0x2f8, 0x368, 0, 0x3c8, 0x428, 0,
    };
    for (uint32_t table = DEPLOY_TABLE_IMAGE;
         table <= DEPLOY_TABLE_STRING;
         ++table) {
        const size_t descriptor = DEPLOY_HEADER_TABLES
            + table * DEPLOY_TABLE_DESC_SIZE;
        put_manifest(
            fixture.raw,
            descriptor + DEPLOY_TABLE_OFFSET,
            old_offsets[table] + shift,
            8);
    }
    const size_t dependency_descriptor = DEPLOY_HEADER_TABLES
        + DEPLOY_TABLE_DEPENDENCY * DEPLOY_TABLE_DESC_SIZE;
    put_manifest(
        fixture.raw,
        dependency_descriptor + DEPLOY_TABLE_OFFSET,
        dependency_offset,
        8);
    put_manifest(
        fixture.raw,
        dependency_descriptor + DEPLOY_TABLE_COUNT_FIELD,
        2,
        4);

    const size_t first_task = 0xe0;
    const size_t second_task = first_task + DEPLOY_TASK_STRIDE;
    put_manifest(
        fixture.raw,
        first_task + DEPLOY_TASK_DEPENDENCY_COUNT,
        1,
        4);
    const uint32_t first_fields[7] = {
        DEPLOY_TASK_IMAGE_FIRST,
        DEPLOY_TASK_MAPPING_FIRST,
        DEPLOY_TASK_OBJECT_FIRST,
        DEPLOY_TASK_EXECUTION_FIRST,
        DEPLOY_TASK_IMPORT_FIRST,
        DEPLOY_TASK_DEPENDENCY_FIRST,
        DEPLOY_TASK_EXPORT_FIRST,
    };
    const uint32_t count_fields[7] = {
        DEPLOY_TASK_IMAGE_COUNT,
        DEPLOY_TASK_MAPPING_COUNT,
        DEPLOY_TASK_OBJECT_COUNT,
        DEPLOY_TASK_EXECUTION_COUNT,
        DEPLOY_TASK_IMPORT_COUNT,
        DEPLOY_TASK_DEPENDENCY_COUNT,
        DEPLOY_TASK_EXPORT_COUNT,
    };
    const uint32_t global_counts[7] = {1, 3, 1, 1, 1, 2, 1};
    for (size_t child = 0; child < 7; ++child) {
        put_manifest(
            fixture.raw,
            second_task + first_fields[child],
            child == 5 ? 1 : global_counts[child],
            4);
        put_manifest(
            fixture.raw,
            second_task + count_fields[child],
            child == 5 ? 1 : 0,
            4);
    }
    put_manifest(
        fixture.raw,
        second_task + DEPLOY_TASK_BOOTSTRAP_MAPPING,
        DEPLOY_NO_INDEX,
        4);

    const size_t dependency = dependency_offset;
    put_manifest(
        fixture.raw,
        dependency + DEPLOY_DEPENDENCY_TARGET,
        1,
        4);
    put_manifest(
        fixture.raw,
        dependency + DEPLOY_DEPENDENCY_KIND,
        DEPLOY_DEPENDENCY_REQUIRED,
        2);
    put_manifest(
        fixture.raw,
        dependency + DEPLOY_DEPENDENCY_FLAGS,
        DEPLOY_DEPENDENCY_STARTUP,
        2);
    put_manifest(
        fixture.raw,
        dependency + DEPLOY_DEPENDENCY_STRIDE
            + DEPLOY_DEPENDENCY_TARGET,
        0,
        4);
    put_manifest(
        fixture.raw,
        dependency + DEPLOY_DEPENDENCY_STRIDE
            + DEPLOY_DEPENDENCY_KIND,
        DEPLOY_DEPENDENCY_OPTIONAL,
        2);
    put_manifest(
        fixture.raw,
        dependency + DEPLOY_DEPENDENCY_STRIDE
            + DEPLOY_DEPENDENCY_FLAGS,
        DEPLOY_DEPENDENCY_STARTUP,
        2);

    auto parsed = deploy::ManifestView::parse(
        fixture.raw, size, fixture.workspace);
    if (!parsed) {
        return false;
    }
    auto decoded = deploy::DeploymentPlan::decode(
        parsed.value(), fixture.plans);
    if (!decoded) {
        return false;
    }
    fixture.plan = std::move(decoded.value());
    for (uint8_t& byte : fixture.raw) {
        byte = 0;
    }
    fixture.workspace.reset();
    return fixture.plan.task_count() == 2
        && fixture.plan.dependency_count() == 2;
}

[[nodiscard]] auto test_dependency_rows_are_durable() noexcept -> bool {
    static DependencyFixture fixture{};
    if (!make_dependency_plan(fixture)) {
        return false;
    }
    const auto* first_task = fixture.plan.task(0);
    const auto* second_task = fixture.plan.task(1);
    const auto* first = fixture.plan.dependency(0);
    const auto* second = fixture.plan.dependency(1);
    return first_task != nullptr && second_task != nullptr
        && first != nullptr && second != nullptr
        && first_task->dependencies.first == 0
        && first_task->dependencies.count == 1
        && second_task->dependencies.first == 1
        && second_task->dependencies.count == 1
        && first->target == 1
        && first->kind == DEPLOY_DEPENDENCY_REQUIRED
        && first->flags == DEPLOY_DEPENDENCY_STARTUP
        && first->relation.empty()
        && second->target == 0
        && second->kind == DEPLOY_DEPENDENCY_OPTIONAL
        && second->flags == DEPLOY_DEPENDENCY_STARTUP
        && second->relation.empty();
}

[[nodiscard]] auto test_plan_owns_decoded_bytes() noexcept -> bool {
    static Fixture fixture{};
    if (!make_plan(fixture)) {
        return false;
    }
    auto lease = fixture.plan.lease();
    if (!lease || !lease->task(0).valid()) {
        return false;
    }
    fixture.workspace.reset();
    const auto equals = [](deploy::ByteView bytes,
                           const char* expected) noexcept -> bool {
        size_t length = 0;
        while (expected[length] != '\0') {
            ++length;
        }
        if (bytes.size() != length) {
            return false;
        }
        for (size_t index = 0; index < length; ++index) {
            if (bytes[index] != static_cast<uint8_t>(expected[index])) {
                return false;
            }
        }
        return true;
    };
    const auto zero_attenuation = [](const CapView& value) noexcept {
        if (value.rights != 0) {
            return false;
        }
        for (uint64_t word : value.words) {
            if (word != 0) {
                return false;
            }
        }
        return true;
    };
    const auto* task = fixture.plan.task(0);
    const auto* image = fixture.plan.image(0);
    const auto* code = fixture.plan.mapping(0);
    const auto* stack = fixture.plan.mapping(1);
    const auto* bootstrap = fixture.plan.mapping(2);
    const auto* object = fixture.plan.object(0);
    const auto* execution = fixture.plan.execution(0);
    const auto* import = fixture.plan.import(0);
    const auto* output = fixture.plan.export_record(0);
    if (fixture.plan.task_count() != 1
        || fixture.plan.image_count() != 1
        || fixture.plan.mapping_count() != 3
        || fixture.plan.object_count() != 1
        || fixture.plan.execution_count() != 1
        || fixture.plan.import_count() != 1
        || fixture.plan.dependency_count() != 0
        || fixture.plan.export_count() != 1
        || task == nullptr || image == nullptr || code == nullptr
        || stack == nullptr || bootstrap == nullptr || object == nullptr
        || execution == nullptr || import == nullptr || output == nullptr
        || !equals(fixture.plan.symbol(task->name), "init")
        || !equals(fixture.plan.symbol(task->pool_key), "pool")
        || !equals(fixture.plan.symbol(task->vspace_key), "vspace")
        || !equals(fixture.plan.symbol(task->cspace_key), "cspace")
        || task->pool_memory != 16384 || task->pool_caps != 16
        || task->kind_mask != DEPLOY_BASE_KINDS
        || task->critical_bytes != 12288 || task->cspace_slots != 16
        || task->cspace_pages != 1 || task->bootstrap_mapping != 2
        || task->images.first != 0 || task->images.count != 1
        || task->mappings.first != 0 || task->mappings.count != 3
        || task->objects.first != 0 || task->objects.count != 1
        || task->executions.first != 0 || task->executions.count != 1
        || task->imports.first != 0 || task->imports.count != 1
        || task->dependencies.first != 0 || task->dependencies.count != 0
        || task->exports.first != 0 || task->exports.count != 1
        || task->flags != 0 || task->readiness != 0
        || task->terminal != 0 || task->restart != 0
        || task->readiness_timeout_ns != 0
        || !equals(fixture.plan.symbol(image->source), "init")
        || image->source_kind != 0 || image->flags != 0
        || !equals(fixture.plan.symbol(code->produced), "code")
        || !code->pager.empty() || code->image != 0 || code->segment != 0
        || code->source != DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
        || code->residency != DEPLOY_MAPPING_RESIDENT
        || code->critical != DEPLOY_CRITICAL_CODE
        || code->flags != 0 || code->access != 0
        || code->address != 0 || code->size != 0
        || !equals(fixture.plan.symbol(stack->produced), "stack")
        || !stack->pager.empty()
        || stack->image != DEPLOY_NO_INDEX
        || stack->segment != DEPLOY_NO_INDEX
        || stack->source != DEPLOY_MAPPING_SOURCE_ZERO
        || stack->residency != DEPLOY_MAPPING_RESIDENT
        || stack->critical != DEPLOY_CRITICAL_STACK
        || stack->flags != 0
        || stack->access != (VM_READ | VM_WRITE)
        || stack->address != 0x210000 || stack->size != 4096
        || !equals(fixture.plan.symbol(bootstrap->produced), "bootstrap")
        || !bootstrap->pager.empty()
        || bootstrap->image != DEPLOY_NO_INDEX
        || bootstrap->segment != DEPLOY_NO_INDEX
        || bootstrap->source != DEPLOY_MAPPING_SOURCE_ZERO
        || bootstrap->residency != DEPLOY_MAPPING_RESIDENT
        || bootstrap->critical != DEPLOY_CRITICAL_BOOTSTRAP
        || bootstrap->flags != 0 || bootstrap->access != VM_READ
        || bootstrap->address != 0x220000 || bootstrap->size != 4096
        || !equals(fixture.plan.symbol(object->output), "notify")
        || !object->output_b.empty() || object->flags != 0
        || object->kind != OBJECT_KIND_NOTIFICATION
        || object->args[0] != 1
        || object->refs[0] != DEPLOY_NO_INDEX
        || object->refs[1] != DEPLOY_NO_INDEX
        || object->refs[2] != DEPLOY_NO_INDEX
        || object->refs[3] != DEPLOY_NO_INDEX
        || object->args[1] != 0 || object->args[2] != 0
        || object->args[3] != 0 || object->args[4] != 0
        || object->args[5] != 0
        || !equals(fixture.plan.symbol(execution->key), "thread")
        || !equals(fixture.plan.symbol(execution->sc), "sc")
        || !equals(fixture.plan.symbol(execution->domain), "domain")
        || execution->image != 0 || execution->stack != 1
        || execution->bootstrap != 2
        || execution->ipc != DEPLOY_NO_INDEX
        || execution->control != DEPLOY_NO_INDEX
        || execution->event != DEPLOY_NO_INDEX
        || execution->model != 0 || execution->flags != 0
        || execution->fault != 0 || execution->terminal != 0
        || execution->entry != 0x200000
        || execution->stack_top != 0x211000
        || execution->sc_budget != 1 || execution->sc_period != 1
        || execution->urgency != 0
        || execution->home_cpu != DEPLOY_HOME_CPU_ANY
        || !equals(fixture.plan.symbol(import->source), "authority")
        || !equals(fixture.plan.symbol(import->destination), "import")
        || import->mode != DEPLOY_IMPORT_DUPLICATE
        || import->selector != DEPLOY_SELECTOR_ALLOCATED_KEYED
        || import->flags != 0
        || import->attenuation.version
            != DEPLOY_ATTENUATION_VERSION_CURRENT
        || import->attenuation.kind != OBJECT_KIND_THREAD
        || import->attenuation.size != DEPLOY_ATTENUATION_STRIDE
        || !zero_attenuation(import->attenuation)
        || !equals(fixture.plan.symbol(output->source), "thread")
        || !equals(fixture.plan.symbol(output->key), "export")
        || output->source_class != DEPLOY_EXPORT_PREPARED_KEY
        || output->flags != 0
        || output->ceiling.version
            != DEPLOY_ATTENUATION_VERSION_CURRENT
        || output->ceiling.kind != OBJECT_KIND_THREAD
        || output->ceiling.size != DEPLOY_ATTENUATION_STRIDE
        || !zero_attenuation(output->ceiling)) {
        return false;
    }
    const auto name = lease->task(0).symbol(lease->task(0).row()->name);
    return name.size() == 4
        && name[0] == 'i' && name[1] == 'n'
        && name[2] == 'i' && name[3] == 't';
}

[[nodiscard]] auto test_plan_registry_lifetime() noexcept -> bool {
    using Plans = deploy::PlanSet<1>;
    static Plans plans{};
    uint8_t raw[deploy::host::kGoldenSize]{};
    deploy::ManifestWorkspace workspace{};
    deploy::TaskPlanView view{};
    {
        for (size_t index = 0; index < sizeof(raw); ++index) {
            raw[index] = deploy::host::kGolden[index];
        }
        auto parsed = deploy::ManifestView::parse(
            raw, sizeof(raw), workspace);
        if (!parsed) {
            return false;
        }
        auto decoded = deploy::DeploymentPlan::decode(parsed.value(), plans);
        if (!decoded) {
            return false;
        }
        deploy::DeploymentPlan owner = std::move(decoded.value());
        auto lease = owner.lease();
        if (!lease) {
            return false;
        }
        view = lease->task(0);
        if (!view.valid() || view.id.plan != deploy::PlanId{0, 1}) {
            return false;
        }
        deploy::DeploymentPlan moved = std::move(owner);
        if (!view.valid() || deploy::DeploymentPlan::decode(
                parsed.value(), plans)) {
            return false;
        }
        for (uint8_t& byte : raw) {
            byte = 0;
        }
    }
    if (view.valid()) {
        return false;
    }
    for (size_t index = 0; index < sizeof(raw); ++index) {
        raw[index] = deploy::host::kGolden[index];
    }
    auto parsed = deploy::ManifestView::parse(
        raw, sizeof(raw), workspace);
    if (!parsed) {
        return false;
    }
    auto decoded = deploy::DeploymentPlan::decode(parsed.value(), plans);
    return decoded && decoded.value().id() == deploy::PlanId{0, 2};
}

[[nodiscard]] auto test_plan_generation_exhaustion() noexcept -> bool {
    using Plans = deploy::PlanSet<1, 3>;
    static uint8_t raw[deploy::host::kGoldenSize]{};
    static deploy::ManifestWorkspace workspace{};
    Plans plans{};
    for (uint32_t expected = 1; expected <= 3; ++expected) {
        for (size_t index = 0; index < sizeof(raw); ++index) {
            raw[index] = deploy::host::kGolden[index];
        }
        auto parsed = deploy::ManifestView::parse(
            raw, sizeof(raw), workspace);
        if (!parsed) {
            return false;
        }
        {
            auto decoded = deploy::DeploymentPlan::decode(
                parsed.value(), plans);
            if (!decoded
                || decoded.value().id()
                    != deploy::PlanId{0, expected}) {
                return false;
            }
            auto owner = std::move(decoded.value());
            for (uint8_t& byte : raw) {
                byte = 0;
            }
        }
    }
    auto parsed = deploy::ManifestView::parse(
        deploy::host::kGolden,
        deploy::host::kGoldenSize,
        workspace);
    return parsed
        && !deploy::DeploymentPlan::decode(parsed.value(), plans);
}

[[nodiscard]] auto test_completion_lifecycle() noexcept -> bool {
    using Set = deploy::CompletionSet<1, 3>;

    Set sender_first_set{};
    auto sender_first_pair = sender_first_set.reserve();
    if (!sender_first_pair) {
        return false;
    }
    auto sender_first = sender_first_pair->take_sender();
    auto receiver_first = sender_first_pair->take_receiver();
    const auto sender_first_id = sender_first.id();
    if (sender_first.cancel()
        || !sender_first.valid() || !receiver_first.valid()
        || sender_first_set.available() != 0
        || sender_first_set.cell_state(sender_first_id)
            != deploy::CompletionCellState::Reserved
        || !receiver_first.detach()
        || !sender_first.cancel()
        || sender_first.valid() || receiver_first.valid()
        || sender_first_set.available() != 1) {
        return false;
    }

    Set pair_set{};
    {
        auto pair = pair_set.reserve();
        if (!pair) {
            return false;
        }
    }
    if (pair_set.available() != 1) {
        return false;
    }

    Set set{};

    auto first = set.reserve();
    if (!first || set.available() != 0) {
        return false;
    }
    auto sender = first->take_sender();
    auto receiver = first->take_receiver();
    const auto first_id = sender.id();
    if (!receiver.detach()
        || sender.complete({deploy::TaskId{1, 1},
                            deploy::CloseReason::Explicit,
                            STATUS_OK})
        || set.available() != 1
        || set.cell_state(first_id)
            != deploy::CompletionCellState::Retired) {
        return false;
    }

    auto second = set.reserve();
    if (!second) {
        return false;
    }
    auto second_sender = second->take_sender();
    auto second_receiver = second->take_receiver();
    const deploy::TaskId task{2, 1};
    if (!second_sender.complete({
            task, deploy::CloseReason::Terminal, STATUS_BUSY})) {
        return false;
    }
    auto result = second_receiver.take();
    if (!result || result->task != task
        || result->status != STATUS_BUSY || set.available() != 1) {
        return false;
    }

    auto third = set.reserve();
    if (!third) {
        return false;
    }
    auto third_sender = third->take_sender();
    auto third_receiver = third->take_receiver();
    if (!third_sender.complete({task, deploy::CloseReason::Explicit,
                                STATUS_OK})) {
        return false;
    }
    if (!third_receiver.detach() || set.available() != 0) {
        return false;
    }

    auto fourth = set.reserve();
    if (fourth || set.retired() != 1) {
        return false;
    }

    Set discard_set{};
    auto discard_pair = discard_set.reserve();
    if (!discard_pair) {
        return false;
    }
    auto discard_sender = discard_pair->take_sender();
    {
    auto discard_receiver = discard_pair->take_receiver();
        if (!discard_sender.complete({
                deploy::TaskId{3, 1},
                deploy::CloseReason::Explicit,
                STATUS_OK})) {
            return false;
        }
    }
    if (discard_set.available() != 1) {
        return false;
    }

    Set sealed_set{};
    auto sealed_pair = sealed_set.reserve();
    if (!sealed_pair) {
        return false;
    }
    auto sealed_sender = sealed_pair->take_sender();
    auto sealed_receiver = sealed_pair->take_receiver();
    sealed_sender.seal();
    if (sealed_sender.cancel()
        || !sealed_sender.complete({
            deploy::TaskId{4, 1},
            deploy::CloseReason::Explicit,
            STATUS_OK})
        || !sealed_receiver.take()) {
        return false;
    }
    return sealed_set.available() == 1;
}

[[nodiscard]] auto test_reservation_and_capacity_recovery() noexcept -> bool {
    static Fixture fixture{};
    if (!make_plan(fixture)) {
        return false;
    }
    Completions completions{};
    Table table{};
    deploy::TaskId first_id{};
    {
        auto lease = fixture.plan.lease();
        if (!lease) {
            return false;
        }
        auto builder = Builder::begin(
            completions, table, std::move(*lease), 0);
        if (!builder || builder->record() == nullptr) {
            return false;
        }
        first_id = builder->record()->id();
    }
    if (table.tag(first_id) != deploy::TaskSlotTag::Vacant
        || completions.available() != completions.capacity()) {
        return false;
    }
    {
        auto lease = fixture.plan.lease();
        if (!lease) {
            return false;
        }
        auto builder = Builder::begin(
            completions, table, std::move(*lease), 0);
        if (!builder || builder->record() == nullptr
            || builder->record()->id() != first_id) {
            return false;
        }
    }
    if (table.tag(first_id) != deploy::TaskSlotTag::Vacant
        || completions.available() != completions.capacity()) {
        return false;
    }

    using SmallTable = deploy::TaskTable<Record, Completions, 1, 3>;
    using SmallBuilder = deploy::TaskBuilder<SmallTable, Completions>;
    Completions pressure{};
    SmallTable small_table{};
    auto lease = fixture.plan.lease();
    if (!lease) {
        return false;
    }
    {
        auto first = SmallBuilder::begin(
            pressure, small_table, std::move(*lease), 0);
        if (!first || pressure.available() != 1) {
            return false;
        }
        const auto id = first->record()->id();
        auto second_lease = fixture.plan.lease();
        if (!second_lease
            || SmallBuilder::begin(
                   pressure, small_table, std::move(*second_lease), 0)
            || small_table.tag(id) != deploy::TaskSlotTag::Reserved
            || pressure.available() != 1) {
            return false;
        }
    }
    return small_table.tag(deploy::TaskId{0, 1})
            == deploy::TaskSlotTag::Vacant
        && pressure.available() == pressure.capacity();
}

[[nodiscard]] auto test_builder_cancel_retains_owner() noexcept -> bool {
    static Fixture fixture{};
    if (!make_plan(fixture)) {
        return false;
    }
    Completions completions{};
    Table table{};
    auto lease = fixture.plan.lease();
    if (!lease) {
        return false;
    }
    auto builder = Builder::begin(
        completions, table, std::move(*lease), 0);
    if (!builder || builder->record() == nullptr) {
        return false;
    }
    const auto id = builder->record()->id();
    auto receiver = builder->take_receiver();
    if (!receiver
        || builder->cancel()
        || !builder->valid()
        || table.tag(id) != deploy::TaskSlotTag::Reserved
        || completions.available() != completions.capacity() - 1) {
        return false;
    }
    if (!receiver->detach()
        || !builder->cancel()
        || builder->valid()
        || table.tag(id) != deploy::TaskSlotTag::Vacant
        || completions.available() != completions.capacity()) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_resourceful_destructor_fail_stop() noexcept -> bool {
    using FaultSpace = deploy::TaskSpace<8, 8, ReturningFaultBackend>;

    ReturningFaultBackend::reset();
    {
        FaultSpace space{};
        if (space.open(
                sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
                != STATUS_OK) {
            return false;
        }
    }
    return ReturningFaultBackend::faults != 0;
}

[[nodiscard]] auto test_checked_task_projections() noexcept -> bool {
    FakeBackend::reset();
    Space space{};
    if (space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    const auto local = space.vspace_slot();
    const auto local_ref = space.lookup(local, OBJECT_KIND_VSPACE);
    if (!local_ref || local_ref->selector != 11 || local_ref->cspace != 0) {
        return false;
    }
    if (space.lookup(local, OBJECT_KIND_THREAD)) {
        return false;
    }
    const auto manager_ref = space.lookup(
        space.manager_slot(), OBJECT_KIND_CSPACE);
    if (!manager_ref) {
        return false;
    }
    auto remote_owner = Space::owner_type{
        sys::cap::CapRef{99, manager_ref->selector}};
    const auto remote_index = space.adopt_remote_index(
        std::move(remote_owner));
    if (!remote_index) {
        return false;
    }
    const auto remote_ref = space.lookup_remote(
        *remote_index, manager_ref->selector);
    if (!remote_ref || remote_ref->selector != 99
        || remote_ref->cspace != manager_ref->selector) {
        return false;
    }
    if (space.lookup_remote(*remote_index, manager_ref->selector + 1)) {
        return false;
    }
    return space.close() == STATUS_OK;
}

[[nodiscard]] auto test_table_transfer_and_close() noexcept -> bool {
    static DependencyFixture fixture{};
    if (!make_dependency_plan(fixture)) {
        return false;
    }
    auto lease = fixture.plan.lease();
    if (!lease) {
        return false;
    }
    Completions completions{};
    Table table{};
    auto builder = Builder::begin(
        completions, table, std::move(*lease), 1);
    if (!builder) {
        return false;
    }
    ConstructionAuthorities authorities{};
    if (!prepare_empty_task(*builder, authorities)) {
        return false;
    }
    auto receiver = builder->take_receiver();
    if (!receiver || builder->record() == nullptr
        || builder->record()->state() != deploy::TaskState::Constructing) {
        return false;
    }
    const deploy::TaskId id = builder->record()->id();
    if (table.transition(id, deploy::TaskState::Prepared)
        || table.transition(id, deploy::TaskState::Starting)
        || table.transition(id, deploy::TaskState::Running)
        || table.transition(id, deploy::TaskState::Closing)
        || table.transition(id, deploy::TaskState::Reclaimed)
        || builder->record()->state()
            != deploy::TaskState::Constructing
        || !builder->commit_prepared()
        || table.tag(id) != deploy::TaskSlotTag::Record
        || table.transition(deploy::TaskId{id.slot, id.generation + 1},
                            deploy::TaskState::Starting)
        || table.transition(id, deploy::TaskState::Constructing)
        || table.transition(id, deploy::TaskState::Prepared)
        || table.transition(id, deploy::TaskState::Running)
        || table.transition(id, deploy::TaskState::Reclaimed)
        || table.record(id)->state() != deploy::TaskState::Prepared
        || !table.transition(id, deploy::TaskState::Starting)
        || table.transition(id, deploy::TaskState::Constructing)
        || table.transition(id, deploy::TaskState::Prepared)
        || table.transition(id, deploy::TaskState::Starting)
        || table.transition(id, deploy::TaskState::Reclaimed)
        || table.record(id)->state() != deploy::TaskState::Starting
        || !table.transition(id, deploy::TaskState::Running)
        || table.transition(id, deploy::TaskState::Constructing)
        || table.transition(id, deploy::TaskState::Prepared)
        || table.transition(id, deploy::TaskState::Starting)
        || table.transition(id, deploy::TaskState::Running)
        || table.transition(id, deploy::TaskState::Reclaimed)
        || table.record(id)->state() != deploy::TaskState::Running
        || !table.transition(id, deploy::TaskState::Failed)
        || table.transition(id, deploy::TaskState::Constructing)
        || table.transition(id, deploy::TaskState::Prepared)
        || table.transition(id, deploy::TaskState::Starting)
        || table.transition(id, deploy::TaskState::Running)
        || table.transition(id, deploy::TaskState::Failed)
        || table.transition(id, deploy::TaskState::Reclaimed)
        || table.record(id)->state() != deploy::TaskState::Failed
        || !table.begin_close(id, deploy::CloseReason::Terminal,
                              STATUS_CANCELED)
        || table.tag(id) != deploy::TaskSlotTag::Closing
        || table.closing(id) == nullptr
        || table.transition(id, deploy::TaskState::Running)
        || table.continue_close(id) != STATUS_OK) {
        return false;
    }
    auto result = receiver->take();
    if (!result || result->task != id
        || result->reason != deploy::CloseReason::Terminal
        || result->status != STATUS_CANCELED
        || table.tag(id) != deploy::TaskSlotTag::Retired) {
        return false;
    }
    return table.record(id) == nullptr
        && table.closing(id) == nullptr
        && !table.transition(id, deploy::TaskState::Running)
        && table.tag(deploy::TaskId{id.slot, id.generation + 1})
            == deploy::TaskSlotTag::Vacant;
}

[[nodiscard]] auto test_pressure_precedes_table() noexcept -> bool {
    static Fixture fixture{};
    if (!make_plan(fixture)) {
        return false;
    }
    Completions completions{};
    Table table{};
    auto held = completions.reserve();
    auto held_second = completions.reserve();
    if (!held || !held_second) {
        return false;
    }
    auto lease = fixture.plan.lease();
    if (!lease || Builder::begin(completions, table, std::move(*lease), 0)) {
        return false;
    }
    return table.tag(deploy::TaskId{0, 1})
        == deploy::TaskSlotTag::Vacant;
}

[[nodiscard]] auto test_resource_failure_moves_to_closing() noexcept -> bool {
    FakeBackend::reset();
    Space space{};
    if (space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
            != STATUS_OK) {
        return false;
    }
    FakeBackend::reset();
    FakeBackend::next_resource_close = STATUS_BUSY;
    if (space.close() != STATUS_BUSY
        || space.phase() != deploy::Phase::ResourceClosing
        || space.close() != STATUS_OK
        || space.phase() != deploy::Phase::Closed) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_partial_open_failure_moves_to_closing() noexcept
    -> bool {
    static DependencyFixture fixture{};
    if (!make_dependency_plan(fixture)) {
        return false;
    }

    FakeBackend::reset();
    FakeBackend::next_vspace_status = STATUS_NO_MEMORY;
    /* TaskSpace::open consumes the first BUSY while unwinding the partial
     * aggregate; the table-owned close consumes the second and succeeds on
     * its retry. */
    FakeBackend::resource_close_busy_count = 2;

    const size_t bundle_size = make_construction_bundle();
    ConstructionBundle bundle{};
    ConstructionScratch scratch{};
    const sys::cap::CapRef root{1, 0};
    const word_t bundle_address = static_cast<word_t>(
        reinterpret_cast<uintptr_t>(construction_bundle));
    const word_t scratch_address = static_cast<word_t>(
        reinterpret_cast<uintptr_t>(construction_scratch));
    if (bundle.open(
            root,
            sys::cap::CapRef{2, 0},
            deploy::Window{bundle_address, 8192},
            bundle_size)
            != STATUS_OK
        || scratch.open(
               root,
               deploy::Window{scratch_address, 16384})
            != STATUS_OK) {
        return false;
    }

    deploy::TaskAuthorityBindings bindings{};
    deploy::TaskConstructionInput<
        FakeBackend, ConstructionAuthorities> input{
        .parent_pool = root,
        .bundle = &bundle,
        .scratch = &scratch,
        .bootstrap = nullptr,
        .bootstrap_size = 0,
        .runtime_cpu_count = 1,
        .bindings = &bindings,
        .workspace = construction_workspace,
    };
    ConstructionAuthorities authorities{};
    ConstructionCompletions completions{};
    ConstructionTable table{};
    auto plan = fixture.plan.lease();
    if (!plan) {
        return false;
    }
    auto builder = ConstructionBuilder::begin(
        completions, table, std::move(*plan), 1);
    if (!builder || builder->record() == nullptr) {
        return false;
    }
    const deploy::TaskId id = builder->record()->id();
    auto receiver = builder->take_receiver();
    if (!receiver) {
        return false;
    }
    const status_t status = builder->construct(input, authorities);
    if (status != STATUS_NO_MEMORY
        || builder->valid()
        || !construction_workspace.empty()
        || table.tag(id) != deploy::TaskSlotTag::Closing) {
        return false;
    }
    const status_t first_close = table.continue_close(id);
    const auto first_tag = table.tag(id);
    const status_t second_close = table.continue_close(id);
    if (first_close != STATUS_BUSY
        || first_tag != deploy::TaskSlotTag::Closing
        || second_close != STATUS_OK
        || table.tag(deploy::TaskId{
                         id.slot, id.generation + 1})
            != deploy::TaskSlotTag::Vacant) {
        return false;
    }
    const auto result = receiver->take();
    return result && result->task == id
        && result->reason == deploy::CloseReason::ConstructionFailure
        && result->status == STATUS_NO_MEMORY;
}

[[nodiscard]] auto test_public_readiness_and_terminal_paths() noexcept
    -> bool {
    static ExplicitFixture fixture{};
    if (!make_explicit_plan(fixture)) {
        return false;
    }
    FakeBackend::reset();

    const size_t bundle_size = make_construction_bundle();
    ConstructionBundle bundle{};
    ConstructionScratch scratch{};
    const sys::cap::CapRef root{1, 0};
    const word_t bundle_address = static_cast<word_t>(
        reinterpret_cast<uintptr_t>(construction_bundle));
    const word_t scratch_address = static_cast<word_t>(
        reinterpret_cast<uintptr_t>(construction_scratch));
    if (bundle.open(
            root,
            sys::cap::CapRef{2, 0},
            deploy::Window{bundle_address, 8192},
            bundle_size)
            != STATUS_OK
        || scratch.open(
               root,
               deploy::Window{scratch_address, 16384})
            != STATUS_OK) {
        return false;
    }

    ConstructionSpace source_space{};
    if (source_space.open(root, 16384, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    const auto domain_slot = source_space.adopt_local(
        ConstructionSpace::owner_type{sys::cap::CapRef{91, 0}},
        OBJECT_KIND_SCHED_DOMAIN);
    if (!domain_slot) {
        return false;
    }
    deploy::RegisteredSpace<ConstructionSpace, 2> source{};
    if (!source.adopt(std::move(source_space))) {
        return false;
    }
    const CapView domain_ceiling{
        .version = CAP_ATTENUATION_VERSION_CURRENT,
        .kind = OBJECT_KIND_SCHED_DOMAIN,
        .size = CAP_ATTENUATION_SIZE,
        .rights = RIGHT_DUPLICATE | RIGHT_CONTROL,
        .words = {},
    };
    ConstructionAuthorities authorities{};
    const auto domain = source.register_source(
        authorities, *domain_slot, UINT64_C(0x4558504c444f4d41),
        domain_ceiling);
    if (!domain) {
        return false;
    }

    ConstructionCompletions completions{};
    ConstructionTable table{};
    auto lease = fixture.plan.lease();
    if (!lease) {
        return false;
    }
    auto builder = ConstructionBuilder::begin(
        completions, table, std::move(*lease), 0);
    if (!builder || builder->record() == nullptr) {
        return false;
    }
    const deploy::TaskId id = builder->record()->id();
    auto receiver = builder->take_receiver();
    if (!receiver) {
        return false;
    }
    deploy::TaskAuthorityBindings bindings{};
    bindings.domains[0] = *domain;
    deploy::TaskConstructionInput<
        FakeBackend, ConstructionAuthorities> input{
        .parent_pool = root,
        .bundle = &bundle,
        .scratch = &scratch,
        .bootstrap = nullptr,
        .bootstrap_size = 0,
        .runtime_cpu_count = 1,
        .bindings = &bindings,
        .workspace = construction_workspace,
    };
    if (builder->construct(input, authorities) != STATUS_OK
        || !builder->commit_prepared()) {
        return false;
    }
    const auto stale_export = table.register_prepared_export(
        deploy::TaskId{id.slot, id.generation + 1}, 0, authorities);
    const auto wrong_export = table.register_prepared_export(
        id, 1, authorities);
    const auto prepared_export = table.register_prepared_export(
        id, 0, authorities);
    const auto duplicate_export = table.register_prepared_export(
        id, 0, authorities);
    if (stale_export || wrong_export || !prepared_export
        || !prepared_export->valid() || duplicate_export) {
        return false;
    }
    if (table.terminal_notification(
            deploy::TaskId{id.slot, id.generation + 1})) {
        return false;
    }
    if (table.terminal_notification(id)) {
        return false;
    }
    if (table.start(id) != STATUS_OK
        || table.record(id) == nullptr
        || table.record(id)->ready()
        || table.consume_readiness(id) != STATUS_RETRY
        || table.record(id)->ready()) {
        return false;
    }
    const auto terminal = table.terminal_notification(id);
    if (!terminal || terminal->cspace != 0) {
        return false;
    }
    FakeBackend::notification_value = 1;
    if (table.consume_readiness(id) != STATUS_OK
        || table.record(id) == nullptr
        || !table.record(id)->ready()
        || table.consume_readiness(id) != STATUS_RETRY) {
        return false;
    }
    FakeBackend::terminal_visible = false;
    const auto empty_terminal = table.observe_terminal(id);
    if (empty_terminal.status != STATUS_OK
        || empty_terminal.value != 0) {
        return false;
    }
    const bool began_close = table.begin_close(
        id, deploy::CloseReason::Explicit, STATUS_OK);
    const status_t closing_readiness = table.consume_readiness(id);
    const auto closing_terminal = table.observe_terminal(id);
    const auto closing_notification = table.terminal_notification(id);
    if (!began_close || closing_readiness != STATUS_INVALID_CAP
        || closing_terminal.status != STATUS_INVALID_CAP
        || closing_notification) {
        return false;
    }
    if (table.continue_close(id) != STATUS_OK) {
        return false;
    }
    const auto result = receiver->take();
    if (!result || result->task != id
        || result->reason != deploy::CloseReason::Explicit
        || result->status != STATUS_OK
        || table.record(id) != nullptr) {
        return false;
    }
    if (source.close() != STATUS_OK
        || scratch.close() != STATUS_OK
        || bundle.close() != STATUS_OK) {
        return false;
    }
    return authorities.active_entries() == 0
        && construction_workspace.empty();
}

[[nodiscard]] auto test_task_generation_exhaustion() noexcept -> bool {
    static DependencyFixture fixture{};
    if (!make_dependency_plan(fixture)) {
        return false;
    }
    using SmallCompletions = deploy::CompletionSet<1, 3>;
    using SmallTable = deploy::TaskTable<Record, SmallCompletions, 1, 3>;
    using SmallBuilder = deploy::TaskBuilder<SmallTable, SmallCompletions>;
    SmallCompletions completions{};
    SmallTable table{};
    for (uint32_t expected = 1; expected <= 3; ++expected) {
        auto lease = fixture.plan.lease();
        if (!lease) {
            return false;
        }
        auto builder = SmallBuilder::begin(
            completions, table, std::move(*lease), 1);
        if (!builder) {
            return false;
        }
        ConstructionAuthorities authorities{};
        if (!prepare_empty_task(*builder, authorities)) {
            return false;
        }
        auto receiver = builder->take_receiver();
        if (!receiver || builder->record() == nullptr
            || builder->record()->id().generation != expected) {
            return false;
        }
        const deploy::TaskId id = builder->record()->id();
        if (!builder->commit_prepared()
            || !table.begin_close(id, deploy::CloseReason::Explicit,
                                  STATUS_OK)
            || table.continue_close(id) != STATUS_OK) {
            return false;
        }
        auto result = receiver->take();
        if (!result || result->task != id) {
            return false;
        }
        const auto next = deploy::TaskId{0, expected + 1};
        if (expected < 3) {
            if (table.tag(next) != deploy::TaskSlotTag::Vacant) {
                return false;
            }
        } else if (table.tag(next) != deploy::TaskSlotTag::Retired) {
            return false;
        }
    }
    auto lease = fixture.plan.lease();
    return lease && !SmallBuilder::begin(
        completions, table, std::move(*lease), 0);
}

[[nodiscard]] auto test_finite_construction_path() noexcept -> bool {
    static Fixture fixture{};
    if (!make_plan(fixture)) {
        return false;
    }

    FakeBackend::reset();
    const size_t bundle_size = make_construction_bundle();
    ConstructionSpace source_space{};
    if (source_space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    const auto domain_slot = source_space.adopt_local(
        ConstructionSpace::owner_type{sys::cap::CapRef{91, 0}},
        OBJECT_KIND_SCHED_DOMAIN);
    const auto import_slot = source_space.adopt_local(
        ConstructionSpace::owner_type{sys::cap::CapRef{92, 0}},
        OBJECT_KIND_THREAD);
    if (!domain_slot || !import_slot) {
        return false;
    }
    deploy::RegisteredSpace<ConstructionSpace, 2> source{};
    if (!source.adopt(std::move(source_space))) {
        return false;
    }
    ConstructionAuthorities authorities{};
    const CapView domain_ceiling{
        .version = CAP_ATTENUATION_VERSION_CURRENT,
        .kind = OBJECT_KIND_SCHED_DOMAIN,
        .size = CAP_ATTENUATION_SIZE,
        .rights = RIGHT_MASK,
        .words = {},
    };
    const CapView thread_ceiling{
        .version = CAP_ATTENUATION_VERSION_CURRENT,
        .kind = OBJECT_KIND_THREAD,
        .size = CAP_ATTENUATION_SIZE,
        .rights = RIGHT_MASK,
        .words = {},
    };
    const auto domain = source.register_source(
        authorities, *domain_slot, 91, domain_ceiling);
    const auto import = source.register_source(
        authorities, *import_slot, 92, thread_ceiling);
    if (!domain || !import) {
        return false;
    }

    ConstructionBundle bundle{};
    ConstructionScratch scratch{};
    const sys::cap::CapRef root{1, 0};
    const word_t bundle_address = static_cast<word_t>(
        reinterpret_cast<uintptr_t>(construction_bundle));
    const word_t scratch_address = static_cast<word_t>(
        reinterpret_cast<uintptr_t>(construction_scratch));
    if (bundle.open(
            root,
            sys::cap::CapRef{2, 0},
            deploy::Window{bundle_address, 8192},
            bundle_size)
        != STATUS_OK
        || scratch.open(
               root,
               deploy::Window{scratch_address, 16384})
            != STATUS_OK
        || bundle.phase() != deploy::LeasePhase::Mapped
        || scratch.phase() != deploy::LeasePhase::Ready) {
        return false;
    }
    deploy::TaskAuthorityBindings bindings{};
    bindings.domains[0] = *domain;
    bindings.imports[0] = *import;
    deploy::TaskConstructionInput<
        FakeBackend, ConstructionAuthorities> input{
        .parent_pool = root,
        .bundle = &bundle,
        .scratch = &scratch,
        .bootstrap = "boot",
        .bootstrap_size = 4,
        .runtime_cpu_count = 1,
        .bindings = &bindings,
        .workspace = construction_workspace,
    };

    ConstructionCompletions completions{};
    ConstructionTable table{};
    auto plan = fixture.plan.lease();
    if (!plan) {
        return false;
    }
    auto builder = ConstructionBuilder::begin(
        completions, table, std::move(*plan), 0);
    const status_t construction_status = builder
        ? builder->construct(input, authorities) : STATUS_BAD_ARGS;
    if (!builder || construction_status != STATUS_OK) {
        return false;
    }
    const auto projections = builder->record()->projections();
    if (builder->record()->state() != deploy::TaskState::Constructing
        || !projections.vspace.valid() || !projections.cspace.valid()
        || !projections.bootstrap.valid()
        || !projections.mappings[0].valid()
        || !projections.mappings[1].valid()
        || !projections.mappings[2].valid()
        || !projections.objects[0].valid()
        || !projections.executions[0].valid()
        || !projections.scheduling_contexts[0].valid()
        || !projections.imports[0].valid()
        || !projections.relations[0].valid()
        || !projections.exports[0].valid()
        || projections.exports[0].kind != OBJECT_KIND_THREAD
        || builder->record()->accounting().total_bytes != 12288
        || builder->record()->accounting().by_class[
               DEPLOY_CRITICAL_CODE] != 4096
        || builder->record()->accounting().by_class[
               DEPLOY_CRITICAL_STACK] != 4096
        || builder->record()->accounting().by_class[
               DEPLOY_CRITICAL_BOOTSTRAP] != 4096) {
        return false;
    }
    if (!construction_workspace.empty()) {
        return false;
    }
    const deploy::TaskId task = builder->record()->id();
    auto* const record_before_commit = builder->record();
    if (!builder->commit_prepared()
        || builder->valid()
        || table.record(task) != record_before_commit
        || table.record(task)->state() != deploy::TaskState::Prepared) {
        return false;
    }
    FakeBackend::reset();
    if (table.start(task) != STATUS_OK
        || table.record(task) == nullptr
        || table.record(task)->state() != deploy::TaskState::Running
        || !table.record(task)->ready()
        || FakeBackend::execution_start_count != 1) {
        return false;
    }
    FakeBackend::terminal_visible = true;
    const auto observation = table.observe_terminal(task);
    if (observation.status != STATUS_OK
        || observation.value != FakeBackend::terminal_sequence
        || table.consume_terminal(task, observation) != STATUS_OK
        || table.record(task) == nullptr
        || table.record(task)->state()
            != deploy::TaskState::Terminating
        || table.record(task)->ready()) {
        return false;
    }
    auto receiver = builder->take_receiver();
    if (!receiver
        || !table.begin_close(
            task, deploy::CloseReason::Terminal,
            STATUS_OK)
        || table.continue_close(task) != STATUS_OK) {
        return false;
    }
    const auto result = receiver->take();
    if (!result || result->task != task
        || result->reason != deploy::CloseReason::Terminal
        || result->status != STATUS_OK) {
        return false;
    }

    /* A terminal published from execution_start is an ordinary runtime
     * terminal because the start submission was accepted.  The record first
     * publishes Running; the canonical observation/consumption path then
     * records and closes that terminal. */
    auto early_terminal_plan = fixture.plan.lease();
    if (!early_terminal_plan) {
        return false;
    }
    auto early_terminal_builder = ConstructionBuilder::begin(
        completions, table, std::move(*early_terminal_plan), 0);
    if (!early_terminal_builder
        || early_terminal_builder->record() == nullptr) {
        return false;
    }
    const deploy::TaskId early_terminal_task =
        early_terminal_builder->record()->id();
    FakeBackend::terminal_visible = false;
    FakeBackend::publish_terminal_on_start = true;
    FakeBackend::terminal_sequence = 2;
    FakeBackend::terminal_status = STATUS_OK;
    if (early_terminal_builder->construct(input, authorities)
            != STATUS_OK
        || !early_terminal_builder->commit_prepared()) {
        return false;
    }
    auto early_terminal_receiver = early_terminal_builder->take_receiver();
    if (!early_terminal_receiver
        || table.start(early_terminal_task) != STATUS_OK
        || table.record(early_terminal_task) == nullptr
        || table.record(early_terminal_task)->state()
            != deploy::TaskState::Running
        || table.record(early_terminal_task)->terminal_sequence() != 0) {
        return false;
    }
    const auto early_observation = table.observe_terminal(early_terminal_task);
    if (early_observation.status != STATUS_OK
        || early_observation.value != 2
        || table.consume_terminal(early_terminal_task, early_observation)
            != STATUS_OK
        || table.record(early_terminal_task) == nullptr
        || table.record(early_terminal_task)->state()
            != deploy::TaskState::Terminating
        || table.record(early_terminal_task)->terminal_sequence() != 2
        || table.record(early_terminal_task)->terminal_status()
            != STATUS_OK) {
        return false;
    }
    if (!table.begin_close(
            early_terminal_task,
            deploy::CloseReason::Terminal,
            STATUS_OK)
        || table.continue_close(early_terminal_task) != STATUS_OK) {
        return false;
    }
    const auto early_terminal_result = early_terminal_receiver->take();
    if (!early_terminal_result
        || early_terminal_result->task != early_terminal_task
        || early_terminal_result->reason
            != deploy::CloseReason::Terminal
        || early_terminal_result->status != STATUS_OK) {
        return false;
    }

    auto plan_after_success = fixture.plan.lease();
    if (!plan_after_success) {
        return false;
    }
    auto failed_builder = ConstructionBuilder::begin(
        completions, table, std::move(*plan_after_success), 0);
    if (!failed_builder || failed_builder->record() == nullptr) {
        return false;
    }
    const deploy::TaskId failed_task = failed_builder->record()->id();
    auto invalid_input = input;
    invalid_input.bootstrap_size = 8192;
    const status_t failed_status = failed_builder->construct(
        invalid_input, authorities);
    if (failed_status != STATUS_BAD_ARGS
        || failed_builder->valid()
        || !construction_workspace.empty()
        || table.tag(failed_task) != deploy::TaskSlotTag::Closing) {
        return false;
    }
    FakeBackend::next_resource_close = STATUS_BUSY;
    if (table.continue_close(failed_task) != STATUS_BUSY
        || table.tag(failed_task) != deploy::TaskSlotTag::Closing
        || !construction_workspace.empty()
        || table.continue_close(failed_task) != STATUS_OK) {
        return false;
    }
    auto failed_receiver = failed_builder->take_receiver();
    if (!failed_receiver) {
        return false;
    }
    const auto failed_result = failed_receiver->take();
    if (!failed_result || failed_result->task != failed_task
        || failed_result->reason
            != deploy::CloseReason::ConstructionFailure
        || failed_result->status != STATUS_BAD_ARGS) {
        return false;
    }

    auto held = authorities.lease(*domain);
    if (!held || authorities.live_leases() != 1
        || source.close() != STATUS_BUSY) {
        /* A source close starts retirement but cannot bypass the reciprocal
         * registration while this independent lease pins the entry. */
        return false;
    }
    held.reset();
    return source.close() == STATUS_OK
        && authorities.active_entries() == 0;
}

using Test = bool (*)() noexcept;

} // namespace

int main() {
    const Test tests[] = {
        test_plan_owns_decoded_bytes,
        test_dependency_rows_are_durable,
        test_plan_registry_lifetime,
        test_plan_generation_exhaustion,
        test_completion_lifecycle,
        test_reservation_and_capacity_recovery,
        test_builder_cancel_retains_owner,
        test_resourceful_destructor_fail_stop,
        test_checked_task_projections,
        test_table_transfer_and_close,
        test_pressure_precedes_table,
        test_resource_failure_moves_to_closing,
        test_partial_open_failure_moves_to_closing,
        test_public_readiness_and_terminal_paths,
        test_task_generation_exhaustion,
        test_finite_construction_path,
    };
    size_t passed = 0;
    for (const Test test : tests) {
        if (test()) {
            ++passed;
        }
    }
    printf("task transaction: passed=%zu failed=%zu\n",
           passed, sizeof(tests) / sizeof(tests[0]) - passed);
    return passed == sizeof(tests) / sizeof(tests[0]) ? 0 : 1;
}
