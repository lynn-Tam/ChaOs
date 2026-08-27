#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <libk/assert.hpp>
#include <libk/utility.hpp>
#include <uapi/resource.h>
#include <uapi/status.h>
#include <user/lib/task_transaction.hpp>

#include "deploypack/golden_fixture.hpp"

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    __builtin_trap();
}
} // namespace libk

namespace {

struct FakeBackend final {
    static inline myos_status_t next_close{MYOS_STATUS_OK};
    static inline myos_status_t next_resource_close{MYOS_STATUS_OK};
    static inline size_t resource_close_busy_count{};
    static inline myos_status_t next_vspace_status{MYOS_STATUS_OK};
    static inline myos_cap_t next_cap{100};

    static void reset() noexcept {
        next_close = MYOS_STATUS_OK;
        next_resource_close = MYOS_STATUS_OK;
        resource_close_busy_count = 0;
        next_vspace_status = MYOS_STATUS_OK;
        next_cap = 100;
    }

    [[noreturn]] static void ownership_fault(myos_status_t) noexcept {
        __builtin_trap();
    }

    [[nodiscard]] static auto close(
        myos::cap::CapRef) noexcept -> myos_status_t {
        const myos_status_t status = next_close;
        next_close = MYOS_STATUS_OK;
        return status;
    }

    [[nodiscard]] static auto resource_create_child(
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, 10, 0};
    }

    [[nodiscard]] static auto resource_close(
        myos::cap::CapRef) noexcept -> myos_status_t {
        if (resource_close_busy_count != 0) {
            --resource_close_busy_count;
            return MYOS_STATUS_BUSY;
        }
        const myos_status_t status = next_resource_close;
        next_resource_close = MYOS_STATUS_OK;
        return status;
    }

    [[nodiscard]] static auto vspace_create(
        myos::cap::CapRef) noexcept -> myos::SysResult {
        const myos_status_t status = next_vspace_status;
        next_vspace_status = MYOS_STATUS_OK;
        return {status,
                static_cast<myos_cap_t>(status == MYOS_STATUS_OK ? 11 : 0),
                0};
    }

    [[nodiscard]] static auto cspace_create(
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, 12, 0};
    }

    [[nodiscard]] static auto vm_create_region(
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t,
        myos_word_t,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, 13, 0};
    }

    [[nodiscard]] static auto vm_map(
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t,
        myos_word_t,
        myos_word_t) noexcept -> myos_status_t {
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto vm_unmap(
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t) noexcept -> myos_status_t {
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto vm_destroy_region(
        myos::cap::CapRef) noexcept -> myos_status_t {
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto memory_create(
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto memory_create_pager(
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t,
        myos::cap::CapRef) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto duplicate(
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto typed_delegate(
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto channel_mint(
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto memory_seal(
        myos::cap::CapRef) noexcept -> myos_status_t {
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto memory_write(
        void* destination,
        const uint8_t* source,
        size_t size) noexcept -> myos_status_t {
        if (destination == nullptr) {
            return MYOS_STATUS_BAD_ARGS;
        }
        auto* const bytes = static_cast<uint8_t*>(destination);
        for (size_t index = 0; index < size; ++index) {
            bytes[index] = source == nullptr ? 0 : source[index];
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto sc_create(
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto sc_bind(
        myos::cap::CapRef,
        myos::cap::CapRef) noexcept -> myos_status_t {
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto thread_create(
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto vproc_create(
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto notification_create(
        myos::cap::CapRef,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto channel_create(
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        const myos_cap_t first = next_cap++;
        const myos_cap_t second = next_cap++;
        return {MYOS_STATUS_OK, first, second};
    }

    [[nodiscard]] static auto pager_create(
        myos::cap::CapRef,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto endpoint_create(
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos_word_t) noexcept -> myos::SysResult {
        return {MYOS_STATUS_OK, next_cap++, 0};
    }

    [[nodiscard]] static auto terminal_observe_bind(
        myos::cap::CapRef,
        myos::cap::CapRef,
        myos_word_t) noexcept -> myos_status_t {
        return MYOS_STATUS_OK;
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

    static void ownership_fault(myos_status_t) noexcept { ++faults; }

    [[nodiscard]] static auto close(
        myos::cap::CapRef reference) noexcept -> myos_status_t {
        return FakeBackend::close(reference);
    }

    [[nodiscard]] static auto resource_create_child(
        myos::cap::CapRef pool,
        myos_word_t memory,
        myos_word_t caps,
        myos_word_t kinds) noexcept -> myos::SysResult {
        return FakeBackend::resource_create_child(pool, memory, caps, kinds);
    }

    [[nodiscard]] static auto resource_close(
        myos::cap::CapRef pool) noexcept -> myos_status_t {
        return FakeBackend::resource_close(pool);
    }

    [[nodiscard]] static auto vspace_create(
        myos::cap::CapRef pool) noexcept -> myos::SysResult {
        return FakeBackend::vspace_create(pool);
    }

    [[nodiscard]] static auto cspace_create(
        myos::cap::CapRef pool,
        myos_word_t slots,
        myos_word_t pages) noexcept -> myos::SysResult {
        return FakeBackend::cspace_create(pool, slots, pages);
    }

    [[nodiscard]] static auto vm_create_region(
        myos::cap::CapRef vspace,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        myos_word_t types,
        myos_word_t rights) noexcept -> myos::SysResult {
        return FakeBackend::vm_create_region(
            vspace, address, size, access, types, rights);
    }

    [[nodiscard]] static auto vm_map(
        myos::cap::CapRef region,
        myos::cap::CapRef memory,
        myos_word_t address,
        myos_word_t size,
        myos_word_t offset,
        myos_word_t access) noexcept -> myos_status_t {
        return FakeBackend::vm_map(
            region, memory, address, size, offset, access);
    }

    [[nodiscard]] static auto vm_unmap(
        myos::cap::CapRef region,
        myos_word_t address,
        myos_word_t size) noexcept -> myos_status_t {
        return FakeBackend::vm_unmap(region, address, size);
    }

    [[nodiscard]] static auto vm_destroy_region(
        myos::cap::CapRef region) noexcept -> myos_status_t {
        return FakeBackend::vm_destroy_region(region);
    }
};

using Space = myos::deploy::TaskSpace<8, 8, FakeBackend>;
using Record = myos::deploy::TaskRecord<Space>;
using Completions = myos::deploy::CompletionSet<2, 3>;
using Table = myos::deploy::TaskTable<Record, Completions, 2, 3>;
using Builder = myos::deploy::TaskBuilder<Table, Completions>;

using ConstructionSpace = myos::deploy::TaskSpace<32, 8, FakeBackend>;
using ConstructionRecord = myos::deploy::TaskRecord<ConstructionSpace>;
using ConstructionCompletions = myos::deploy::CompletionSet<1, 3>;
using ConstructionTable = myos::deploy::TaskTable<
    ConstructionRecord, ConstructionCompletions, 1, 3>;
using ConstructionBuilder = myos::deploy::TaskBuilder<
    ConstructionTable, ConstructionCompletions>;
using ConstructionBundle = myos::deploy::MappedBundle<FakeBackend>;
using ConstructionScratch = myos::deploy::ScratchWindow<FakeBackend>;
using ConstructionAuthorities = myos::deploy::AuthoritySet<2, 2>;
using ConstructionWorkspace = myos::deploy::TaskConstructionWorkspace<
    ConstructionAuthorities>;

static_assert(myos::deploy::Backend<FakeBackend>);
static_assert(myos::deploy::ConstructionBackend<FakeBackend>);
static_assert(myos::deploy::Backend<ReturningFaultBackend>);
static_assert(!libk::is_copy_constructible_v<Record>);
static_assert(!libk::is_copy_constructible_v<Table>);

struct Fixture final {
    uint8_t raw[myos::deploy::host::kGoldenSize]{};
    myos::deploy::ManifestWorkspace workspace{};
    myos::deploy::PlanSet<1> plans{};
    myos::deploy::DeploymentPlan plan{};
};

struct DependencyFixture final {
    uint8_t raw[1400]{};
    myos::deploy::ManifestWorkspace workspace{};
    myos::deploy::PlanSet<1> plans{};
    myos::deploy::DeploymentPlan plan{};
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
    constexpr size_t modules_offset = MYOS_BOOT_HEADER_SIZE;
    constexpr size_t segments_offset =
        modules_offset + MYOS_BOOT_MODULE_SIZE;
    constexpr size_t name_offset =
        segments_offset + MYOS_BOOT_SEGMENT_SIZE;
    constexpr size_t image_offset = name_offset + 4;
    constexpr size_t image_size = 0x1000;
    constexpr size_t total_size = image_offset + image_size;
    for (size_t index = 0; index < total_size; ++index) {
        construction_bundle[index] = 0;
    }
    put_bundle(0, MYOS_BOOT_MAGIC, 8);
    put_bundle(8, MYOS_BOOT_MAJOR, 2);
    put_bundle(10, MYOS_BOOT_MINOR, 2);
    put_bundle(12, MYOS_BOOT_HEADER_SIZE, 4);
    put_bundle(16, total_size, 8);
    put_bundle(24, MYOS_BOOT_ARCH_RISCV64, 4);
    put_bundle(28, MYOS_BOOT_ABI_RISCV_LP64, 4);
    put_bundle(40, modules_offset, 8);
    put_bundle(48, 1, 4);
    put_bundle(56, segments_offset, 8);
    put_bundle(64, 1, 4);
    put_bundle(modules_offset, name_offset, 8);
    put_bundle(modules_offset + 8, 4, 4);
    put_bundle(modules_offset + 12, MYOS_BOOT_MODULE_BOOTABLE, 4);
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
        MYOS_BOOT_SEGMENT_READ | MYOS_BOOT_SEGMENT_EXECUTE, 4);
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
    const myos::cap::CapRef root{1, 0};
    const myos_word_t bundle_address = static_cast<myos_word_t>(
        reinterpret_cast<uintptr_t>(construction_bundle));
    const myos_word_t scratch_address = static_cast<myos_word_t>(
        reinterpret_cast<uintptr_t>(construction_scratch));
    if (bundle.open(
            root,
            myos::cap::CapRef{2, 0},
            myos::deploy::Window{bundle_address, 8192},
            bundle_size)
        != MYOS_STATUS_OK
        || scratch.open(
               root,
               myos::deploy::Window{scratch_address, 16384})
            != MYOS_STATUS_OK) {
        return false;
    }
    myos::deploy::TaskAuthorityBindings bindings{};
    myos::deploy::TaskConstructionInput<
        FakeBackend, ConstructionAuthorities> input{
        .parent_pool = root,
        .bundle = &bundle,
        .scratch = &scratch,
        .bootstrap = nullptr,
        .bootstrap_size = 0,
        .bindings = &bindings,
        .workspace = construction_workspace,
    };
    return builder.construct(input, authorities) == MYOS_STATUS_OK
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
        fixture.raw[index] = myos::deploy::host::kGolden[index];
    }
    auto parsed = myos::deploy::ManifestView::parse(
        fixture.raw,
        sizeof(fixture.raw),
        fixture.workspace);
    if (!parsed) {
        return false;
    }
    auto decoded = myos::deploy::DeploymentPlan::decode(
        parsed.value(), fixture.plans);
    if (!decoded) {
        return false;
    }
    fixture.plan = libk::move(decoded.value());
    for (uint8_t& byte : fixture.raw) {
        byte = 0;
    }
    return fixture.plan.id() == myos::deploy::PlanId{0, 1}
        && fixture.plan.task_count() == 1
        && fixture.plan.mapping_count() == 3;
}

[[nodiscard]] auto make_dependency_plan(
    DependencyFixture& fixture) noexcept -> bool {
    constexpr size_t shift = MYOS_DEPLOY_TASK_STRIDE;
    constexpr size_t dependency_offset = 0x500;
    const size_t size = dependency_offset + 2 * MYOS_DEPLOY_DEPENDENCY_STRIDE;
    for (size_t index = 0; index < size; ++index) {
        fixture.raw[index] = 0;
    }
    for (size_t index = 0; index < myos::deploy::host::kGoldenSize; ++index) {
        fixture.raw[index] = myos::deploy::host::kGolden[index];
    }
    for (size_t index = myos::deploy::host::kGoldenSize; index > 0x170;
         --index) {
        fixture.raw[index - 1 + shift]
            = myos::deploy::host::kGolden[index - 1];
    }
    for (size_t index = 0; index < MYOS_DEPLOY_TASK_STRIDE; ++index) {
        fixture.raw[0xd0 + shift + index] = fixture.raw[0xd0 + index];
    }

    put_manifest(
        fixture.raw, MYOS_DEPLOY_HEADER_TOTAL_SIZE, size, 8);
    put_manifest(
        fixture.raw,
        MYOS_DEPLOY_HEADER_TABLES
            + MYOS_DEPLOY_TABLE_TASK * MYOS_DEPLOY_TABLE_DESC_SIZE
            + MYOS_DEPLOY_TABLE_COUNT_FIELD,
        2,
        4);
    const uint64_t old_offsets[MYOS_DEPLOY_TABLE_COUNT] = {
        0xd0, 0x170, 0x190, 0x280, 0x2e0, 0x350, 0, 0x3b0, 0x410,
    };
    for (uint32_t table = MYOS_DEPLOY_TABLE_IMAGE;
         table < MYOS_DEPLOY_TABLE_COUNT;
         ++table) {
        const size_t descriptor = MYOS_DEPLOY_HEADER_TABLES
            + table * MYOS_DEPLOY_TABLE_DESC_SIZE;
        put_manifest(
            fixture.raw,
            descriptor + MYOS_DEPLOY_TABLE_OFFSET,
            old_offsets[table] + shift,
            8);
    }
    const size_t dependency_descriptor = MYOS_DEPLOY_HEADER_TABLES
        + MYOS_DEPLOY_TABLE_DEPENDENCY * MYOS_DEPLOY_TABLE_DESC_SIZE;
    put_manifest(
        fixture.raw,
        dependency_descriptor + MYOS_DEPLOY_TABLE_OFFSET,
        dependency_offset,
        8);
    put_manifest(
        fixture.raw,
        dependency_descriptor + MYOS_DEPLOY_TABLE_COUNT_FIELD,
        2,
        4);

    const size_t first_task = 0xd0;
    const size_t second_task = first_task + MYOS_DEPLOY_TASK_STRIDE;
    put_manifest(
        fixture.raw,
        first_task + MYOS_DEPLOY_TASK_DEPENDENCY_COUNT,
        1,
        4);
    const uint32_t first_fields[7] = {
        MYOS_DEPLOY_TASK_IMAGE_FIRST,
        MYOS_DEPLOY_TASK_MAPPING_FIRST,
        MYOS_DEPLOY_TASK_OBJECT_FIRST,
        MYOS_DEPLOY_TASK_EXECUTION_FIRST,
        MYOS_DEPLOY_TASK_IMPORT_FIRST,
        MYOS_DEPLOY_TASK_DEPENDENCY_FIRST,
        MYOS_DEPLOY_TASK_EXPORT_FIRST,
    };
    const uint32_t count_fields[7] = {
        MYOS_DEPLOY_TASK_IMAGE_COUNT,
        MYOS_DEPLOY_TASK_MAPPING_COUNT,
        MYOS_DEPLOY_TASK_OBJECT_COUNT,
        MYOS_DEPLOY_TASK_EXECUTION_COUNT,
        MYOS_DEPLOY_TASK_IMPORT_COUNT,
        MYOS_DEPLOY_TASK_DEPENDENCY_COUNT,
        MYOS_DEPLOY_TASK_EXPORT_COUNT,
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
        second_task + MYOS_DEPLOY_TASK_BOOTSTRAP_MAPPING,
        MYOS_DEPLOY_NO_INDEX,
        4);

    const size_t dependency = dependency_offset;
    put_manifest(
        fixture.raw,
        dependency + MYOS_DEPLOY_DEPENDENCY_TARGET,
        1,
        4);
    put_manifest(
        fixture.raw,
        dependency + MYOS_DEPLOY_DEPENDENCY_KIND,
        MYOS_DEPLOY_DEPENDENCY_REQUIRED,
        2);
    put_manifest(
        fixture.raw,
        dependency + MYOS_DEPLOY_DEPENDENCY_FLAGS,
        MYOS_DEPLOY_DEPENDENCY_STARTUP,
        2);
    put_manifest(
        fixture.raw,
        dependency + MYOS_DEPLOY_DEPENDENCY_STRIDE
            + MYOS_DEPLOY_DEPENDENCY_TARGET,
        0,
        4);
    put_manifest(
        fixture.raw,
        dependency + MYOS_DEPLOY_DEPENDENCY_STRIDE
            + MYOS_DEPLOY_DEPENDENCY_KIND,
        MYOS_DEPLOY_DEPENDENCY_OPTIONAL,
        2);
    put_manifest(
        fixture.raw,
        dependency + MYOS_DEPLOY_DEPENDENCY_STRIDE
            + MYOS_DEPLOY_DEPENDENCY_FLAGS,
        MYOS_DEPLOY_DEPENDENCY_STARTUP,
        2);

    auto parsed = myos::deploy::ManifestView::parse(
        fixture.raw, size, fixture.workspace);
    if (!parsed) {
        return false;
    }
    auto decoded = myos::deploy::DeploymentPlan::decode(
        parsed.value(), fixture.plans);
    if (!decoded) {
        return false;
    }
    fixture.plan = libk::move(decoded.value());
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
        && first->kind == MYOS_DEPLOY_DEPENDENCY_REQUIRED
        && first->flags == MYOS_DEPLOY_DEPENDENCY_STARTUP
        && first->relation.empty()
        && second->target == 0
        && second->kind == MYOS_DEPLOY_DEPENDENCY_OPTIONAL
        && second->flags == MYOS_DEPLOY_DEPENDENCY_STARTUP
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
    const auto equals = [](myos::deploy::ByteView bytes,
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
    const auto zero_attenuation = [](const myos_cap_attenuation& value) noexcept {
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
        || task->kind_mask != MYOS_RESOURCE_E2_KINDS
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
        || task->readiness_value != 0
        || !equals(fixture.plan.symbol(image->source), "init")
        || image->source_kind != 0 || image->flags != 0
        || !equals(fixture.plan.symbol(code->produced), "code")
        || !code->pager.empty() || code->image != 0 || code->segment != 0
        || code->source != MYOS_DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
        || code->residency != MYOS_DEPLOY_MAPPING_RESIDENT
        || code->critical != MYOS_DEPLOY_CRITICAL_CODE
        || code->flags != 0 || code->access != 0
        || code->address != 0 || code->size != 0
        || !equals(fixture.plan.symbol(stack->produced), "stack")
        || !stack->pager.empty()
        || stack->image != MYOS_DEPLOY_NO_INDEX
        || stack->segment != MYOS_DEPLOY_NO_INDEX
        || stack->source != MYOS_DEPLOY_MAPPING_SOURCE_ZERO
        || stack->residency != MYOS_DEPLOY_MAPPING_RESIDENT
        || stack->critical != MYOS_DEPLOY_CRITICAL_STACK
        || stack->flags != 0
        || stack->access != (MYOS_VM_READ | MYOS_VM_WRITE)
        || stack->address != 0x210000 || stack->size != 4096
        || !equals(fixture.plan.symbol(bootstrap->produced), "bootstrap")
        || !bootstrap->pager.empty()
        || bootstrap->image != MYOS_DEPLOY_NO_INDEX
        || bootstrap->segment != MYOS_DEPLOY_NO_INDEX
        || bootstrap->source != MYOS_DEPLOY_MAPPING_SOURCE_ZERO
        || bootstrap->residency != MYOS_DEPLOY_MAPPING_RESIDENT
        || bootstrap->critical != MYOS_DEPLOY_CRITICAL_BOOTSTRAP
        || bootstrap->flags != 0 || bootstrap->access != MYOS_VM_READ
        || bootstrap->address != 0x220000 || bootstrap->size != 4096
        || !equals(fixture.plan.symbol(object->output), "notify")
        || !object->output_b.empty() || object->flags != 0
        || object->kind != MYOS_OBJECT_KIND_NOTIFICATION
        || object->args[0] != 1
        || object->refs[0] != MYOS_DEPLOY_NO_INDEX
        || object->refs[1] != MYOS_DEPLOY_NO_INDEX
        || object->refs[2] != MYOS_DEPLOY_NO_INDEX
        || object->refs[3] != MYOS_DEPLOY_NO_INDEX
        || object->args[1] != 0 || object->args[2] != 0
        || object->args[3] != 0 || object->args[4] != 0
        || object->args[5] != 0
        || !equals(fixture.plan.symbol(execution->key), "thread")
        || !equals(fixture.plan.symbol(execution->sc), "sc")
        || !equals(fixture.plan.symbol(execution->domain), "domain")
        || execution->image != 0 || execution->stack != 1
        || execution->bootstrap != 2
        || execution->ipc != MYOS_DEPLOY_NO_INDEX
        || execution->control != MYOS_DEPLOY_NO_INDEX
        || execution->event != MYOS_DEPLOY_NO_INDEX
        || execution->model != 0 || execution->flags != 0
        || execution->fault != 0 || execution->terminal != 0
        || execution->entry != 0x200000
        || execution->stack_top != 0x211000
        || execution->sc_budget != 1 || execution->sc_period != 1
        || execution->urgency != 0
        || execution->home_cpu != MYOS_DEPLOY_HOME_CPU_ANY
        || !equals(fixture.plan.symbol(import->source), "authority")
        || !equals(fixture.plan.symbol(import->destination), "import")
        || import->mode != MYOS_DEPLOY_IMPORT_DUPLICATE
        || import->selector != MYOS_DEPLOY_SELECTOR_ALLOCATED_KEYED
        || import->flags != 0
        || import->attenuation.version
            != MYOS_DEPLOY_ATTENUATION_VERSION_CURRENT
        || import->attenuation.kind != MYOS_OBJECT_KIND_THREAD
        || import->attenuation.size != MYOS_DEPLOY_ATTENUATION_STRIDE
        || !zero_attenuation(import->attenuation)
        || !equals(fixture.plan.symbol(output->source), "thread")
        || !equals(fixture.plan.symbol(output->key), "export")
        || output->source_class != MYOS_DEPLOY_EXPORT_PREPARED_KEY
        || output->flags != 0
        || output->ceiling.version
            != MYOS_DEPLOY_ATTENUATION_VERSION_CURRENT
        || output->ceiling.kind != MYOS_OBJECT_KIND_THREAD
        || output->ceiling.size != MYOS_DEPLOY_ATTENUATION_STRIDE
        || !zero_attenuation(output->ceiling)) {
        return false;
    }
    const auto name = lease->task(0).symbol(lease->task(0).row()->name);
    return name.size() == 4
        && name[0] == 'i' && name[1] == 'n'
        && name[2] == 'i' && name[3] == 't';
}

[[nodiscard]] auto test_plan_registry_lifetime() noexcept -> bool {
    using Plans = myos::deploy::PlanSet<1>;
    static Plans plans{};
    uint8_t raw[myos::deploy::host::kGoldenSize]{};
    myos::deploy::ManifestWorkspace workspace{};
    myos::deploy::TaskPlanView view{};
    {
        for (size_t index = 0; index < sizeof(raw); ++index) {
            raw[index] = myos::deploy::host::kGolden[index];
        }
        auto parsed = myos::deploy::ManifestView::parse(
            raw, sizeof(raw), workspace);
        if (!parsed) {
            return false;
        }
        auto decoded = myos::deploy::DeploymentPlan::decode(parsed.value(), plans);
        if (!decoded) {
            return false;
        }
        myos::deploy::DeploymentPlan owner = libk::move(decoded.value());
        auto lease = owner.lease();
        if (!lease) {
            return false;
        }
        view = lease->task(0);
        if (!view.valid() || view.id.plan != myos::deploy::PlanId{0, 1}) {
            return false;
        }
        myos::deploy::DeploymentPlan moved = libk::move(owner);
        if (!view.valid() || myos::deploy::DeploymentPlan::decode(
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
        raw[index] = myos::deploy::host::kGolden[index];
    }
    auto parsed = myos::deploy::ManifestView::parse(
        raw, sizeof(raw), workspace);
    if (!parsed) {
        return false;
    }
    auto decoded = myos::deploy::DeploymentPlan::decode(parsed.value(), plans);
    return decoded && decoded.value().id() == myos::deploy::PlanId{0, 2};
}

[[nodiscard]] auto test_plan_generation_exhaustion() noexcept -> bool {
    using Plans = myos::deploy::PlanSet<1, 3>;
    static uint8_t raw[myos::deploy::host::kGoldenSize]{};
    static myos::deploy::ManifestWorkspace workspace{};
    Plans plans{};
    for (uint32_t expected = 1; expected <= 3; ++expected) {
        for (size_t index = 0; index < sizeof(raw); ++index) {
            raw[index] = myos::deploy::host::kGolden[index];
        }
        auto parsed = myos::deploy::ManifestView::parse(
            raw, sizeof(raw), workspace);
        if (!parsed) {
            return false;
        }
        {
            auto decoded = myos::deploy::DeploymentPlan::decode(
                parsed.value(), plans);
            if (!decoded
                || decoded.value().id()
                    != myos::deploy::PlanId{0, expected}) {
                return false;
            }
            auto owner = libk::move(decoded.value());
            for (uint8_t& byte : raw) {
                byte = 0;
            }
        }
    }
    auto parsed = myos::deploy::ManifestView::parse(
        myos::deploy::host::kGolden,
        myos::deploy::host::kGoldenSize,
        workspace);
    return parsed
        && !myos::deploy::DeploymentPlan::decode(parsed.value(), plans);
}

[[nodiscard]] auto test_completion_lifecycle() noexcept -> bool {
    using Set = myos::deploy::CompletionSet<1, 3>;

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
            != myos::deploy::CompletionCellState::Reserved
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
        || sender.complete({myos::deploy::TaskId{1, 1},
                            myos::deploy::CloseReason::Explicit,
                            MYOS_STATUS_OK})
        || set.available() != 1
        || set.cell_state(first_id)
            != myos::deploy::CompletionCellState::Retired) {
        return false;
    }

    auto second = set.reserve();
    if (!second) {
        return false;
    }
    auto second_sender = second->take_sender();
    auto second_receiver = second->take_receiver();
    const myos::deploy::TaskId task{2, 1};
    if (!second_sender.complete({
            task, myos::deploy::CloseReason::Terminal, MYOS_STATUS_BUSY})) {
        return false;
    }
    auto result = second_receiver.take();
    if (!result || result->task != task
        || result->status != MYOS_STATUS_BUSY || set.available() != 1) {
        return false;
    }

    auto third = set.reserve();
    if (!third) {
        return false;
    }
    auto third_sender = third->take_sender();
    auto third_receiver = third->take_receiver();
    if (!third_sender.complete({task, myos::deploy::CloseReason::Explicit,
                                MYOS_STATUS_OK})) {
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
                myos::deploy::TaskId{3, 1},
                myos::deploy::CloseReason::Explicit,
                MYOS_STATUS_OK})) {
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
            myos::deploy::TaskId{4, 1},
            myos::deploy::CloseReason::Explicit,
            MYOS_STATUS_OK})
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
    myos::deploy::TaskId first_id{};
    {
        auto lease = fixture.plan.lease();
        if (!lease) {
            return false;
        }
        auto builder = Builder::begin(
            completions, table, libk::move(*lease), 0);
        if (!builder || builder->record() == nullptr) {
            return false;
        }
        first_id = builder->record()->id();
    }
    if (table.tag(first_id) != myos::deploy::TaskSlotTag::Vacant
        || completions.available() != completions.capacity()) {
        return false;
    }
    {
        auto lease = fixture.plan.lease();
        if (!lease) {
            return false;
        }
        auto builder = Builder::begin(
            completions, table, libk::move(*lease), 0);
        if (!builder || builder->record() == nullptr
            || builder->record()->id() != first_id) {
            return false;
        }
    }
    if (table.tag(first_id) != myos::deploy::TaskSlotTag::Vacant
        || completions.available() != completions.capacity()) {
        return false;
    }

    using SmallTable = myos::deploy::TaskTable<Record, Completions, 1, 3>;
    using SmallBuilder = myos::deploy::TaskBuilder<SmallTable, Completions>;
    Completions pressure{};
    SmallTable small_table{};
    auto lease = fixture.plan.lease();
    if (!lease) {
        return false;
    }
    {
        auto first = SmallBuilder::begin(
            pressure, small_table, libk::move(*lease), 0);
        if (!first || pressure.available() != 1) {
            return false;
        }
        const auto id = first->record()->id();
        auto second_lease = fixture.plan.lease();
        if (!second_lease
            || SmallBuilder::begin(
                   pressure, small_table, libk::move(*second_lease), 0)
            || small_table.tag(id) != myos::deploy::TaskSlotTag::Reserved
            || pressure.available() != 1) {
            return false;
        }
    }
    return small_table.tag(myos::deploy::TaskId{0, 1})
            == myos::deploy::TaskSlotTag::Vacant
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
        completions, table, libk::move(*lease), 0);
    if (!builder || builder->record() == nullptr) {
        return false;
    }
    const auto id = builder->record()->id();
    auto receiver = builder->take_receiver();
    if (!receiver
        || builder->cancel()
        || !builder->valid()
        || table.tag(id) != myos::deploy::TaskSlotTag::Reserved
        || completions.available() != completions.capacity() - 1) {
        return false;
    }
    if (!receiver->detach()
        || !builder->cancel()
        || builder->valid()
        || table.tag(id) != myos::deploy::TaskSlotTag::Vacant
        || completions.available() != completions.capacity()) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_resourceful_destructor_fail_stop() noexcept -> bool {
    using FaultSpace = myos::deploy::TaskSpace<8, 8, ReturningFaultBackend>;

    ReturningFaultBackend::reset();
    {
        FaultSpace space{};
        if (space.open(
                myos::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
                != MYOS_STATUS_OK) {
            return false;
        }
    }
    return ReturningFaultBackend::faults != 0;
}

[[nodiscard]] auto test_checked_task_projections() noexcept -> bool {
    FakeBackend::reset();
    Space space{};
    if (space.open(
            myos::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != MYOS_STATUS_OK) {
        return false;
    }
    const auto local = space.vspace_slot();
    const auto local_ref = space.lookup(local, MYOS_OBJECT_KIND_VSPACE);
    if (!local_ref || local_ref->selector != 11 || local_ref->cspace != 0) {
        return false;
    }
    if (space.lookup(local, MYOS_OBJECT_KIND_THREAD)) {
        return false;
    }
    const auto manager_ref = space.lookup(
        space.manager_slot(), MYOS_OBJECT_KIND_CSPACE);
    if (!manager_ref) {
        return false;
    }
    auto remote_owner = Space::owner_type{
        myos::cap::CapRef{99, manager_ref->selector}};
    const auto remote_index = space.adopt_remote_index(
        libk::move(remote_owner));
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
    return space.close() == MYOS_STATUS_OK;
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
        completions, table, libk::move(*lease), 1);
    if (!builder) {
        return false;
    }
    ConstructionAuthorities authorities{};
    if (!prepare_empty_task(*builder, authorities)) {
        return false;
    }
    auto receiver = builder->take_receiver();
    if (!receiver || builder->record() == nullptr
        || builder->record()->state() != myos::deploy::TaskState::Constructing) {
        return false;
    }
    const myos::deploy::TaskId id = builder->record()->id();
    if (table.transition(id, myos::deploy::TaskState::Prepared)
        || table.transition(id, myos::deploy::TaskState::Starting)
        || table.transition(id, myos::deploy::TaskState::Running)
        || table.transition(id, myos::deploy::TaskState::Closing)
        || table.transition(id, myos::deploy::TaskState::Reclaimed)
        || builder->record()->state()
            != myos::deploy::TaskState::Constructing
        || !builder->commit_prepared()
        || table.tag(id) != myos::deploy::TaskSlotTag::Record
        || table.transition(myos::deploy::TaskId{id.slot, id.generation + 1},
                            myos::deploy::TaskState::Starting)
        || table.transition(id, myos::deploy::TaskState::Constructing)
        || table.transition(id, myos::deploy::TaskState::Prepared)
        || table.transition(id, myos::deploy::TaskState::Running)
        || table.transition(id, myos::deploy::TaskState::Reclaimed)
        || table.record(id)->state() != myos::deploy::TaskState::Prepared
        || !table.transition(id, myos::deploy::TaskState::Starting)
        || table.transition(id, myos::deploy::TaskState::Constructing)
        || table.transition(id, myos::deploy::TaskState::Prepared)
        || table.transition(id, myos::deploy::TaskState::Starting)
        || table.transition(id, myos::deploy::TaskState::Reclaimed)
        || table.record(id)->state() != myos::deploy::TaskState::Starting
        || !table.transition(id, myos::deploy::TaskState::Running)
        || table.transition(id, myos::deploy::TaskState::Constructing)
        || table.transition(id, myos::deploy::TaskState::Prepared)
        || table.transition(id, myos::deploy::TaskState::Starting)
        || table.transition(id, myos::deploy::TaskState::Running)
        || table.transition(id, myos::deploy::TaskState::Reclaimed)
        || table.record(id)->state() != myos::deploy::TaskState::Running
        || !table.transition(id, myos::deploy::TaskState::Failed)
        || table.transition(id, myos::deploy::TaskState::Constructing)
        || table.transition(id, myos::deploy::TaskState::Prepared)
        || table.transition(id, myos::deploy::TaskState::Starting)
        || table.transition(id, myos::deploy::TaskState::Running)
        || table.transition(id, myos::deploy::TaskState::Failed)
        || table.transition(id, myos::deploy::TaskState::Reclaimed)
        || table.record(id)->state() != myos::deploy::TaskState::Failed
        || !table.begin_close(id, myos::deploy::CloseReason::Terminal,
                              MYOS_STATUS_CANCELED)
        || table.tag(id) != myos::deploy::TaskSlotTag::Closing
        || table.closing(id) == nullptr
        || table.transition(id, myos::deploy::TaskState::Running)
        || table.continue_close(id) != MYOS_STATUS_OK) {
        return false;
    }
    auto result = receiver->take();
    if (!result || result->task != id
        || result->reason != myos::deploy::CloseReason::Terminal
        || result->status != MYOS_STATUS_CANCELED
        || table.tag(id) != myos::deploy::TaskSlotTag::Retired) {
        return false;
    }
    return table.record(id) == nullptr
        && table.closing(id) == nullptr
        && !table.transition(id, myos::deploy::TaskState::Running)
        && table.tag(myos::deploy::TaskId{id.slot, id.generation + 1})
            == myos::deploy::TaskSlotTag::Vacant;
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
    if (!lease || Builder::begin(completions, table, libk::move(*lease), 0)) {
        return false;
    }
    return table.tag(myos::deploy::TaskId{0, 1})
        == myos::deploy::TaskSlotTag::Vacant;
}

[[nodiscard]] auto test_resource_failure_moves_to_closing() noexcept -> bool {
    FakeBackend::reset();
    Space space{};
    if (space.open(
            myos::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
            != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::reset();
    FakeBackend::next_resource_close = MYOS_STATUS_BUSY;
    if (space.close() != MYOS_STATUS_BUSY
        || space.phase() != myos::deploy::Phase::ResourceClosing
        || space.close() != MYOS_STATUS_OK
        || space.phase() != myos::deploy::Phase::Closed) {
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
    FakeBackend::next_vspace_status = MYOS_STATUS_NO_MEMORY;
    /* TaskSpace::open consumes the first BUSY while unwinding the partial
     * aggregate; the table-owned close consumes the second and succeeds on
     * its retry. */
    FakeBackend::resource_close_busy_count = 2;

    const size_t bundle_size = make_construction_bundle();
    ConstructionBundle bundle{};
    ConstructionScratch scratch{};
    const myos::cap::CapRef root{1, 0};
    const myos_word_t bundle_address = static_cast<myos_word_t>(
        reinterpret_cast<uintptr_t>(construction_bundle));
    const myos_word_t scratch_address = static_cast<myos_word_t>(
        reinterpret_cast<uintptr_t>(construction_scratch));
    if (bundle.open(
            root,
            myos::cap::CapRef{2, 0},
            myos::deploy::Window{bundle_address, 8192},
            bundle_size)
            != MYOS_STATUS_OK
        || scratch.open(
               root,
               myos::deploy::Window{scratch_address, 16384})
            != MYOS_STATUS_OK) {
        return false;
    }

    myos::deploy::TaskAuthorityBindings bindings{};
    myos::deploy::TaskConstructionInput<
        FakeBackend, ConstructionAuthorities> input{
        .parent_pool = root,
        .bundle = &bundle,
        .scratch = &scratch,
        .bootstrap = nullptr,
        .bootstrap_size = 0,
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
        completions, table, libk::move(*plan), 1);
    if (!builder || builder->record() == nullptr) {
        return false;
    }
    const myos::deploy::TaskId id = builder->record()->id();
    auto receiver = builder->take_receiver();
    if (!receiver) {
        return false;
    }
    const myos_status_t status = builder->construct(input, authorities);
    if (status != MYOS_STATUS_NO_MEMORY
        || builder->valid()
        || !construction_workspace.empty()
        || table.tag(id) != myos::deploy::TaskSlotTag::Closing) {
        return false;
    }
    const myos_status_t first_close = table.continue_close(id);
    const auto first_tag = table.tag(id);
    const myos_status_t second_close = table.continue_close(id);
    if (first_close != MYOS_STATUS_BUSY
        || first_tag != myos::deploy::TaskSlotTag::Closing
        || second_close != MYOS_STATUS_OK
        || table.tag(myos::deploy::TaskId{
                         id.slot, id.generation + 1})
            != myos::deploy::TaskSlotTag::Vacant) {
        return false;
    }
    const auto result = receiver->take();
    return result && result->task == id
        && result->reason == myos::deploy::CloseReason::ConstructionFailure
        && result->status == MYOS_STATUS_NO_MEMORY;
}

[[nodiscard]] auto test_task_generation_exhaustion() noexcept -> bool {
    static DependencyFixture fixture{};
    if (!make_dependency_plan(fixture)) {
        return false;
    }
    using SmallCompletions = myos::deploy::CompletionSet<1, 3>;
    using SmallTable = myos::deploy::TaskTable<Record, SmallCompletions, 1, 3>;
    using SmallBuilder = myos::deploy::TaskBuilder<SmallTable, SmallCompletions>;
    SmallCompletions completions{};
    SmallTable table{};
    for (uint32_t expected = 1; expected <= 3; ++expected) {
        auto lease = fixture.plan.lease();
        if (!lease) {
            return false;
        }
        auto builder = SmallBuilder::begin(
            completions, table, libk::move(*lease), 1);
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
        const myos::deploy::TaskId id = builder->record()->id();
        if (!builder->commit_prepared()
            || !table.begin_close(id, myos::deploy::CloseReason::Explicit,
                                  MYOS_STATUS_OK)
            || table.continue_close(id) != MYOS_STATUS_OK) {
            return false;
        }
        auto result = receiver->take();
        if (!result || result->task != id) {
            return false;
        }
        const auto next = myos::deploy::TaskId{0, expected + 1};
        if (expected < 3) {
            if (table.tag(next) != myos::deploy::TaskSlotTag::Vacant) {
                return false;
            }
        } else if (table.tag(next) != myos::deploy::TaskSlotTag::Retired) {
            return false;
        }
    }
    auto lease = fixture.plan.lease();
    return lease && !SmallBuilder::begin(
        completions, table, libk::move(*lease), 0);
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
            myos::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != MYOS_STATUS_OK) {
        return false;
    }
    const auto domain_slot = source_space.adopt_local(
        ConstructionSpace::owner_type{myos::cap::CapRef{91, 0}},
        MYOS_OBJECT_KIND_SCHED_DOMAIN);
    const auto import_slot = source_space.adopt_local(
        ConstructionSpace::owner_type{myos::cap::CapRef{92, 0}},
        MYOS_OBJECT_KIND_THREAD);
    if (!domain_slot || !import_slot) {
        return false;
    }
    myos::deploy::RegisteredSpace<ConstructionSpace, 2> source{};
    if (!source.adopt(libk::move(source_space))) {
        return false;
    }
    ConstructionAuthorities authorities{};
    const myos_cap_attenuation domain_ceiling{
        .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
        .kind = MYOS_OBJECT_KIND_SCHED_DOMAIN,
        .size = MYOS_CAP_ATTENUATION_SIZE,
        .rights = MYOS_RIGHT_MASK,
        .words = {},
    };
    const myos_cap_attenuation thread_ceiling{
        .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
        .kind = MYOS_OBJECT_KIND_THREAD,
        .size = MYOS_CAP_ATTENUATION_SIZE,
        .rights = MYOS_RIGHT_MASK,
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
    const myos::cap::CapRef root{1, 0};
    const myos_word_t bundle_address = static_cast<myos_word_t>(
        reinterpret_cast<uintptr_t>(construction_bundle));
    const myos_word_t scratch_address = static_cast<myos_word_t>(
        reinterpret_cast<uintptr_t>(construction_scratch));
    if (bundle.open(
            root,
            myos::cap::CapRef{2, 0},
            myos::deploy::Window{bundle_address, 8192},
            bundle_size)
        != MYOS_STATUS_OK
        || scratch.open(
               root,
               myos::deploy::Window{scratch_address, 16384})
            != MYOS_STATUS_OK
        || bundle.phase() != myos::deploy::LeasePhase::Mapped
        || scratch.phase() != myos::deploy::LeasePhase::Ready) {
        return false;
    }
    myos::deploy::TaskAuthorityBindings bindings{};
    bindings.domains[0] = *domain;
    bindings.imports[0] = *import;
    myos::deploy::TaskConstructionInput<
        FakeBackend, ConstructionAuthorities> input{
        .parent_pool = root,
        .bundle = &bundle,
        .scratch = &scratch,
        .bootstrap = "boot",
        .bootstrap_size = 4,
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
        completions, table, libk::move(*plan), 0);
    const myos_status_t construction_status = builder
        ? builder->construct(input, authorities) : MYOS_STATUS_BAD_ARGS;
    if (!builder || construction_status != MYOS_STATUS_OK) {
        return false;
    }
    const auto projections = builder->record()->projections();
    if (builder->record()->state() != myos::deploy::TaskState::Constructing
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
        || projections.exports[0].kind != MYOS_OBJECT_KIND_THREAD
        || builder->record()->accounting().total_bytes != 12288
        || builder->record()->accounting().by_class[
               MYOS_DEPLOY_CRITICAL_CODE] != 4096
        || builder->record()->accounting().by_class[
               MYOS_DEPLOY_CRITICAL_STACK] != 4096
        || builder->record()->accounting().by_class[
               MYOS_DEPLOY_CRITICAL_BOOTSTRAP] != 4096) {
        return false;
    }
    if (!construction_workspace.empty()) {
        return false;
    }
    const myos::deploy::TaskId task = builder->record()->id();
    auto* const record_before_commit = builder->record();
    if (!builder->commit_prepared()
        || builder->valid()
        || table.record(task) != record_before_commit
        || table.record(task)->state() != myos::deploy::TaskState::Prepared) {
        return false;
    }
    auto receiver = builder->take_receiver();
    if (!receiver
        || !table.begin_close(
            task, myos::deploy::CloseReason::ConstructionFailure,
            MYOS_STATUS_OK)
        || table.continue_close(task) != MYOS_STATUS_OK) {
        return false;
    }
    const auto result = receiver->take();
    if (!result || result->task != task
        || result->reason
            != myos::deploy::CloseReason::ConstructionFailure
        || result->status != MYOS_STATUS_OK) {
        return false;
    }

    auto plan_after_success = fixture.plan.lease();
    if (!plan_after_success) {
        return false;
    }
    auto failed_builder = ConstructionBuilder::begin(
        completions, table, libk::move(*plan_after_success), 0);
    if (!failed_builder || failed_builder->record() == nullptr) {
        return false;
    }
    const myos::deploy::TaskId failed_task = failed_builder->record()->id();
    auto invalid_input = input;
    invalid_input.bootstrap_size = 8192;
    const myos_status_t failed_status = failed_builder->construct(
        invalid_input, authorities);
    if (failed_status != MYOS_STATUS_BAD_ARGS
        || failed_builder->valid()
        || !construction_workspace.empty()
        || table.tag(failed_task) != myos::deploy::TaskSlotTag::Closing) {
        return false;
    }
    FakeBackend::next_resource_close = MYOS_STATUS_BUSY;
    if (table.continue_close(failed_task) != MYOS_STATUS_BUSY
        || table.tag(failed_task) != myos::deploy::TaskSlotTag::Closing
        || !construction_workspace.empty()
        || table.continue_close(failed_task) != MYOS_STATUS_OK) {
        return false;
    }
    auto failed_receiver = failed_builder->take_receiver();
    if (!failed_receiver) {
        return false;
    }
    const auto failed_result = failed_receiver->take();
    if (!failed_result || failed_result->task != failed_task
        || failed_result->reason
            != myos::deploy::CloseReason::ConstructionFailure
        || failed_result->status != MYOS_STATUS_BAD_ARGS) {
        return false;
    }

    auto held = authorities.lease(*domain);
    if (!held || authorities.live_leases() != 1
        || source.close() != MYOS_STATUS_BUSY) {
        /* A source close starts retirement but cannot bypass the reciprocal
         * registration while this independent lease pins the entry. */
        return false;
    }
    held.reset();
    return source.close() == MYOS_STATUS_OK
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
