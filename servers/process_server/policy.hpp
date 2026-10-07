#pragma once

#include <servers/deploy/detail/plan.hpp>
#include <sys/start.hpp>
#include <servers/runtime/service.hpp>

namespace sys::process {

inline auto named(deploy::ByteView value, const char* name) noexcept -> bool {
    return value.equals({reinterpret_cast<const uint8_t*>(name), service::length(name)});
}

// The scheduling domain is an input to construction, not an application
// import. Only the explicitly allowed service contracts may cross that boundary.
inline auto admit(const deploy::TaskPlanView& task, deploy::ByteView package) noexcept -> bool {
    const auto& row = *task.row();
    if (row.executions.count != 1 || task.execution(0)->model != DEPLOY_EXECUTION_THREAD
        || row.images.count != 1 || row.exports.count != 0 || row.dependencies.count != 0
        || row.pool_memory > 8 * 1024 * 1024 || row.pool_caps > 256
        || (row.kind_mask & ~(DEPLOY_BASE_KINDS | OBJ_BIT(OBJECT_KIND_CHANNEL))) != 0)
        return false;
    for (uint32_t i = 0; i < row.objects.count; ++i)
        if (task.object(i)->kind != OBJECT_KIND_NOTIFICATION) return false;
    for (uint32_t i = 0; i < row.mappings.count; ++i)
        if (task.mapping(i)->source != DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
            && task.mapping(i)->source != DEPLOY_MAPPING_SOURCE_ZERO) return false;
    for (uint32_t i = 0; i < row.imports.count; ++i) {
        const auto& imported = *task.import(i);
        const deploy::PlanBootstrap* binding{};
        for (uint32_t b = 0; b < row.bootstraps.count; ++b) {
            if (task.bootstrap(b)->destination != imported.destination) continue;
            if (binding != nullptr) return false;
            binding = task.bootstrap(b);
        }
        if (binding == nullptr) return false;
        if (binding->kind == 0) {
            const auto name = task.symbol(binding->name);
            const boot::Import* contract{};
            const char* source{};
            word_t rights = RIGHT_SEND;
            if (named(name, boot::Vfs.name)) {
                contract = &boot::Vfs; source = "vfs.directory";
            } else if (named(name, boot::VfsRead.name)) {
                contract = &boot::VfsRead; source = "vfs.read.directory";
            } else if (named(name, boot::StoreAdmin.name)
                && named(package, "mkfs")) {
                // package is the trusted boot-disk lookup selected by argv[0].
                contract = &boot::StoreAdmin; source = "store.admin.directory";
            } else if (named(name, boot::Stdin.name)) {
                contract = &boot::Stdin; source = "stdin"; rights = RIGHT_RECEIVE;
            } else if (named(name, boot::Stdout.name)) {
                contract = &boot::Stdout; source = "stdout";
            } else if (named(name, boot::Stderr.name)) {
                contract = &boot::Stderr; source = "stderr";
            }
            if (contract == nullptr || binding->protocol != contract->protocol || binding->major != contract->major
                || binding->object_kind != contract->kind
                || imported.source_class != DEPLOY_IMPORT_SOURCE_AUTHORITY
                || !named(task.symbol(imported.source), source) || imported.attenuation.rights != rights)
                return false;
            continue;
        }
        if (imported.source_class != DEPLOY_IMPORT_SOURCE_TASK_KEY) return false;
        const auto source = imported.source;
        word_t rights{};
        switch (binding->kind) {
        case BOOT_POOL:
            if (source != row.pool_key) return false;
            rights = RIGHT_CREATE;
            break;
        case BOOT_VSPACE:
            if (source != row.vspace_key) return false;
            rights = RIGHT_DELEGATE | RIGHT_MAP | RIGHT_PROTECT
                | RIGHT_UNMAP | RIGHT_DESTROY;
            break;
        case BOOT_CSPACE:
            if (source != row.cspace_key) return false;
            rights = RIGHT_MANAGE;
            break;
        case BOOT_EVENTS:
            rights = RIGHT_SIGNAL | RIGHT_RECEIVE | RIGHT_DUPLICATE;
            break;
        default: return false;
        }
        if ((imported.attenuation.rights & ~rights) != 0) return false;
    }
    return true;
}
} // namespace sys::process
