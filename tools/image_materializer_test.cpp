#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <initializer_list>

#include <libk/assert.hpp>
#include <libk/utility.hpp>
#include <uapi/resource.h>
#include <user/lib/deployment_syscall.hpp>
#include <user/lib/image_materializer.hpp>

#include "deploypack/golden_fixture.hpp"

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    __builtin_trap();
}
} // namespace libk

namespace {

struct FakeBackend final {
    enum class Op : uint8_t {
        Close,
        Child,
        VSpace,
        CSpace,
        Memory,
        Region,
        Map,
        Unmap,
        Destroy,
        ResourceClose,
        Seal,
        Write,
    };

    struct Call final {
        Op op;
        myos::cap::CapRef first;
        myos::cap::CapRef second;
        myos_word_t address;
        myos_word_t size;
        myos_word_t access;
        myos_word_t types;
        myos_word_t rights;
        bool zero;

        constexpr Call() noexcept : Call(Op::Close) {}

        constexpr Call(
            Op operation,
            myos::cap::CapRef first_reference = {},
            myos::cap::CapRef second_reference = {},
            myos_word_t call_address = 0,
            myos_word_t call_size = 0,
            myos_word_t call_access = 0,
            myos_word_t call_types = 0,
            myos_word_t call_rights = 0,
            bool call_zero = false) noexcept
            : op(operation),
              first(first_reference),
              second(second_reference),
              address(call_address),
              size(call_size),
              access(call_access),
              types(call_types),
              rights(call_rights),
              zero(call_zero) {}
    };

    static inline Call calls[256]{};
    static inline size_t call_count{};
    static inline myos_status_t next_close{MYOS_STATUS_OK};
    static inline myos_status_t next_child{MYOS_STATUS_OK};
    static inline myos_status_t next_vspace{MYOS_STATUS_OK};
    static inline myos_status_t next_cspace{MYOS_STATUS_OK};
    static inline myos_status_t next_memory{MYOS_STATUS_OK};
    static inline myos_status_t next_pager_memory{MYOS_STATUS_OK};
    static inline myos_status_t next_region{MYOS_STATUS_OK};
    static inline myos_status_t next_map{MYOS_STATUS_OK};
    static inline myos_status_t next_unmap{MYOS_STATUS_OK};
    static inline myos_status_t next_destroy{MYOS_STATUS_OK};
    static inline myos_status_t next_resource_close{MYOS_STATUS_OK};
    static inline myos_status_t next_seal{MYOS_STATUS_OK};
    static inline myos_status_t next_write{MYOS_STATUS_OK};
    static inline myos_status_t map_failure{MYOS_STATUS_BUSY};
    static inline myos_status_t write_failure{MYOS_STATUS_BUSY};
    static inline size_t map_count{};
    static inline size_t fail_map_at{};
    static inline size_t write_count{};
    static inline size_t fail_write_at{};
    static inline myos_cap_t next_cap{100};
    static inline myos_cap_t nonzero_memory_failure_cap{};
    static inline myos_cap_t nonzero_pager_failure_cap{};
    static inline myos_cap_t nonzero_region_failure_cap{};
    static inline myos_word_t scratch_address{};
    static inline uint8_t snapshots[8][0x4000]{};
    static inline size_t snapshot_count{};
    static inline size_t snapshot_size{};

    static void reset() noexcept {
        call_count = 0;
        next_close = MYOS_STATUS_OK;
        next_child = MYOS_STATUS_OK;
        next_vspace = MYOS_STATUS_OK;
        next_cspace = MYOS_STATUS_OK;
        next_memory = MYOS_STATUS_OK;
        next_pager_memory = MYOS_STATUS_OK;
        next_region = MYOS_STATUS_OK;
        next_map = MYOS_STATUS_OK;
        next_unmap = MYOS_STATUS_OK;
        next_destroy = MYOS_STATUS_OK;
        next_resource_close = MYOS_STATUS_OK;
        next_seal = MYOS_STATUS_OK;
        next_write = MYOS_STATUS_OK;
        map_failure = MYOS_STATUS_BUSY;
        write_failure = MYOS_STATUS_BUSY;
        map_count = 0;
        fail_map_at = 0;
        write_count = 0;
        fail_write_at = 0;
        next_cap = 100;
        nonzero_memory_failure_cap = 0;
        nonzero_pager_failure_cap = 0;
        nonzero_region_failure_cap = 0;
        snapshot_count = 0;
        snapshot_size = 0;
    }

    static void record(Call call) noexcept {
        if (call_count < sizeof(calls) / sizeof(calls[0])) {
            calls[call_count++] = call;
        }
    }

    [[nodiscard]] static auto consume(myos_status_t& status) noexcept
        -> myos_status_t {
        const myos_status_t result = status;
        status = MYOS_STATUS_OK;
        return result;
    }

    [[nodiscard]] static auto close(
        myos::cap::CapRef reference) noexcept -> myos_status_t {
        record(Call{Op::Close, reference});
        return consume(next_close);
    }

    static void ownership_fault(myos_status_t) noexcept { __builtin_trap(); }

    [[nodiscard]] static auto resource_create_child(
        myos::cap::CapRef pool,
        myos_word_t,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        record(Call{Op::Child, pool});
        const myos_status_t status = consume(next_child);
        return {status, status == MYOS_STATUS_OK ? myos_word_t{10} : 0, 0};
    }

    [[nodiscard]] static auto resource_close(
        myos::cap::CapRef pool) noexcept -> myos_status_t {
        record(Call{Op::ResourceClose, pool});
        return consume(next_resource_close);
    }

    [[nodiscard]] static auto vspace_create(
        myos::cap::CapRef pool) noexcept -> myos::SysResult {
        record(Call{Op::VSpace, pool});
        const myos_status_t status = consume(next_vspace);
        return {status, status == MYOS_STATUS_OK ? myos_word_t{11} : 0, 0};
    }

    [[nodiscard]] static auto cspace_create(
        myos::cap::CapRef pool,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        record(Call{Op::CSpace, pool});
        const myos_status_t status = consume(next_cspace);
        return {status, status == MYOS_STATUS_OK ? myos_word_t{12} : 0, 0};
    }

    [[nodiscard]] static auto memory_create(
        myos::cap::CapRef pool,
        myos_word_t size,
        myos_word_t access) noexcept -> myos::SysResult {
        record(Call{Op::Memory, pool, {}, 0, size, access});
        const myos_status_t status = consume(next_memory);
        if (status != MYOS_STATUS_OK && nonzero_memory_failure_cap != 0) {
            const myos_cap_t selector = nonzero_memory_failure_cap;
            nonzero_memory_failure_cap = 0;
            return {status, selector, 0};
        }
        return {status, status == MYOS_STATUS_OK ? next_cap++ : 0, 0};
    }

    [[nodiscard]] static auto memory_create_pager(
        myos::cap::CapRef pool,
        myos_word_t size,
        myos_word_t access,
        myos::cap::CapRef pager) noexcept -> myos::SysResult {
        record(Call{Op::Memory, pool, pager, 0, size, access});
        const myos_status_t status = consume(next_pager_memory);
        if (status != MYOS_STATUS_OK && nonzero_pager_failure_cap != 0) {
            const myos_cap_t selector = nonzero_pager_failure_cap;
            nonzero_pager_failure_cap = 0;
            return {status, selector, 0};
        }
        return {status, status == MYOS_STATUS_OK ? next_cap++ : 0, 0};
    }

    [[nodiscard]] static auto memory_seal(
        myos::cap::CapRef memory) noexcept -> myos_status_t {
        record(Call{Op::Seal, memory});
        return consume(next_seal);
    }

    [[nodiscard]] static auto memory_write(
        void* destination,
        const uint8_t* source,
        size_t size) noexcept -> myos_status_t {
        ++write_count;
        record(Call{
            Op::Write,
            {}, {},
            reinterpret_cast<myos_word_t>(destination),
            static_cast<myos_word_t>(size), 0, 0, 0, source == nullptr});
        if (fail_write_at != 0 && write_count == fail_write_at) {
            fail_write_at = 0;
            return consume(write_failure);
        }
        const myos_status_t status = consume(next_write);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        auto* const bytes = static_cast<uint8_t*>(destination);
        for (size_t index = 0; index < size; ++index) {
            bytes[index] = source == nullptr ? 0 : source[index];
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto vm_create_region(
        myos::cap::CapRef vspace,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        myos_word_t types,
        myos_word_t rights) noexcept -> myos::SysResult {
        record(Call{Op::Region, vspace, {}, address, size,
            access, types, rights});
        const myos_status_t status = consume(next_region);
        if (status != MYOS_STATUS_OK && nonzero_region_failure_cap != 0) {
            const myos_cap_t selector = nonzero_region_failure_cap;
            nonzero_region_failure_cap = 0;
            return {status, selector, 0};
        }
        return {status, status == MYOS_STATUS_OK ? next_cap++ : 0, 0};
    }

    [[nodiscard]] static auto vm_map(
        myos::cap::CapRef region,
        myos::cap::CapRef memory,
        myos_word_t address,
        myos_word_t size,
        myos_word_t object_page,
        myos_word_t access) noexcept -> myos_status_t {
        ++map_count;
        record(Call{Op::Map, region, memory, address, size,
            access, object_page, 0});
        if (fail_map_at != 0 && map_count == fail_map_at) {
            fail_map_at = 0;
            return consume(map_failure);
        }
        return consume(next_map);
    }

    [[nodiscard]] static auto vm_unmap(
        myos::cap::CapRef region,
        myos_word_t address,
        myos_word_t size) noexcept -> myos_status_t {
        record(Call{Op::Unmap, region, {}, address, size});
        if (address == scratch_address && snapshot_count
                < sizeof(snapshots) / sizeof(snapshots[0])) {
            snapshot_size = size < sizeof(snapshots[0])
                ? static_cast<size_t>(size)
                : sizeof(snapshots[0]);
            auto* const source = reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(address));
            for (size_t index = 0; index < snapshot_size; ++index) {
                snapshots[snapshot_count][index] = source[index];
            }
            ++snapshot_count;
        }
        return consume(next_unmap);
    }

    [[nodiscard]] static auto vm_destroy_region(
        myos::cap::CapRef region) noexcept -> myos_status_t {
        record(Call{Op::Destroy, region});
        return consume(next_destroy);
    }
};

using Space = myos::deploy::TaskSpace<16, 4, FakeBackend>;
using SmallSpace = myos::deploy::TaskSpace<2, 4, FakeBackend>;
using Bundle = myos::deploy::MappedBundle<FakeBackend>;
using Scratch = myos::deploy::ScratchWindow<FakeBackend>;
using Materializer = myos::deploy::ImageMaterializer<
    16, 4, FakeBackend, 4, 4>;
using SmallMaterializer = myos::deploy::ImageMaterializer<
    2, 4, FakeBackend, 4, 4>;
using Image = Materializer::Image;

static_assert(myos::deploy::MaterializerBackend<FakeBackend>);

alignas(4096) uint8_t bundle_bytes[8192]{};
alignas(4096) uint8_t scratch_bytes[0x10000]{};

struct PlanFixture final {
    uint8_t raw[myos::deploy::host::kGoldenSize]{};
    myos::deploy::ManifestWorkspace workspace{};
    myos::deploy::PlanSet<1> plans{};
    myos::deploy::DeploymentPlan plan{};
};

void put_plan(
    uint8_t* bytes,
    size_t offset,
    uint64_t value,
    size_t width) noexcept {
    for (size_t index = 0; index < width; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(
            value >> (index * 8));
    }
}

[[nodiscard]] auto read_plan(
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

[[nodiscard]] auto make_query_plan(
    PlanFixture& fixture,
    uint64_t stack_size = 0x10000) noexcept -> bool {
    for (size_t index = 0; index < sizeof(fixture.raw); ++index) {
        fixture.raw[index] = myos::deploy::host::kGolden[index];
    }
    const size_t task_table = static_cast<size_t>(read_plan(
        fixture.raw,
        MYOS_DEPLOY_HEADER_TABLES
            + MYOS_DEPLOY_TABLE_TASK * MYOS_DEPLOY_TABLE_DESC_SIZE
            + MYOS_DEPLOY_TABLE_OFFSET,
        8));
    const size_t mapping_table = static_cast<size_t>(read_plan(
        fixture.raw,
        MYOS_DEPLOY_HEADER_TABLES
            + MYOS_DEPLOY_TABLE_MAPPING * MYOS_DEPLOY_TABLE_DESC_SIZE
            + MYOS_DEPLOY_TABLE_OFFSET,
        8));
    put_plan(
        fixture.raw,
        task_table + MYOS_DEPLOY_TASK_POOL_MEMORY,
        0x20000,
        8);
    put_plan(
        fixture.raw,
        task_table + MYOS_DEPLOY_TASK_CRITICAL_BYTES,
        0x12000,
        8);
    put_plan(
        fixture.raw,
        mapping_table + MYOS_DEPLOY_MAPPING_STRIDE
            + MYOS_DEPLOY_MAPPING_SIZE,
        stack_size,
        8);
    auto parsed = myos::deploy::ManifestView::parse(
        fixture.raw, sizeof(fixture.raw), fixture.workspace);
    if (!parsed) {
        return false;
    }
    auto decoded = myos::deploy::DeploymentPlan::decode(
        parsed.value(), fixture.plans);
    if (!decoded) {
        return false;
    }
    fixture.plan = libk::move(decoded.value());
    return fixture.plan.task_count() == 1
        && fixture.plan.image_count() == 1
        && fixture.plan.mapping_count() == 3;
}

void put_bundle(size_t offset, uint64_t value, size_t width) noexcept {
    for (size_t index = 0; index < width; ++index) {
        bundle_bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

[[nodiscard]] auto make_bundle() noexcept -> size_t {
    constexpr size_t modules = MYOS_BOOT_HEADER_SIZE;
    constexpr size_t segments = modules + MYOS_BOOT_MODULE_SIZE;
    constexpr size_t name = segments + 2 * MYOS_BOOT_SEGMENT_SIZE;
    constexpr size_t image = name + 8;
    constexpr size_t image_size = 0x1003;
    constexpr size_t total = image + image_size;
    for (size_t index = 0; index < total; ++index) {
        bundle_bytes[index] = 0;
    }
    put_bundle(0, MYOS_BOOT_MAGIC, 8);
    put_bundle(8, MYOS_BOOT_MAJOR, 2);
    put_bundle(10, MYOS_BOOT_MINOR, 2);
    put_bundle(12, MYOS_BOOT_HEADER_SIZE, 4);
    put_bundle(16, total, 8);
    put_bundle(24, MYOS_BOOT_ARCH_RISCV64, 4);
    put_bundle(28, MYOS_BOOT_ABI_RISCV_LP64, 4);
    put_bundle(40, modules, 8);
    put_bundle(48, 1, 4);
    put_bundle(56, segments, 8);
    put_bundle(64, 2, 4);
    put_bundle(modules, name, 8);
    put_bundle(modules + 8, 5, 4);
    put_bundle(modules + 12, MYOS_BOOT_MODULE_BOOTABLE, 4);
    put_bundle(modules + 16, image, 8);
    put_bundle(modules + 24, image_size, 8);
    put_bundle(modules + 32, 0x200000, 8);
    put_bundle(modules + 40, 0, 4);
    put_bundle(modules + 44, 2, 4);
    for (size_t index = 0; index < 5; ++index) {
        bundle_bytes[name + index] = static_cast<uint8_t>("proof"[index]);
    }
    put_bundle(segments, 0x200000, 8);
    put_bundle(segments + 8, image, 8);
    put_bundle(segments + 16, 5, 8);
    put_bundle(segments + 24, 0x1000, 8);
    put_bundle(segments + 32, 0x1000, 8);
    put_bundle(segments + 40, MYOS_BOOT_SEGMENT_EXECUTE, 4);
    put_bundle(segments + MYOS_BOOT_SEGMENT_SIZE, 0x210000, 8);
    put_bundle(segments + MYOS_BOOT_SEGMENT_SIZE + 8, image + 0x1000, 8);
    put_bundle(segments + MYOS_BOOT_SEGMENT_SIZE + 16, 3, 8);
    put_bundle(segments + MYOS_BOOT_SEGMENT_SIZE + 24, 0x2000, 8);
    put_bundle(segments + MYOS_BOOT_SEGMENT_SIZE + 32, 0x1000, 8);
    put_bundle(segments + MYOS_BOOT_SEGMENT_SIZE + 40,
        MYOS_BOOT_SEGMENT_READ | MYOS_BOOT_SEGMENT_WRITE, 4);
    bundle_bytes[image] = 0xa1;
    bundle_bytes[image + 1] = 0xa2;
    bundle_bytes[image + 2] = 0xa3;
    bundle_bytes[image + 3] = 0xa4;
    bundle_bytes[image + 4] = 0xa5;
    bundle_bytes[image + 0x1000] = 0xb1;
    bundle_bytes[image + 0x1001] = 0xb2;
    bundle_bytes[image + 0x1002] = 0xb3;
    return total;
}

[[nodiscard]] auto make_query_bundle() noexcept -> size_t {
    const size_t total = make_bundle();
    constexpr size_t module = MYOS_BOOT_HEADER_SIZE;
    constexpr size_t name = module + MYOS_BOOT_MODULE_SIZE
        + 2 * MYOS_BOOT_SEGMENT_SIZE;
    put_bundle(64, 1, 4);
    put_bundle(module + 8, 4, 4);
    put_bundle(module + 44, 1, 4);
    bundle_bytes[name + 0] = 'i';
    bundle_bytes[name + 1] = 'n';
    bundle_bytes[name + 2] = 'i';
    bundle_bytes[name + 3] = 't';
    bundle_bytes[name + 4] = 0;
    return total;
}

[[nodiscard]] auto count(FakeBackend::Op op) noexcept -> size_t {
    size_t result = 0;
    for (size_t index = 0; index < FakeBackend::call_count; ++index) {
        if (FakeBackend::calls[index].op == op) {
            ++result;
        }
    }
    return result;
}

[[nodiscard]] auto first(
    FakeBackend::Op op,
    size_t start = 0) noexcept -> size_t {
    for (size_t index = start; index < FakeBackend::call_count; ++index) {
        if (FakeBackend::calls[index].op == op) {
            return index;
        }
    }
    return FakeBackend::call_count;
}

[[nodiscard]] auto has_close_selector(myos_cap_t selector) noexcept -> bool {
    for (size_t index = 0; index < FakeBackend::call_count; ++index) {
        const auto& call = FakeBackend::calls[index];
        if (call.op == FakeBackend::Op::Close
            && call.first.selector == selector && call.first.cspace == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto has_resource_close_selector(
    myos_cap_t selector) noexcept -> bool {
    for (size_t index = 0; index < FakeBackend::call_count; ++index) {
        const auto& call = FakeBackend::calls[index];
        if (call.op == FakeBackend::Op::ResourceClose
            && call.first.selector == selector && call.first.cspace == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto environment_roots_closed() noexcept -> bool {
    return has_close_selector(10)
        && has_close_selector(11)
        && has_close_selector(12)
        && has_close_selector(100)
        && has_close_selector(101)
        && has_resource_close_selector(10);
}

[[nodiscard]] auto materializer_caps_closed() noexcept -> bool {
    for (myos_cap_t selector = 102;
         selector < FakeBackend::next_cap; ++selector) {
        if (!has_close_selector(selector)) {
            return false;
        }
    }
    return true;
}

template<typename Task>
[[nodiscard]] auto open_environment(
    Task& task,
    Bundle& bundle,
    Scratch& scratch,
    size_t bundle_size,
    myos_word_t scratch_size = sizeof(scratch_bytes)) noexcept -> bool {
    FakeBackend::reset();
    FakeBackend::scratch_address = reinterpret_cast<myos_word_t>(scratch_bytes);
    if (task.open({1, 0}, 8192, 64, MYOS_RESOURCE_E7_KINDS, 32, 8)
            != MYOS_STATUS_OK) {
        return false;
    }
    const auto vspace = task.lookup(
        task.vspace_slot(), MYOS_OBJECT_KIND_VSPACE);
    const myos::deploy::Window bundle_window{
        reinterpret_cast<myos_word_t>(bundle_bytes), 0x2000};
    const myos::deploy::Window scratch_window{
        reinterpret_cast<myos_word_t>(scratch_bytes), scratch_size};
    return vspace.has_value()
        && bundle.open(vspace.value(), {2, 0}, bundle_window, bundle_size)
            == MYOS_STATUS_OK
        && scratch.open(vspace.value(), scratch_window) == MYOS_STATUS_OK;
}

template<typename Task>
[[nodiscard]] auto close_environment(
    Task& task, Bundle& bundle, Scratch& scratch) noexcept -> bool {
    myos_status_t status = MYOS_STATUS_INTERNAL;
    for (size_t attempt = 0; attempt < 4; ++attempt) {
        status = scratch.close();
        if (status == MYOS_STATUS_OK) {
            break;
        }
    }
    const bool scratch_closed = status == MYOS_STATUS_OK
        && scratch.phase() == myos::deploy::LeasePhase::Closed;
    status = MYOS_STATUS_INTERNAL;
    for (size_t attempt = 0; attempt < 4; ++attempt) {
        status = bundle.close();
        if (status == MYOS_STATUS_OK) {
            break;
        }
    }
    const bool bundle_closed = status == MYOS_STATUS_OK
        && bundle.phase() == myos::deploy::LeasePhase::Closed;
    status = MYOS_STATUS_INTERNAL;
    for (size_t attempt = 0; attempt < 4; ++attempt) {
        status = task.close();
        if (status == MYOS_STATUS_OK) {
            break;
        }
    }
    return scratch_closed && bundle_closed && status == MYOS_STATUS_OK
        && task.phase() == myos::deploy::Phase::Closed;
}

[[nodiscard]] auto test_production_zero_fill() noexcept -> bool {
    uint8_t bytes[4] = {0xcc, 0xcc, 0xcc, 0xcc};
    constexpr uint8_t source[2] = {0x12, 0x34};
    if (myos::cap::SyscallBackend::memory_write(
            bytes, source, sizeof(source)) != MYOS_STATUS_OK
        || bytes[0] != 0x12 || bytes[1] != 0x34) {
        return false;
    }
    if (myos::cap::SyscallBackend::memory_write(
            bytes + 2, nullptr, 2) != MYOS_STATUS_OK
        || bytes[2] != 0 || bytes[3] != 0) {
        return false;
    }
    return myos::cap::SyscallBackend::memory_write(
               nullptr, nullptr, 0) == MYOS_STATUS_BAD_ARGS;
}

[[nodiscard]] auto test_success_and_order() noexcept -> bool {
    const size_t bundle_size = make_bundle();
    Space task{};
    Bundle bundle{};
    Scratch scratch{};
    if (!open_environment(task, bundle, scratch, bundle_size)) {
        return false;
    }
    Materializer materializer{task, bundle, scratch};
    Image image{};
    if (materializer.materialize("proof", image) != MYOS_STATUS_OK
        || image.entry != 0x200000
        || image.segments.size() != 2
        || image.segments[0].access
            != MYOS_VM_EXECUTE
        || image.segments[1].access
            != (MYOS_VM_READ | MYOS_VM_WRITE)
        || count(FakeBackend::Op::Memory) != 2
        || count(FakeBackend::Op::Seal) != 1
        || count(FakeBackend::Op::Write) != 4
        || count(FakeBackend::Op::Unmap) != 2
        || count(FakeBackend::Op::Map) != 5
        || FakeBackend::snapshot_count != 2
        || FakeBackend::snapshot_size != 0x2000) {
        (void)close_environment(task, bundle, scratch);
        return false;
    }
    const size_t memory = first(FakeBackend::Op::Memory);
    const size_t write = first(FakeBackend::Op::Write, memory);
    const size_t unmap = first(FakeBackend::Op::Unmap, write);
    const size_t seal = first(FakeBackend::Op::Seal, unmap);
    const size_t region = first(FakeBackend::Op::Region, seal);
    const size_t map = first(FakeBackend::Op::Map, region);
    const bool order = memory < write && write < unmap && unmap < seal
        && seal < region && region < map
        && FakeBackend::calls[region].access
            == MYOS_VM_EXECUTE
        && FakeBackend::calls[map].access
            == MYOS_VM_EXECUTE
        && FakeBackend::calls[memory].access
            == (MYOS_VM_READ | MYOS_VM_WRITE | MYOS_VM_EXECUTE);
    const auto first_memory = image.segments[0].memory;
    const auto second_memory = image.segments[1].memory;
    const auto first_region = image.segments[0].region;
    const bool live_before_retire =
        task.lookup(first_memory, MYOS_OBJECT_KIND_MEMORY).has_value()
        && task.lookup(second_memory, MYOS_OBJECT_KIND_MEMORY).has_value()
        && first(FakeBackend::Op::Close, map) == FakeBackend::call_count;
    FakeBackend::next_close = MYOS_STATUS_BUSY;
    const myos_status_t partial = materializer.retire_sources(image);
    const bool retained_after_failure =
        partial == MYOS_STATUS_BUSY
        && task.lookup(first_memory, MYOS_OBJECT_KIND_MEMORY).has_value();
    const myos_status_t retired_status = materializer.retire_sources(image);
    const bool retired = retired_status == MYOS_STATUS_OK
        && !task.lookup(first_memory, MYOS_OBJECT_KIND_MEMORY)
        && !task.lookup(second_memory, MYOS_OBJECT_KIND_MEMORY)
        && task.lookup(first_region, MYOS_OBJECT_KIND_VSPACE).has_value();
    const bool data = FakeBackend::snapshots[0][0] == 0xa1
        && FakeBackend::snapshots[0][4] == 0xa5
        && FakeBackend::snapshots[0][5] == 0
        && FakeBackend::snapshots[1][0] == 0xb1
        && FakeBackend::snapshots[1][2] == 0xb3
        && FakeBackend::snapshots[1][3] == 0;
    const bool closed = close_environment(task, bundle, scratch);
    return order && live_before_retire && retained_after_failure && retired
        && data && closed && environment_roots_closed()
        && materializer_caps_closed();
}

[[nodiscard]] auto test_stack_plan() noexcept -> bool {
    const size_t bundle_size = make_bundle();
    Space task{};
    Bundle bundle{};
    Scratch scratch{};
    if (!open_environment(task, bundle, scratch, bundle_size)) {
        return false;
    }
    Materializer materializer{task, bundle, scratch};
    Image image{};
    const bool success = materializer.materialize_stacks(
        2, 0x400000, 0x2000, 0x1000, image) == MYOS_STATUS_OK
        && image.stacks.size() == 2
        && image.stacks[0].top == 0x401000
        && image.stacks[1].mapping.address == 0x402000
        && count(FakeBackend::Op::Seal) == 0
        && count(FakeBackend::Op::Write) == 0
        && task.lookup(
            image.stacks[0].mapping.memory,
            MYOS_OBJECT_KIND_MEMORY).has_value();
    const bool retired = success
        && materializer.retire_sources(image) == MYOS_STATUS_OK
        && !task.lookup(
            image.stacks[0].mapping.memory, MYOS_OBJECT_KIND_MEMORY);
    const bool closed = close_environment(task, bundle, scratch);
    return success && retired && closed && environment_roots_closed()
        && materializer_caps_closed();
}

[[nodiscard]] auto test_descriptor_and_readonly() noexcept -> bool {
    const size_t bundle_size = make_bundle();
    Space task{};
    Bundle bundle{};
    Scratch scratch{};
    if (!open_environment(task, bundle, scratch, bundle_size)) {
        return false;
    }
    Materializer materializer{task, bundle, scratch};
    constexpr uint8_t bytes[] = {0x31, 0x32, 0x33, 0x34};
    Image::Mapping readonly{};
    myos::deploy::LocalSlot descriptor{};
    const auto readonly_status = materializer.materialize_readonly(
        0x500000, bytes, sizeof(bytes), readonly);
    const auto descriptor_status = materializer.materialize_descriptor(
        bytes, sizeof(bytes), descriptor);
    const bool materialized = readonly_status == MYOS_STATUS_OK
        && readonly.access == MYOS_VM_READ
        && readonly.address == 0x500000
        && readonly.size == MYOS_DEPLOY_PAGE_SIZE
        && task.lookup(readonly.region, MYOS_OBJECT_KIND_VSPACE)
            .has_value()
        && !task.lookup(readonly.memory, MYOS_OBJECT_KIND_MEMORY)
        && descriptor_status == MYOS_STATUS_OK
        && task.lookup(descriptor, MYOS_OBJECT_KIND_MEMORY).has_value()
        && count(FakeBackend::Op::Memory) == 2
        && count(FakeBackend::Op::Region) == 3
        && count(FakeBackend::Op::Map) == 4
        && count(FakeBackend::Op::Write) == 4;
    const bool closed = close_environment(task, bundle, scratch);
    return materialized && closed && environment_roots_closed()
        && materializer_caps_closed();
}

[[nodiscard]] auto test_stack_failures() noexcept -> bool {
    const size_t bundle_size = make_bundle();

    auto run = [&](auto configure, myos_status_t expected) noexcept -> bool {
        Space task{};
        Bundle bundle{};
        Scratch scratch{};
        if (!open_environment(task, bundle, scratch, bundle_size)) {
            return false;
        }
        configure();
        Materializer materializer{task, bundle, scratch};
        Image image{};
        const myos_status_t status = materializer.materialize_stacks(
            2, 0x400000, 0x2000, 0x1000, image);
        const bool result = status == expected && image.stacks.empty();
        const bool closed = close_environment(task, bundle, scratch);
        return result && closed && environment_roots_closed()
            && materializer_caps_closed();
    };

    if (!run([] { FakeBackend::next_memory = MYOS_STATUS_BUSY; },
            MYOS_STATUS_BUSY)) {
        return false;
    }
    if (!run([] { FakeBackend::next_region = MYOS_STATUS_BUSY; },
            MYOS_STATUS_BUSY)) {
        return false;
    }
    if (!run([] {
            FakeBackend::fail_map_at = FakeBackend::map_count + 1;
            FakeBackend::map_failure = MYOS_STATUS_BUSY;
        }, MYOS_STATUS_BUSY)) {
        return false;
    }
    {
        Space task{};
        Bundle bundle{};
        Scratch scratch{};
        if (!open_environment(task, bundle, scratch, bundle_size)) {
            return false;
        }
        FakeBackend::next_close = MYOS_STATUS_BUSY;
        Materializer materializer{task, bundle, scratch};
        Image image{};
        const bool deferred =
            materializer.materialize_stacks(
                2, 0x400000, 0x2000, 0x1000, image) == MYOS_STATUS_OK
            && !image.stacks.empty()
            && materializer.retire_sources(image) == MYOS_STATUS_BUSY
            && materializer.retire_sources(image) == MYOS_STATUS_OK;
        const bool closed = close_environment(task, bundle, scratch);
        if (!deferred || !closed || !environment_roots_closed()
            || !materializer_caps_closed()) {
            return false;
        }
    }

    auto run_invalid = [&](size_t stack_count, myos_word_t base,
                           myos_word_t stride, myos_word_t size) noexcept
        -> bool {
        Space task{};
        Bundle bundle{};
        Scratch scratch{};
        if (!open_environment(task, bundle, scratch, bundle_size)) {
            return false;
        }
        Materializer materializer{task, bundle, scratch};
        Image image{};
        const bool rejected = materializer.materialize_stacks(
            stack_count, base, stride, size, image) == MYOS_STATUS_BAD_ARGS
            && image.stacks.empty();
        const bool closed = close_environment(task, bundle, scratch);
        return rejected && closed && environment_roots_closed()
            && materializer_caps_closed();
    };

    const myos_word_t near_end = ~myos_word_t{}
        - (MYOS_DEPLOY_PAGE_SIZE - 1);
    if (!run_invalid(0, 0x400000, 0x2000, 0x1000)
        || !run_invalid(5, 0x400000, 0x2000, 0x1000)
        || !run_invalid(2, 0x400000, 0x1000, 0x2000)
        || !run_invalid(2, 0x400000, near_end, 0x1000)
        || !run_invalid(1, near_end, 0x2000, 0x2000)) {
        return false;
    }

    SmallSpace task{};
    Bundle bundle{};
    Scratch scratch{};
    if (!open_environment(task, bundle, scratch, bundle_size)) {
        return false;
    }
    SmallMaterializer materializer{task, bundle, scratch};
    Image image{};
    const bool capacity = materializer.materialize_stacks(
            1, 0x400000, 0x2000, 0x1000, image)
            == MYOS_STATUS_NO_MEMORY
        && image.stacks.empty();
    const bool closed = close_environment(task, bundle, scratch);
    return capacity && closed && environment_roots_closed()
        && materializer_caps_closed()
        && has_close_selector(102) && has_close_selector(103);
}

[[nodiscard]] auto test_nonzero_results_are_closed() noexcept -> bool {
    const size_t bundle_size = make_bundle();

    {
        Space task{};
        Bundle bundle{};
        Scratch scratch{};
        if (!open_environment(task, bundle, scratch, bundle_size)) {
            return false;
        }
        const size_t baseline = task.local_cumulative();
        FakeBackend::next_memory = MYOS_STATUS_BUSY;
        FakeBackend::nonzero_memory_failure_cap = 700;
        Materializer materializer{task, bundle, scratch};
        Image image{};
        const bool result = materializer.materialize_stacks(
                1, 0x400000, 0x2000, 0x1000, image)
                == MYOS_STATUS_BUSY
            && image.stacks.empty()
            && task.local_cumulative() == baseline
            && has_close_selector(700);
        const bool closed = close_environment(task, bundle, scratch);
        if (!result || !closed || !environment_roots_closed()
            || !materializer_caps_closed()) {
            return false;
        }
    }

    {
        Space task{};
        Bundle bundle{};
        Scratch scratch{};
        if (!open_environment(task, bundle, scratch, bundle_size)) {
            return false;
        }
        const size_t baseline = task.local_cumulative();
        FakeBackend::next_region = MYOS_STATUS_BUSY;
        FakeBackend::nonzero_region_failure_cap = 701;
        Materializer materializer{task, bundle, scratch};
        Image image{};
        const bool result = materializer.materialize_stacks(
                1, 0x400000, 0x2000, 0x1000, image)
                == MYOS_STATUS_BUSY
            && image.stacks.empty()
            && task.local_cumulative() == baseline + 1
            && has_close_selector(701);
        const bool closed = close_environment(task, bundle, scratch);
        if (!result || !closed || !environment_roots_closed()
            || !materializer_caps_closed() || !has_close_selector(100)) {
            return false;
        }
    }

    {
        Space task{};
        Bundle bundle{};
        Scratch scratch{};
        if (!open_environment(task, bundle, scratch, bundle_size)) {
            return false;
        }
        const size_t baseline = task.local_cumulative();
        FakeBackend::next_pager_memory = MYOS_STATUS_BUSY;
        FakeBackend::nonzero_pager_failure_cap = 702;
        Materializer materializer{task, bundle, scratch};
        Image::Mapping mapping{};
        const bool result = materializer.materialize_paged(
                {77, 0}, 0x600000, 0x1000, MYOS_VM_READ, mapping)
                == MYOS_STATUS_BUSY
            && !mapping.memory.valid()
            && task.local_cumulative() == baseline
            && has_close_selector(702);
        const bool closed = close_environment(task, bundle, scratch);
        if (!result || !closed || !environment_roots_closed()
            || !materializer_caps_closed()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto test_write_failure_unmaps_scratch() noexcept -> bool {
    const size_t bundle_size = make_bundle();
    for (const size_t failure : {size_t{1}, size_t{2}}) {
        Space task{};
        Bundle bundle{};
        Scratch scratch{};
        if (!open_environment(task, bundle, scratch, bundle_size)) {
            return false;
        }
        FakeBackend::fail_write_at = failure;
        FakeBackend::write_failure = MYOS_STATUS_BUSY;
        Materializer materializer{task, bundle, scratch};
        Image image{};
        const myos_status_t status = materializer.materialize("proof", image);
        const bool cleaned = status == MYOS_STATUS_BUSY
            && image.segments.empty()
            && scratch.phase() == myos::deploy::LeasePhase::Ready
            && count(FakeBackend::Op::Unmap) == 1;
        const bool closed = close_environment(task, bundle, scratch);
        if (!cleaned || !closed || !environment_roots_closed()
            || !materializer_caps_closed()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto test_segment_failures() noexcept -> bool {
    const size_t bundle_size = make_bundle();

    auto run = [&](auto configure, myos_status_t expected) noexcept -> bool {
        Space task{};
        Bundle bundle{};
        Scratch scratch{};
        if (!open_environment(task, bundle, scratch, bundle_size)) {
            return false;
        }
        configure();
        Materializer materializer{task, bundle, scratch};
        Image image{};
        const myos_status_t status = materializer.materialize("proof", image);
        const bool result = status == expected && image.segments.empty();
        const bool closed = close_environment(task, bundle, scratch);
        return result && closed && environment_roots_closed()
            && materializer_caps_closed();
    };

    if (!run([] { FakeBackend::next_memory = MYOS_STATUS_BUSY; },
            MYOS_STATUS_BUSY)) {
        return false;
    }
    if (!run([] { FakeBackend::next_region = MYOS_STATUS_BUSY; },
            MYOS_STATUS_BUSY)) {
        return false;
    }
    if (!run([] { FakeBackend::next_map = MYOS_STATUS_BUSY; },
            MYOS_STATUS_BUSY)) {
        return false;
    }
    if (!run([] { FakeBackend::next_write = MYOS_STATUS_BUSY; },
            MYOS_STATUS_BUSY)) {
        return false;
    }
    if (!run([] {
            FakeBackend::fail_write_at = FakeBackend::write_count + 2;
            FakeBackend::write_failure = MYOS_STATUS_BUSY;
        }, MYOS_STATUS_BUSY)) {
        return false;
    }
    if (!run([] { FakeBackend::next_unmap = MYOS_STATUS_BUSY; },
            MYOS_STATUS_BUSY)) {
        return false;
    }
    if (!run([] { FakeBackend::next_seal = MYOS_STATUS_BUSY; },
            MYOS_STATUS_BUSY)) {
        return false;
    }
    if (!run([] {
            FakeBackend::fail_map_at = FakeBackend::map_count + 2;
            FakeBackend::map_failure = MYOS_STATUS_BUSY;
        }, MYOS_STATUS_BUSY)) {
        return false;
    }

    SmallSpace task{};
    Bundle bundle{};
    Scratch scratch{};
    if (!open_environment(task, bundle, scratch, bundle_size)) {
        return false;
    }
    SmallMaterializer materializer{task, bundle, scratch};
    Image image{};
    const bool capacity = materializer.materialize("proof", image)
            == MYOS_STATUS_NO_MEMORY
        && image.segments.empty();
    const bool closed = close_environment(task, bundle, scratch);
    return capacity && closed && environment_roots_closed()
        && has_close_selector(103) && has_close_selector(102);
}

[[nodiscard]] auto test_plan_scratch_requirement() noexcept -> bool {
    PlanFixture fixture{};
    if (!make_query_plan(fixture, 0x10000)) {
        return false;
    }
    const size_t bundle_size = make_query_bundle();
    const myos::boot::Bundle parsed = myos::boot::Bundle::parse(
        bundle_bytes, bundle_size);
    auto lease = fixture.plan.lease();
    if (!parsed || !lease) {
        return false;
    }
    const auto task = lease->task(0);
    const auto requirement = myos::deploy::required_scratch_size(
        task, parsed);
    if (!requirement || *requirement != 0x10000) {
        return false;
    }
    if (myos::deploy::required_scratch_size(
            myos::deploy::TaskPlanView{}, parsed)
        || myos::deploy::required_scratch_size(task, myos::boot::Bundle{})) {
        return false;
    }

    PlanFixture minimum{};
    if (!make_query_plan(minimum, MYOS_DEPLOY_PAGE_SIZE)) {
        return false;
    }
    auto minimum_lease = minimum.plan.lease();
    if (!minimum_lease
        || !myos::deploy::required_scratch_size(
            minimum_lease->task(0), parsed)
        || *myos::deploy::required_scratch_size(
               minimum_lease->task(0), parsed) != MYOS_DEPLOY_PAGE_SIZE) {
        return false;
    }
    minimum_lease.reset();

    PlanFixture overflow{};
    if (make_query_plan(overflow, UINT64_MAX)) {
        return false;
    }

    Space task_space{};
    Bundle bundle{};
    Scratch scratch{};
    if (!open_environment(
            task_space, bundle, scratch, bundle_size, *requirement)) {
        return false;
    }
    Materializer materializer{task_space, bundle, scratch};
    Image image{};
    Image::Mapping zero{};
    const bool materialized =
        materializer.materialize("init", image) == MYOS_STATUS_OK
        && image.segments.size() == 1
        && materializer.materialize_zero(
               0x400000, *requirement,
               MYOS_VM_READ | MYOS_VM_WRITE, zero) == MYOS_STATUS_OK
        && zero.size == *requirement
        && FakeBackend::snapshot_size == sizeof(FakeBackend::snapshots[0]);
    const bool retired = materialized
        && materializer.retire_sources(image) == MYOS_STATUS_OK;
    const bool closed = close_environment(task_space, bundle, scratch);
    if (!materialized || !retired || !closed || !environment_roots_closed()
        || !materializer_caps_closed()) {
        return false;
    }

    Space undersized_task{};
    Bundle undersized_bundle{};
    Scratch undersized_scratch{};
    if (!open_environment(
            undersized_task, undersized_bundle, undersized_scratch,
            bundle_size, MYOS_DEPLOY_PAGE_SIZE)) {
        return false;
    }
    Materializer undersized_materializer{
        undersized_task, undersized_bundle, undersized_scratch};
    Image::Mapping rejected{};
    const bool rejected_zero =
        undersized_materializer.materialize_zero(
            0x400000, *requirement,
            MYOS_VM_READ | MYOS_VM_WRITE, rejected)
            == MYOS_STATUS_BAD_ARGS
        && undersized_scratch.phase() == myos::deploy::LeasePhase::Ready;
    const bool undersized_closed = close_environment(
        undersized_task, undersized_bundle, undersized_scratch);
    return rejected_zero && undersized_closed
        && environment_roots_closed() && materializer_caps_closed();
}

struct Test final {
    const char* name;
    bool (*run)() noexcept;
};

constexpr Test tests[] = {
    {"production zero fill", test_production_zero_fill},
    {"segment success/order/data", test_success_and_order},
    {"stack plan", test_stack_plan},
    {"descriptor and readonly", test_descriptor_and_readonly},
    {"stack failure matrix", test_stack_failures},
    {"nonzero result ownership", test_nonzero_results_are_closed},
    {"write failure unmaps scratch", test_write_failure_unmaps_scratch},
    {"segment failure matrix", test_segment_failures},
    {"plan scratch requirement", test_plan_scratch_requirement},
};

} // namespace

int main() {
    size_t failures = 0;
    for (const Test& test : tests) {
        if (test.run()) {
            continue;
        }
        ++failures;
        (void)fprintf(stderr, "[FAIL] %s\n", test.name);
    }
    (void)fprintf(
        stdout,
        "image materializer tests: %zu passed, %zu failed\n",
        sizeof(tests) / sizeof(tests[0]) - failures,
        failures);
    return failures == 0 ? 0 : 1;
}
