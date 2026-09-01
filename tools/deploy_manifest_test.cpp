#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <initializer_list>

#include <uapi/deploy.h>
#include <uapi/arch/riscv64/address_space.h>
#include <uapi/boot_bundle.h>
#include <uapi/resource.h>
#include <libk/assert.hpp>

#include "deploypack/golden_fixture.hpp"
#include "../user/lib/deploy_manifest.hpp"

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    __builtin_trap();
}
} // namespace libk

namespace {

using myos::deploy::Error;
using myos::deploy::ManifestImportRow;
using myos::deploy::ManifestTaskRow;
using myos::deploy::ManifestWorkspace;
using myos::deploy::ManifestView;
using myos::deploy::host::kGolden;
using myos::deploy::host::kGoldenSize;

void put(uint8_t* bytes, size_t offset, uint64_t value, size_t width);

constexpr size_t kManifestBufferSize =
    static_cast<size_t>(MYOS_DEPLOY_MAX_SIZE);
uint8_t production_bytes[kManifestBufferSize]{};
uint8_t production_mutation[kManifestBufferSize]{};
size_t production_size{};

void make_boot_bundle(uint8_t* bytes, size_t& size) {
    constexpr size_t modules = MYOS_BOOT_HEADER_SIZE;
    constexpr size_t segments = modules + MYOS_BOOT_MODULE_SIZE;
    constexpr size_t name = segments + MYOS_BOOT_SEGMENT_SIZE;
    constexpr size_t image = name + 4;
    size = image + 4;
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
    put(bytes, 0, MYOS_BOOT_MAGIC, 8);
    put(bytes, 8, MYOS_BOOT_MAJOR, 2);
    put(bytes, 10, MYOS_BOOT_MINOR, 2);
    put(bytes, 12, MYOS_BOOT_HEADER_SIZE, 4);
    put(bytes, 16, size, 8);
    put(bytes, 24, MYOS_BOOT_ARCH_RISCV64, 4);
    put(bytes, 28, MYOS_BOOT_ABI_RISCV_LP64, 4);
    put(bytes, 40, modules, 8);
    put(bytes, 48, 1, 4);
    put(bytes, 56, segments, 8);
    put(bytes, 64, 1, 4);
    put(bytes, modules, name, 8);
    put(bytes, modules + 8, 4, 4);
    put(bytes, modules + 12, MYOS_BOOT_MODULE_BOOTABLE, 4);
    put(bytes, modules + 16, image, 8);
    put(bytes, modules + 24, 4, 8);
    put(bytes, modules + 32, 0x200000, 8);
    put(bytes, modules + 40, 0, 4);
    put(bytes, modules + 44, 1, 4);
    bytes[name + 0] = 'i';
    bytes[name + 1] = 'n';
    bytes[name + 2] = 'i';
    bytes[name + 3] = 't';
    put(bytes, segments, 0x200000, 8);
    put(bytes, segments + 8, image, 8);
    put(bytes, segments + 16, 4, 8);
    put(bytes, segments + 24, 0x1000, 8);
    put(bytes, segments + 32, 0x1000, 8);
    put(bytes, segments + 40,
        MYOS_BOOT_SEGMENT_READ | MYOS_BOOT_SEGMENT_EXECUTE, 4);
}

auto matches_file(const char* path) -> bool {
    FILE* const input = fopen(path, "rb");
    if (input == nullptr) {
        return false;
    }
    size_t index{};
    int byte{};
    while ((byte = fgetc(input)) != EOF) {
        if (index >= kGoldenSize || kGolden[index++] != byte) {
            fclose(input);
            return false;
        }
    }
    const bool result = index == kGoldenSize;
    fclose(input);
    return result;
}

void put(uint8_t* bytes, size_t offset, uint64_t value, size_t width) {
    for (size_t byte = 0; byte < width; ++byte) {
        bytes[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
    }
}

auto load_production_manifest(const char* path) -> bool {
    FILE* const input = fopen(path, "rb");
    if (input == nullptr || fseek(input, 0, SEEK_END) != 0) {
        if (input != nullptr) {
            fclose(input);
        }
        return false;
    }
    const long end = ftell(input);
    if (end < 0 || static_cast<size_t>(end) > kManifestBufferSize
        || fseek(input, 0, SEEK_SET) != 0) {
        fclose(input);
        return false;
    }
    production_size = static_cast<size_t>(end);
    const size_t read = production_size == 0
        ? 0 : fread(production_bytes, 1, production_size, input);
    fclose(input);
    if (read != production_size) {
        production_size = 0;
        return false;
    }
    return true;
}

auto copy_production_manifest() -> bool {
    if (production_size == 0) {
        return false;
    }
    for (size_t index = 0; index < production_size; ++index) {
        production_mutation[index] = production_bytes[index];
    }
    return true;
}

uint64_t fnv1a(const uint8_t* bytes, size_t size) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void make_two_task_manifest(
    uint8_t* bytes,
    size_t& size,
    bool optional_second_edge) {
    constexpr size_t shift = MYOS_DEPLOY_TASK_STRIDE;
    constexpr size_t dependency_offset = 0x510;
    size = dependency_offset + 2 * MYOS_DEPLOY_DEPENDENCY_STRIDE;
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    for (size_t index = kGoldenSize; index > 0x180; --index) {
        bytes[index - 1 + shift] = kGolden[index - 1];
    }
    for (size_t index = 0; index < MYOS_DEPLOY_TASK_STRIDE; ++index) {
        bytes[0xe0 + shift + index] = bytes[0xe0 + index];
    }
    put(bytes, MYOS_DEPLOY_HEADER_TOTAL_SIZE, size, 8);
    put(bytes, MYOS_DEPLOY_HEADER_TABLES
            + MYOS_DEPLOY_TABLE_TASK * MYOS_DEPLOY_TABLE_DESC_SIZE
            + MYOS_DEPLOY_TABLE_COUNT_FIELD,
        2, 4);
    const uint64_t old_offsets[MYOS_DEPLOY_TABLE_COUNT] = {
        0xe0, 0x180, 0x1a0, 0x290, 0x2f0, 0x360, 0, 0x3c0, 0x420, 0,
    };
    for (uint32_t table = MYOS_DEPLOY_TABLE_IMAGE;
         table <= MYOS_DEPLOY_TABLE_STRING; ++table) {
        const size_t descriptor = MYOS_DEPLOY_HEADER_TABLES
            + table * MYOS_DEPLOY_TABLE_DESC_SIZE;
        put(bytes, descriptor + MYOS_DEPLOY_TABLE_OFFSET,
            old_offsets[table] + shift, 8);
    }
    const size_t dependency_descriptor = MYOS_DEPLOY_HEADER_TABLES
        + MYOS_DEPLOY_TABLE_DEPENDENCY * MYOS_DEPLOY_TABLE_DESC_SIZE;
    put(bytes, dependency_descriptor + MYOS_DEPLOY_TABLE_OFFSET,
        dependency_offset, 8);
    put(bytes, dependency_descriptor + MYOS_DEPLOY_TABLE_COUNT_FIELD, 2, 4);
    const size_t first_task = 0xe0;
    const size_t second_task = first_task + MYOS_DEPLOY_TASK_STRIDE;
    put(bytes, first_task + MYOS_DEPLOY_TASK_DEPENDENCY_COUNT, 1, 4);
    const uint32_t first_fields[7] = {
        MYOS_DEPLOY_TASK_IMAGE_FIRST, MYOS_DEPLOY_TASK_MAPPING_FIRST,
        MYOS_DEPLOY_TASK_OBJECT_FIRST, MYOS_DEPLOY_TASK_EXECUTION_FIRST,
        MYOS_DEPLOY_TASK_IMPORT_FIRST, MYOS_DEPLOY_TASK_DEPENDENCY_FIRST,
        MYOS_DEPLOY_TASK_EXPORT_FIRST,
    };
    const uint32_t count_fields[7] = {
        MYOS_DEPLOY_TASK_IMAGE_COUNT, MYOS_DEPLOY_TASK_MAPPING_COUNT,
        MYOS_DEPLOY_TASK_OBJECT_COUNT, MYOS_DEPLOY_TASK_EXECUTION_COUNT,
        MYOS_DEPLOY_TASK_IMPORT_COUNT, MYOS_DEPLOY_TASK_DEPENDENCY_COUNT,
        MYOS_DEPLOY_TASK_EXPORT_COUNT,
    };
    const uint32_t global_counts[7] = {1, 3, 1, 1, 1, 2, 1};
    for (size_t child = 0; child < 7; ++child) {
        put(bytes, second_task + first_fields[child],
            child == 5 ? 1 : global_counts[child], 4);
        put(bytes, second_task + count_fields[child],
            child == 5 ? 1 : 0, 4);
    }
    put(bytes, second_task + MYOS_DEPLOY_TASK_BOOTSTRAP_MAPPING,
        MYOS_DEPLOY_NO_INDEX, 4);
    const size_t dependency = dependency_offset;
    put(bytes, dependency + MYOS_DEPLOY_DEPENDENCY_TARGET, 1, 4);
    put(bytes, dependency + MYOS_DEPLOY_DEPENDENCY_KIND,
        MYOS_DEPLOY_DEPENDENCY_REQUIRED, 2);
    put(bytes, dependency + MYOS_DEPLOY_DEPENDENCY_FLAGS,
        MYOS_DEPLOY_DEPENDENCY_STARTUP, 2);
    put(bytes, dependency + MYOS_DEPLOY_DEPENDENCY_STRIDE
            + MYOS_DEPLOY_DEPENDENCY_TARGET,
        0, 4);
    put(bytes, dependency + MYOS_DEPLOY_DEPENDENCY_STRIDE
            + MYOS_DEPLOY_DEPENDENCY_KIND,
        optional_second_edge ? MYOS_DEPLOY_DEPENDENCY_OPTIONAL
                             : MYOS_DEPLOY_DEPENDENCY_REQUIRED,
        2);
    put(bytes, dependency + MYOS_DEPLOY_DEPENDENCY_STRIDE
            + MYOS_DEPLOY_DEPENDENCY_FLAGS,
        MYOS_DEPLOY_DEPENDENCY_STARTUP, 2);
}

void make_two_execution_manifest(uint8_t* bytes, size_t& size) {
    constexpr size_t shift = MYOS_DEPLOY_EXECUTION_STRIDE;
    constexpr size_t execution_tail = 0x360;
    constexpr size_t string_offset = 0x490;
    size = string_offset + 79;
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
    for (size_t index = kGoldenSize; index > execution_tail; --index) {
        bytes[index - 1 + shift] = kGolden[index - 1];
    }
    for (size_t index = 0; index < execution_tail; ++index) {
        bytes[index] = kGolden[index];
    }
    for (size_t index = 0; index < MYOS_DEPLOY_EXECUTION_STRIDE; ++index) {
        bytes[execution_tail + index] = bytes[0x2f0 + index];
    }
    put(bytes, MYOS_DEPLOY_HEADER_TOTAL_SIZE, size, 8);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_EXECUTION_COUNT, 2, 4);
    put(bytes, MYOS_DEPLOY_HEADER_TABLES
            + MYOS_DEPLOY_TABLE_EXECUTION * MYOS_DEPLOY_TABLE_DESC_SIZE
            + MYOS_DEPLOY_TABLE_COUNT_FIELD,
        2, 4);
    put(bytes, 0x360 + MYOS_DEPLOY_EXECUTION_KEY,
        UINT64_C(0x0000000600000040), 8);
    put(bytes, 0x360 + MYOS_DEPLOY_EXECUTION_SC,
        UINT64_C(0x0000000900000046), 8);
    put(bytes, MYOS_DEPLOY_HEADER_TABLES
            + MYOS_DEPLOY_TABLE_IMPORT * MYOS_DEPLOY_TABLE_DESC_SIZE
            + MYOS_DEPLOY_TABLE_OFFSET,
        0x3d0, 8);
    put(bytes, MYOS_DEPLOY_HEADER_TABLES
            + MYOS_DEPLOY_TABLE_EXPORT * MYOS_DEPLOY_TABLE_DESC_SIZE
            + MYOS_DEPLOY_TABLE_OFFSET,
        0x430, 8);
    put(bytes, MYOS_DEPLOY_HEADER_TABLES
            + MYOS_DEPLOY_TABLE_STRING * MYOS_DEPLOY_TABLE_DESC_SIZE
            + MYOS_DEPLOY_TABLE_OFFSET,
        string_offset, 8);
}

void make_two_object_manifest(
    uint8_t* bytes,
    size_t& size,
    uint16_t kind,
    uint16_t flags,
    const char* name,
    size_t name_size) {
    constexpr size_t object_offset = 0x480;
    constexpr size_t string_offset = 0x420;
    constexpr size_t string_count = 79;
    constexpr size_t object_name_offset = string_count;
    size = object_offset + 2 * MYOS_DEPLOY_OBJECT_STRIDE;
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    for (size_t row = 0; row < 2; ++row) {
        for (size_t index = 0; index < MYOS_DEPLOY_OBJECT_STRIDE; ++index) {
            bytes[object_offset + row * MYOS_DEPLOY_OBJECT_STRIDE + index] =
                kGolden[0x290 + index];
        }
    }
    for (size_t index = 0; index < name_size; ++index) {
        bytes[string_offset + object_name_offset + index] =
            static_cast<uint8_t>(name[index]);
    }
    put(bytes, MYOS_DEPLOY_HEADER_TOTAL_SIZE, size, 8);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_OBJECT_COUNT, 2, 4);
    const size_t object_descriptor = MYOS_DEPLOY_HEADER_TABLES
        + MYOS_DEPLOY_TABLE_OBJECT * MYOS_DEPLOY_TABLE_DESC_SIZE;
    put(bytes, object_descriptor + MYOS_DEPLOY_TABLE_OFFSET,
        object_offset, 8);
    put(bytes, object_descriptor + MYOS_DEPLOY_TABLE_COUNT_FIELD, 2, 4);
    const size_t string_descriptor = MYOS_DEPLOY_HEADER_TABLES
        + MYOS_DEPLOY_TABLE_STRING * MYOS_DEPLOY_TABLE_DESC_SIZE;
    put(bytes, string_descriptor + MYOS_DEPLOY_TABLE_COUNT_FIELD,
        string_count + name_size, 4);
    const size_t object = object_offset + MYOS_DEPLOY_OBJECT_STRIDE;
    put(bytes, object + MYOS_DEPLOY_OBJECT_OUTPUT_A,
        object_name_offset | (static_cast<uint64_t>(name_size) << 32),
        8);
    put(bytes, object + MYOS_DEPLOY_OBJECT_KIND, kind, 2);
    put(bytes, object + MYOS_DEPLOY_OBJECT_FLAGS, flags, 2);
}

void make_endpoint_manifest(uint8_t* bytes, size_t& size) {
    make_two_object_manifest(
        bytes, size, MYOS_OBJECT_KIND_ENDPOINT,
        MYOS_DEPLOY_OBJECT_POST_MAPPING, "endpoint", 8);
    put(bytes, 0x4e0 + MYOS_DEPLOY_OBJECT_REF0, 1, 4);
}

void make_channel_manifest(uint8_t* bytes, size_t& size) {
    make_two_object_manifest(
        bytes, size, MYOS_OBJECT_KIND_CHANNEL,
        MYOS_DEPLOY_OBJECT_FLAG_NONE, "channel", 7);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_KIND_MASK, MYOS_RESOURCE_E6_KINDS, 8);
    put(bytes, 0x4e0 + MYOS_DEPLOY_OBJECT_OUTPUT_B,
        UINT64_C(0x0000000900000046), 8);
    put(bytes, 0x4e0 + MYOS_DEPLOY_OBJECT_ARG0, 1, 8);
    put(bytes, 0x4e0 + MYOS_DEPLOY_OBJECT_ARG1, 1, 8);
    put(bytes, 0x4e0 + MYOS_DEPLOY_OBJECT_ARG3, 1, 8);
}

void make_pager_manifest(uint8_t* bytes, size_t& size) {
    make_two_object_manifest(
        bytes, size, MYOS_OBJECT_KIND_PAGER,
        MYOS_DEPLOY_OBJECT_EPHEMERAL_TASK, "pager", 5);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_KIND_MASK, MYOS_RESOURCE_E7_KINDS, 8);
    put(bytes, 0x4e0 + MYOS_DEPLOY_OBJECT_ARG0, 1, 8);
    put(bytes, 0x4e0 + MYOS_DEPLOY_OBJECT_ARG1, 1, 8);
}

auto accepts_golden() -> bool {
    ManifestWorkspace workspace{};
    auto parsed = ManifestView::parse(kGolden, kGoldenSize, workspace);
    return parsed && parsed.value().task_count() == 1
        && parsed.value().image_count() == 1
        && parsed.value().mapping_count() == 3
        && parsed.value().object_count() == 1
        && parsed.value().execution_count() == 1
        && parsed.value().import_count() == 1
        && parsed.value().export_count() == 1
        && parsed.value().task_name(0).size() == 4
        && fnv1a(kGolden, kGoldenSize) == UINT64_C(0xf726c0b2cea06463);
}

auto accepts_production_shape() -> bool {
    if (production_size == 0) {
        return false;
    }
    ManifestWorkspace workspace{};
    const auto parsed = ManifestView::parse(
        production_bytes, production_size, workspace);
    return parsed && parsed.value().task_count() == 5
        && parsed.value().bootstrap_count() == 35;
}

auto accepts_production_authority_budget() -> bool {
    if (production_size == 0) {
        return false;
    }
    ManifestWorkspace workspace{};
    const auto parsed = ManifestView::parse(
        production_bytes, production_size, workspace);
    if (!parsed) {
        return false;
    }
    ManifestTaskRow process{};
    ManifestTaskRow proof{};
    ManifestTaskRow consumer{};
    ManifestTaskRow pager{};
    ManifestTaskRow uart{};
    ManifestImportRow process_pool{};
    ManifestImportRow proof_pool{};
    if (!parsed.value().task_row(0, process)
        || !parsed.value().task_row(1, proof)
        || !parsed.value().task_row(2, consumer)
        || !parsed.value().task_row(3, pager)
        || !parsed.value().task_row(4, uart)
        || process.import_count == 0 || proof.import_count == 0
        || !parsed.value().import_row(process.import_first, process_pool)
        || !parsed.value().import_row(proof.import_first, proof_pool)) {
        return false;
    }
    constexpr uint64_t process_memory =
        UINT64_C(32) * UINT64_C(1024) * UINT64_C(1024)
        + MYOS_DEPLOY_PAGE_SIZE;
    const auto exact_import_rights = [&](const ManifestTaskRow& task,
                                         uint32_t local,
                                         uint64_t rights) {
        ManifestImportRow row{};
        return local < task.import_count
            && parsed.value().import_row(task.import_first + local, row)
            && row.attenuation.rights == rights;
    };
    return process.pool_memory == process_memory
        && process.pool_caps == 513
        && process.cspace_slots == 64
        && process.cspace_pages == 5
        && proof.pool_memory == UINT64_C(16) * UINT64_C(1024) * UINT64_C(1024)
        && proof.pool_caps == 256
        && proof.cspace_slots == 64
        && proof.cspace_pages == 4
        && consumer.pool_memory == UINT64_C(0x34000)
        && consumer.pool_caps == 5
        && consumer.critical_bytes == UINT64_C(0x12000)
        && consumer.cspace_slots == 5
        && consumer.cspace_pages == 3
        && consumer.readiness == MYOS_DEPLOY_READINESS_START
        && consumer.export_count == 1
        && pager.pool_memory == UINT64_C(0x3a000)
        && pager.pool_caps == 11
        && pager.critical_bytes == UINT64_C(0x14000)
        && pager.cspace_slots == 11
        && pager.cspace_pages == 4
        && pager.readiness == MYOS_DEPLOY_READINESS_EXPLICIT
        && pager.export_count == 0
        && uart.pool_memory == UINT64_C(0x37000)
        && uart.pool_caps == 10
        && uart.critical_bytes == UINT64_C(0x12000)
        && uart.cspace_slots == 10
        && uart.cspace_pages == 4
        && uart.readiness == MYOS_DEPLOY_READINESS_EXPLICIT
        && uart.export_count == 0
        && process_pool.attenuation.rights == MYOS_RIGHT_SPLIT
        && (proof_pool.attenuation.rights & MYOS_RIGHT_SPLIT) == 0
        /* Consumer only checks the closed role presence. */
        && exact_import_rights(consumer, 0, 0)
        && exact_import_rights(consumer, 1, 0)
        && exact_import_rights(consumer, 2, 0)
        && exact_import_rights(consumer, 3, 0)
        && exact_import_rights(consumer, 4, 0)
        /* Pager roots are presence-only; its service path gets only the
         * operations exercised by the worker loop. */
        && exact_import_rights(pager, 0, 0)
        && exact_import_rights(pager, 1, 0)
        && exact_import_rights(pager, 2, 0)
        && exact_import_rights(pager, 3, 0)
        && exact_import_rights(pager, 4, 0)
        && exact_import_rights(
               pager, 5, MYOS_RIGHT_SERVE | MYOS_RIGHT_SUPPLY)
        && exact_import_rights(pager, 6, MYOS_RIGHT_MANAGE)
        && exact_import_rights(pager, 7, MYOS_RIGHT_MANAGE)
        && exact_import_rights(
               pager, 8, MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE)
        && exact_import_rights(pager, 9, MYOS_RIGHT_SIGNAL)
        && exact_import_rights(pager, 10, MYOS_RIGHT_UNMAP)
        /* UART needs region creation and mapping, device mapping, and the
         * exact IRQ and wake operations used by its loop. */
        && exact_import_rights(uart, 0, 0)
        && exact_import_rights(
               uart, 1, MYOS_RIGHT_CREATE_REGION | MYOS_RIGHT_MAP)
        && exact_import_rights(uart, 2, 0)
        && exact_import_rights(uart, 3, 0)
        && exact_import_rights(uart, 4, 0)
        && exact_import_rights(uart, 5, MYOS_RIGHT_MAP)
        && exact_import_rights(
               uart, 6, MYOS_RIGHT_ROUTE | MYOS_RIGHT_OBSERVE
                   | MYOS_RIGHT_ACK)
        && exact_import_rights(
               uart, 7, MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE)
        && exact_import_rights(uart, 8, MYOS_RIGHT_SIGNAL);
}

auto rejects_minor_shape_hybrids() -> bool {
    if (!copy_production_manifest()) {
        return false;
    }
    put(production_mutation, MYOS_DEPLOY_HEADER_MINOR, 0, 2);
    ManifestWorkspace current_workspace{};
    if (ManifestView::parse(
            production_mutation, production_size, current_workspace)) {
        return false;
    }

    for (size_t index = 0; index < production_size; ++index) {
        production_mutation[index] = production_bytes[index];
    }
    put(production_mutation, MYOS_DEPLOY_HEADER_SIZE_FIELD, 208, 4);
    put(production_mutation, MYOS_DEPLOY_HEADER_TABLE_COUNT, 9, 4);
    ManifestWorkspace legacy_workspace{};
    if (ManifestView::parse(
            production_mutation, production_size, legacy_workspace)) {
        return false;
    }

    for (size_t index = 0; index < kGoldenSize; ++index) {
        production_mutation[index] = kGolden[index];
    }
    put(production_mutation, MYOS_DEPLOY_HEADER_MINOR, 1, 2);
    ManifestWorkspace legacy_minor_workspace{};
    return !ManifestView::parse(
        production_mutation, kGoldenSize, legacy_minor_workspace);
}

auto rejects_task_key_kind_relabel() -> bool {
    if (!copy_production_manifest()) {
        return false;
    }
    ManifestWorkspace workspace{};
    const auto parsed = ManifestView::parse(
        production_mutation, production_size, workspace);
    if (!parsed) {
        return false;
    }
    const auto imports = parsed.value().table(MYOS_DEPLOY_TABLE_IMPORT);
    put(production_mutation, imports.offset + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_KIND,
        MYOS_OBJECT_KIND_VSPACE, 2);
    ManifestWorkspace mutated_workspace{};
    return !ManifestView::parse(
        production_mutation, production_size, mutated_workspace);
}

auto rejects_bootstrap_kind_relabel() -> bool {
    if (!copy_production_manifest()) {
        return false;
    }
    ManifestWorkspace workspace{};
    const auto parsed = ManifestView::parse(
        production_mutation, production_size, workspace);
    if (!parsed) {
        return false;
    }
    const auto bootstraps = parsed.value().table(MYOS_DEPLOY_TABLE_BOOTSTRAP);
    put(production_mutation, bootstraps.offset + MYOS_DEPLOY_BOOTSTRAP_KIND,
        MYOS_BOOTSTRAP_CAP_VSPACE, 4);
    ManifestWorkspace mutated_workspace{};
    return !ManifestView::parse(
        production_mutation, production_size, mutated_workspace);
}

auto accepts_closed_bootstrap_kind_mapping() -> bool {
    return myos_bootstrap_object_kind(MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE)
            == MYOS_OBJECT_KIND_MEMORY
        && myos_bootstrap_object_kind(MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY)
            == MYOS_OBJECT_KIND_MEMORY
        && myos_bootstrap_object_kind(MYOS_BOOTSTRAP_CAP_STAGING_REGION)
            == MYOS_OBJECT_KIND_VSPACE;
}

auto accepts_boot_bundle_cross_validation() -> bool {
    uint8_t bundle_bytes[512]{};
    size_t bundle_size{};
    make_boot_bundle(bundle_bytes, bundle_size);
    const auto bundle = myos::boot::Bundle::parse(bundle_bytes, bundle_size);
    ManifestWorkspace workspace{};
    auto parsed = ManifestView::parse(kGolden, kGoldenSize, workspace);
    return bundle && parsed
        && parsed.value().validate_boot_bundle(bundle, workspace);
}

auto rejects_effective_stack_range() -> bool {
    uint8_t bundle_bytes[512]{};
    size_t bundle_size{};
    make_boot_bundle(bundle_bytes, bundle_size);
    const auto bundle = myos::boot::Bundle::parse(bundle_bytes, bundle_size);
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x2f0 + MYOS_DEPLOY_EXECUTION_STACK_TOP, 0x230000, 8);
    ManifestWorkspace workspace{};
    auto parsed = ManifestView::parse(bytes, sizeof(bytes), workspace);
    return bundle && parsed
        && !parsed.value().validate_boot_bundle(bundle, workspace);
}

auto rejects_truncation() -> bool {
    ManifestWorkspace workspace{};
    return !ManifestView::parse(kGolden, kGoldenSize - 1, workspace);
}

auto rejects_null() -> bool {
    ManifestWorkspace workspace{};
    return !ManifestView::parse(nullptr, kGoldenSize, workspace);
}

auto rejects_table_overlap() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, MYOS_DEPLOY_HEADER_TABLES + MYOS_DEPLOY_TABLE_DESC_SIZE,
        0xe0, 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_invalid_enum() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x1a0 + MYOS_DEPLOY_MAPPING_SOURCE, 9, 2);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_duplicate_key() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_OUTPUT_A,
        UINT64_C(0x0000000400000004), 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_numeric_key() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x1a0 + MYOS_DEPLOY_MAPPING_PRODUCED, 4, 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_noncanonical_empty_ref() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_OUTPUT_B,
        UINT64_C(0x0000000000000001), 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_invalid_positive_string_ref() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x1a0 + MYOS_DEPLOY_MAPPING_PRODUCED,
        UINT64_C(0x000000010000004f), 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto accepts_external_domain_key() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x2f0 + MYOS_DEPLOY_EXECUTION_DOMAIN,
        UINT64_C(0x0000000400000004), 8);
    ManifestWorkspace workspace{};
    return ManifestView::parse(bytes, sizeof(bytes), workspace).has_value();
}

auto rejects_execution_fault_policy() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x2f0 + MYOS_DEPLOY_EXECUTION_FAULT,
        MYOS_DEPLOY_EXECUTION_FAULT_ENDPOINT, 2);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_execution_terminal_policy() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x2f0 + MYOS_DEPLOY_EXECUTION_TERMINAL,
        MYOS_DEPLOY_EXECUTION_TERMINAL_ALL_EXIT, 2);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_endpoint_with_two_executions() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_two_execution_manifest(bytes, size);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_KIND_MASK,
        MYOS_RESOURCE_E4_KINDS, 8);
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_KIND,
        MYOS_OBJECT_KIND_ENDPOINT, 2);
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_FLAGS,
        MYOS_DEPLOY_OBJECT_POST_MAPPING, 2);
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_REF0, 1, 4);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, size, workspace);
}

auto rejects_missing_notification_relation() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_channel_manifest(bytes, size);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_KIND_MASK, MYOS_RESOURCE_E7_KINDS, 8);
    put(bytes, 0x480 + MYOS_DEPLOY_OBJECT_KIND,
        MYOS_OBJECT_KIND_PAGER, 2);
    put(bytes, 0x480 + MYOS_DEPLOY_OBJECT_FLAGS,
        MYOS_DEPLOY_OBJECT_EPHEMERAL_TASK, 2);
    put(bytes, 0x480 + MYOS_DEPLOY_OBJECT_ARG1, 1, 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, size, workspace);
}

auto rejects_multiple_notifications_relation() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_two_object_manifest(
        bytes, size, MYOS_OBJECT_KIND_NOTIFICATION,
        MYOS_DEPLOY_OBJECT_FLAG_NONE, "notify2", 7);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_KIND_MASK,
        MYOS_RESOURCE_E2_KINDS, 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, size, workspace);
}

auto accepts_shared_external_domain() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_two_execution_manifest(bytes, size);
    ManifestWorkspace workspace{};
    auto parsed = ManifestView::parse(bytes, size, workspace);
    return parsed && parsed.value().execution_count() == 2;
}

auto rejects_prepared_key_dangling_source() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x3c0 + MYOS_DEPLOY_EXPORT_SOURCE,
        UINT64_C(0x0000000900000046), 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_prepared_key_import_destination() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    /* The import destination is the distinct "import" key at string offset
     * 0x46.  PreparedKey sources are current-CSpace only and must reject it at
     * manifest admission rather than relying on construction lookup failure. */
    put(bytes, 0x3c0 + MYOS_DEPLOY_EXPORT_SOURCE,
        UINT64_C(0x0000000600000046), 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_prepared_key_kind_mismatch() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    /* The source key names the task's Thread, while this structurally valid
     * ceiling advertises a ResourcePool.  Manifest admission must reject the
     * namespace-kind mismatch before a DeploymentPlan is constructed. */
    put(bytes, 0x3c0 + MYOS_DEPLOY_EXPORT_CEILING
            + MYOS_DEPLOY_ATTENUATION_KIND,
        MYOS_OBJECT_KIND_RESOURCE_POOL, 2);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_kind_mask_denial() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_KIND_MASK, MYOS_RESOURCE_E1_KINDS, 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_notification_badge() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_ARG0, 0, 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_wx_mapping() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x1f0 + MYOS_DEPLOY_MAPPING_ACCESS,
        MYOS_VM_READ | MYOS_VM_WRITE | MYOS_VM_EXECUTE, 4);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_mapping_target_range() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x1f0 + MYOS_DEPLOY_MAPPING_ADDRESS,
        MYOS_RISCV64_LOWER_CANONICAL_END, 8);
    uint8_t bundle_bytes[512]{};
    size_t bundle_size{};
    make_boot_bundle(bundle_bytes, bundle_size);
    const auto bundle = myos::boot::Bundle::parse(bundle_bytes, bundle_size);
    ManifestWorkspace workspace{};
    auto parsed = ManifestView::parse(bytes, sizeof(bytes), workspace);
    return bundle && parsed
        && !parsed.value().validate_boot_bundle(bundle, workspace);
}

auto rejects_sc_configuration() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x2f0 + MYOS_DEPLOY_EXECUTION_SC_BUDGET, 0, 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_home_cpu() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x2f0 + MYOS_DEPLOY_EXECUTION_HOME_CPU,
        MYOS_DEPLOY_CPU_MAX, 4);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto accepts_duplicate_typed_kind() -> bool {
    constexpr uint16_t kinds[] = {
        MYOS_OBJECT_KIND_MEMORY, MYOS_OBJECT_KIND_VSPACE,
        MYOS_OBJECT_KIND_RESOURCE_POOL, MYOS_OBJECT_KIND_ENDPOINT,
        MYOS_OBJECT_KIND_CHANNEL, MYOS_OBJECT_KIND_PAGER,
    };
    for (const uint16_t kind : kinds) {
        uint8_t bytes[kGoldenSize]{};
        for (size_t index = 0; index < kGoldenSize; ++index) {
            bytes[index] = kGolden[index];
        }
        put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                + MYOS_DEPLOY_ATTENUATION_KIND,
            kind, 2);
        ManifestWorkspace workspace{};
        if (!ManifestView::parse(bytes, sizeof(bytes), workspace)) {
            return false;
        }
    }
    return true;
}

void make_typed_import(uint8_t* bytes, uint16_t kind) {
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_MODE,
        MYOS_DEPLOY_IMPORT_TYPED_DELEGATE, 2);
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_KIND,
        kind, 2);
}

auto accepts_typed_memory_schema() -> bool {
    uint8_t bytes[kGoldenSize]{};
    make_typed_import(bytes, MYOS_OBJECT_KIND_MEMORY);
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_WORD0,
        1, 8);
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_WORD1,
        2, 8);
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_WORD2,
        MYOS_VM_READ, 8);
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_WORD3,
        MYOS_VM_NORMAL, 8);
    ManifestWorkspace workspace{};
    return ManifestView::parse(bytes, sizeof(bytes), workspace).has_value();
}

auto accepts_typed_rwx_memory_vspace() -> bool {
    for (const uint16_t kind : {
             uint16_t{MYOS_OBJECT_KIND_MEMORY},
             uint16_t{MYOS_OBJECT_KIND_VSPACE}}) {
        uint8_t bytes[kGoldenSize]{ };
        make_typed_import(bytes, kind);
        put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                + MYOS_DEPLOY_ATTENUATION_WORD0,
            kind == MYOS_OBJECT_KIND_MEMORY ? 1 : 0x1000, 8);
        put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                + MYOS_DEPLOY_ATTENUATION_WORD1,
            kind == MYOS_OBJECT_KIND_MEMORY ? 2 : 0x2000, 8);
        put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                + MYOS_DEPLOY_ATTENUATION_WORD2,
            MYOS_VM_READ | MYOS_VM_WRITE | MYOS_VM_EXECUTE, 8);
        put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                + MYOS_DEPLOY_ATTENUATION_WORD3,
            MYOS_VM_NORMAL, 8);
        ManifestWorkspace workspace{};
        if (!ManifestView::parse(bytes, sizeof(bytes), workspace)) {
            return false;
        }
    }
    return true;
}

auto accepts_typed_vspace_schema() -> bool {
    uint8_t bytes[kGoldenSize]{};
    make_typed_import(bytes, MYOS_OBJECT_KIND_VSPACE);
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_WORD0,
        0x1000, 8);
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_WORD1,
        0x2000, 8);
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_WORD2,
        MYOS_VM_READ, 8);
    put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_WORD3,
        MYOS_VM_NORMAL, 8);
    ManifestWorkspace workspace{};
    return ManifestView::parse(bytes, sizeof(bytes), workspace).has_value();
}

auto accepts_typed_zero_resource_budget() -> bool {
    uint8_t bytes[kGoldenSize]{};
    make_typed_import(bytes, MYOS_OBJECT_KIND_RESOURCE_POOL);
    ManifestWorkspace workspace{};
    return ManifestView::parse(bytes, sizeof(bytes), workspace).has_value();
}

auto accepts_typed_channel_forms() -> bool {
    for (const uint64_t fixed : {uint64_t{0}, UINT64_MAX}) {
        uint8_t bytes[kGoldenSize]{};
        make_typed_import(bytes, MYOS_OBJECT_KIND_CHANNEL);
        put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                + MYOS_DEPLOY_ATTENUATION_WORD0,
            MYOS_CAP_CHANNEL_SIDE_A, 8);
        put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                + MYOS_DEPLOY_ATTENUATION_WORD1,
            fixed == 0 ? 0 : 7, 8);
        put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                + MYOS_DEPLOY_ATTENUATION_WORD2,
            fixed, 8);
        ManifestWorkspace workspace{};
        if (!ManifestView::parse(bytes, sizeof(bytes), workspace)) {
            return false;
        }
    }
    return true;
}

auto rejects_typed_schema_mutations() -> bool {
    struct Mutation final {
        uint16_t kind;
        size_t word;
        uint64_t value;
    };
    const Mutation mutations[] = {
        {MYOS_OBJECT_KIND_TUNNEL, 0, 0},
        {MYOS_OBJECT_KIND_MEMORY, 3, 0},
        {MYOS_OBJECT_KIND_MEMORY, 2, MYOS_VM_WRITE},
        {MYOS_OBJECT_KIND_MEMORY, 2, MYOS_VM_READ | (uint64_t{1} << 8)},
        {MYOS_OBJECT_KIND_VSPACE, 0, 0x1001},
        {MYOS_OBJECT_KIND_VSPACE, 1, 0x1001},
        {MYOS_OBJECT_KIND_VSPACE, 0, UINT64_MAX - 0xfff},
        {MYOS_OBJECT_KIND_CHANNEL, 1, 1},
    };
    for (const Mutation mutation : mutations) {
        uint8_t bytes[kGoldenSize]{};
        make_typed_import(bytes, mutation.kind);
        if (mutation.kind == MYOS_OBJECT_KIND_MEMORY) {
            put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                    + MYOS_DEPLOY_ATTENUATION_WORD1,
                1, 8);
            put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                    + MYOS_DEPLOY_ATTENUATION_WORD2,
                MYOS_VM_READ, 8);
            put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                    + MYOS_DEPLOY_ATTENUATION_WORD3,
                MYOS_VM_NORMAL, 8);
        } else if (mutation.kind == MYOS_OBJECT_KIND_VSPACE) {
            put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                    + MYOS_DEPLOY_ATTENUATION_WORD1,
                0x1000, 8);
            put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                    + MYOS_DEPLOY_ATTENUATION_WORD2,
                MYOS_VM_READ, 8);
            put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                    + MYOS_DEPLOY_ATTENUATION_WORD3,
                MYOS_VM_NORMAL, 8);
        } else if (mutation.kind == MYOS_OBJECT_KIND_CHANNEL) {
            put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                    + MYOS_DEPLOY_ATTENUATION_WORD0,
                MYOS_CAP_CHANNEL_SIDE_A, 8);
            put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                    + MYOS_DEPLOY_ATTENUATION_WORD1,
                0, 8);
            put(bytes, 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
                    + MYOS_DEPLOY_ATTENUATION_WORD2,
                0, 8);
        }
        const size_t base = 0x360 + MYOS_DEPLOY_IMPORT_ATTENUATION
            + MYOS_DEPLOY_ATTENUATION_WORD0 + mutation.word * 8;
        put(bytes, base, mutation.value, 8);
        ManifestWorkspace workspace{};
        if (ManifestView::parse(bytes, sizeof(bytes), workspace)) {
            return false;
        }
    }
    return true;
}

auto accepts_channel_schema() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_channel_manifest(bytes, size);
    ManifestWorkspace workspace{};
    auto parsed = ManifestView::parse(bytes, size, workspace);
    return parsed.has_value();
}

auto rejects_channel_zero_scalars() -> bool {
    for (const size_t field : {MYOS_DEPLOY_OBJECT_ARG1,
                               MYOS_DEPLOY_OBJECT_ARG3}) {
        uint8_t bytes[1400]{};
        size_t size{};
        make_channel_manifest(bytes, size);
        put(bytes, 0x4e0 + field, 0, 8);
        ManifestWorkspace workspace{};
        if (ManifestView::parse(bytes, size, workspace)) {
            return false;
        }
    }
    return true;
}

auto rejects_duplicate_channel_b() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_KIND_MASK,
        MYOS_RESOURCE_E6_KINDS, 8);
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_OUTPUT_B,
        UINT64_C(0x0000000600000026), 8);
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_KIND,
        MYOS_OBJECT_KIND_CHANNEL, 2);
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_ARG0, 1, 8);
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_ARG1, 1, 8);
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_ARG3, 1, 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto accepts_prepared_channel_b() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_channel_manifest(bytes, size);
    put(bytes, 0x3c0 + MYOS_DEPLOY_EXPORT_SOURCE,
        UINT64_C(0x0000000900000046), 8);
    put(bytes, 0x3c0 + MYOS_DEPLOY_EXPORT_CEILING
            + MYOS_DEPLOY_ATTENUATION_KIND,
        MYOS_OBJECT_KIND_CHANNEL, 2);
    ManifestWorkspace workspace{};
    return ManifestView::parse(bytes, size, workspace).has_value();
}

auto accepts_prepared_sc() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x3c0 + MYOS_DEPLOY_EXPORT_SOURCE,
        UINT64_C(0x0000000200000032), 8);
    put(bytes, 0x3c0 + MYOS_DEPLOY_EXPORT_CEILING
            + MYOS_DEPLOY_ATTENUATION_KIND,
        MYOS_OBJECT_KIND_SCHED_CONTEXT, 2);
    ManifestWorkspace workspace{};
    return ManifestView::parse(bytes, sizeof(bytes), workspace).has_value();
}

auto accepts_pager_schema() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_pager_manifest(bytes, size);
    ManifestWorkspace workspace{};
    return ManifestView::parse(bytes, size, workspace).has_value();
}

auto accepts_endpoint_schema() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_endpoint_manifest(bytes, size);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_KIND_MASK, MYOS_RESOURCE_E4_KINDS, 8);
    ManifestWorkspace workspace{};
    return ManifestView::parse(bytes, size, workspace).has_value();
}

auto rejects_endpoint_nonresident_source() -> bool {
    uint8_t bundle_bytes[512]{};
    size_t bundle_size{};
    make_boot_bundle(bundle_bytes, bundle_size);
    const auto bundle = myos::boot::Bundle::parse(bundle_bytes, bundle_size);
    uint8_t bytes[1400]{};
    size_t size{};
    make_endpoint_manifest(bytes, size);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_KIND_MASK,
        MYOS_RESOURCE_E4_KINDS, 8);
    put(bytes, 0x4e0 + MYOS_DEPLOY_OBJECT_REF0, 0, 4);
    ManifestWorkspace workspace{};
    auto parsed = ManifestView::parse(bytes, size, workspace);
    return bundle && parsed
        && !parsed.value().validate_boot_bundle(bundle, workspace);
}

auto rejects_dangling_object_ref() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x290 + MYOS_DEPLOY_OBJECT_REF0, 2, 4);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_critical_overflow() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_CRITICAL_BYTES, 4096, 8);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_nul_string() -> bool {
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    bytes[0x420] = 0;
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_dependency_cycle() -> bool {
    uint8_t bytes[1200]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, MYOS_DEPLOY_HEADER_TOTAL_SIZE, sizeof(bytes), 8);
    put(bytes, MYOS_DEPLOY_HEADER_TABLES
            + MYOS_DEPLOY_TABLE_DEPENDENCY * MYOS_DEPLOY_TABLE_DESC_SIZE,
        0x458, 8);
    put(bytes, MYOS_DEPLOY_HEADER_TABLES
        + MYOS_DEPLOY_TABLE_DEPENDENCY * MYOS_DEPLOY_TABLE_DESC_SIZE + 8,
        1, 4);
    put(bytes, 0xe0 + MYOS_DEPLOY_TASK_DEPENDENCY_COUNT, 1, 4);
    put(bytes, 0x458 + MYOS_DEPLOY_DEPENDENCY_TARGET, 0, 4);
    put(bytes, 0x458 + MYOS_DEPLOY_DEPENDENCY_KIND,
        MYOS_DEPLOY_DEPENDENCY_REQUIRED, 2);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, sizeof(bytes), workspace);
}

auto rejects_two_task_required_cycle() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_two_task_manifest(bytes, size, false);
    ManifestWorkspace workspace{};
    return !ManifestView::parse(bytes, size, workspace);
}

auto accepts_optional_dependency_edge() -> bool {
    uint8_t bytes[1400]{};
    size_t size{};
    make_two_task_manifest(bytes, size, true);
    ManifestWorkspace workspace{};
    auto parsed = ManifestView::parse(bytes, size, workspace);
    return parsed.has_value();
}

auto rejects_boot_bundle_alignment() -> bool {
    uint8_t bytes[512]{};
    size_t size{};
    make_boot_bundle(bytes, size);
    put(bytes, MYOS_BOOT_HEADER_SIZE + 32, 0x201000, 8);
    put(bytes, MYOS_BOOT_HEADER_SIZE + MYOS_BOOT_MODULE_SIZE,
        0x201000, 8);
    put(bytes, MYOS_BOOT_HEADER_SIZE + MYOS_BOOT_MODULE_SIZE + 32,
        0x2000, 8);
    return !myos::boot::Bundle::parse(bytes, size);
}

auto rejects_boot_alignment_upper_bound() -> bool {
    uint8_t bytes[512]{};
    size_t size{};
    make_boot_bundle(bytes, size);
    put(bytes, MYOS_BOOT_HEADER_SIZE + MYOS_BOOT_MODULE_SIZE + 32,
        MYOS_RISCV64_LOWER_CANONICAL_END << 1, 8);
    return !myos::boot::Bundle::parse(bytes, size);
}

auto accepts_entry_zero_fallback() -> bool {
    uint8_t bundle_bytes[512]{};
    size_t bundle_size{};
    make_boot_bundle(bundle_bytes, bundle_size);
    const auto bundle = myos::boot::Bundle::parse(bundle_bytes, bundle_size);
    uint8_t bytes[kGoldenSize]{};
    for (size_t index = 0; index < kGoldenSize; ++index) {
        bytes[index] = kGolden[index];
    }
    put(bytes, 0x2f0 + MYOS_DEPLOY_EXECUTION_ENTRY, 0, 8);
    ManifestWorkspace workspace{};
    auto parsed = ManifestView::parse(bytes, sizeof(bytes), workspace);
    return bundle && parsed
        && parsed.value().validate_boot_bundle(bundle, workspace);
}

} // namespace

int main(int argc, char** argv) {
    const bool have_fixture = argc >= 2;
    const bool have_production = argc >= 3;
    bool result = (argc == 1 || (have_fixture && matches_file(argv[1])))
        && (!have_production || load_production_manifest(argv[2]));
    const auto run = [&](const char* name, bool value) {
        if (!value) {
            fprintf(stderr, "failed %s\n", name);
        }
        return value;
    };
    result = result && run("golden", accepts_golden())
            && (!have_production
                || run("production-shape", accepts_production_shape()))
            && (!have_production
                || run("production-authority-budget",
                       accepts_production_authority_budget()))
            && (!have_production
                || run("minor-shape-hybrids", rejects_minor_shape_hybrids()))
            && (!have_production
                || run("task-key-kind-relabel",
                       rejects_task_key_kind_relabel()))
            && (!have_production
                || run("bootstrap-kind-relabel",
                       rejects_bootstrap_kind_relabel()))
            && (!have_production
                || run("bootstrap-kind-map",
                       accepts_closed_bootstrap_kind_mapping()))
            && run("bundle", accepts_boot_bundle_cross_validation())
            && run("stack", rejects_effective_stack_range())
            && run("trunc", rejects_truncation())
            && run("null", rejects_null())
            && run("overlap", rejects_table_overlap())
            && run("enum", rejects_invalid_enum())
            && run("dup", rejects_duplicate_key())
            && run("numeric", rejects_numeric_key())
            && run("mask", rejects_kind_mask_denial())
            && run("badge", rejects_notification_badge())
            && run("empty-ref", rejects_noncanonical_empty_ref())
            && run("channel", accepts_channel_schema())
            && run("channel-zero", rejects_channel_zero_scalars())
            && run("channel-b-duplicate", rejects_duplicate_channel_b())
            && run("prepared-channel-b", accepts_prepared_channel_b())
            && run("prepared-sc", accepts_prepared_sc())
            && run("pager", accepts_pager_schema())
            && run("endpoint", accepts_endpoint_schema())
            && run("endpoint-policy", rejects_endpoint_nonresident_source())
            && run("objectref", rejects_dangling_object_ref())
            && run("critical", rejects_critical_overflow())
            && run("nul", rejects_nul_string())
            && run("cycle", rejects_dependency_cycle())
            && run("string-ref", rejects_invalid_positive_string_ref())
            && run("external-domain", accepts_external_domain_key())
            && run("execution-fault-policy", rejects_execution_fault_policy())
            && run("execution-terminal-policy",
                   rejects_execution_terminal_policy())
            && run("endpoint-two-executions",
                   rejects_endpoint_with_two_executions())
            && run("missing-notification-relation",
                   rejects_missing_notification_relation())
            && run("multiple-notifications-relation",
                   rejects_multiple_notifications_relation())
            && run("shared-domain", accepts_shared_external_domain())
            && run("prepared-source", rejects_prepared_key_dangling_source())
            && run("prepared-import-source",
                   rejects_prepared_key_import_destination())
            && run("prepared-kind-mismatch",
                   rejects_prepared_key_kind_mismatch())
            && run("wx", rejects_wx_mapping())
            && run("target-range", rejects_mapping_target_range())
            && run("sc", rejects_sc_configuration())
            && run("home-cpu", rejects_home_cpu())
            && run("duplicate-typed", accepts_duplicate_typed_kind())
            && run("typed-memory", accepts_typed_memory_schema())
            && run("typed-rwx", accepts_typed_rwx_memory_vspace())
            && run("typed-vspace", accepts_typed_vspace_schema())
            && run("typed-pool-zero", accepts_typed_zero_resource_budget())
            && run("typed-channel", accepts_typed_channel_forms())
            && run("typed-schema", rejects_typed_schema_mutations())
            && run("two-task-cycle", rejects_two_task_required_cycle())
            && run("optional-edge", accepts_optional_dependency_edge())
            && run("boot-alignment", rejects_boot_bundle_alignment())
            && run("boot-alignment-upper", rejects_boot_alignment_upper_bound())
            && run("entry-zero", accepts_entry_zero_fallback());
    return result ? 0 : 1;
}
