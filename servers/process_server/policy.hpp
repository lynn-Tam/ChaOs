#pragma once

#include <user/lib/deployment_plan.hpp>
#include <user/lib/imports.hpp>
#include <user/lib/service.hpp>

namespace myos::process {

inline auto named(deploy::ByteView value, const char* name) noexcept -> bool {
    return value.equals({reinterpret_cast<const uint8_t*>(name), service::length(name)});
}

// The scheduling domain is an input to construction, not an application
// import. Only the explicitly allowed service contracts may cross that boundary.
inline auto admit(const deploy::TaskPlanView& task) noexcept -> bool {
    const auto& row = *task.row();
    if (row.executions.count != 1 || task.execution(0)->model != MYOS_DEPLOY_EXECUTION_THREAD
        || row.images.count != 1 || row.exports.count != 0 || row.dependencies.count != 0
        || row.pool_memory > 8 * 1024 * 1024 || row.pool_caps > 256
        || (row.kind_mask & ~(MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_CHANNEL)) != 0)
        return false;
    for (uint32_t i = 0; i < row.objects.count; ++i)
        if (task.object(i)->kind != MYOS_OBJECT_KIND_NOTIFICATION) return false;
    for (uint32_t i = 0; i < row.mappings.count; ++i)
        if (task.mapping(i)->source != MYOS_DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
            && task.mapping(i)->source != MYOS_DEPLOY_MAPPING_SOURCE_ZERO) return false;
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
            const bool file = named(task.symbol(binding->name), bootstrap::imports::Files.name);
            const auto contract = file ? bootstrap::imports::Files : bootstrap::imports::ConsoleOutput;
            if (!named(task.symbol(binding->name), contract.name) || binding->protocol != contract.protocol
                || binding->major != contract.major || binding->object_kind != contract.kind
                || imported.source_class != MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY
                || !named(task.symbol(imported.source), file ? "files.directory" : "console.sender")
                || imported.attenuation.rights != MYOS_RIGHT_SEND) return false;
            continue;
        }
        if (imported.source_class != MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY) return false;
        const auto source = imported.source;
        myos_word_t rights{};
        switch (binding->kind) {
        case MYOS_BOOTSTRAP_CAP_RESOURCE_POOL:
            if (source != row.pool_key) return false;
            rights = MYOS_RIGHT_CREATE;
            break;
        case MYOS_BOOTSTRAP_CAP_VSPACE:
            if (source != row.vspace_key) return false;
            rights = MYOS_RIGHT_CREATE_REGION | MYOS_RIGHT_MAP | MYOS_RIGHT_PROTECT
                | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY;
            break;
        case MYOS_BOOTSTRAP_CAP_CSPACE:
            if (source != row.cspace_key) return false;
            rights = MYOS_RIGHT_MANAGE;
            break;
        case MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION:
            rights = MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_DUPLICATE;
            break;
        default: return false;
        }
        if ((imported.attenuation.rights & ~rights) != 0) return false;
    }
    return true;
}
} // namespace myos::process
