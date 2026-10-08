#pragma once

#include <concepts>
#include <optional>
#include <servers/deploy/bundle.hpp>
#include <servers/deploy/format.hpp>

namespace deploy {

// Borrowed for one synchronous construction. The caller owns the selector.
struct CapSrc final {
    sys::cap::CapRef cap{};
    CapView limit{};
    auto valid() const noexcept -> bool {
        return cap && cap.cspace == 0
            && attenuation::valid_descriptor(limit, attenuation::DescriptorForm::Ceiling);
    }
};

inline constexpr size_t kImportBatchMax =
    DEPLOY_TASK_IMPORT_MAX < 32U ? DEPLOY_TASK_IMPORT_MAX : 32U;

// Upper bound on simultaneously owned construction capabilities. Closed
// entries are reusable; no slot identities or cumulative tombstones exist.
inline constexpr size_t kTaskLocalCapacity =
    1U + 2U * DEPLOY_TASK_MAPPING_MAX
    + 2U * DEPLOY_TASK_OBJECT_MAX
    + 3U * DEPLOY_TASK_EXECUTION_MAX + 1U;

// Installation is synchronous. The task immediately owns every returned child
// selector, so any later failure follows the same group close path.
struct ImportBinding final {
    CapSrc borrowed{};
    sys::cap::CapRef descriptor{};
    word_t descriptor_offset{};
};

template<typename Task, size_t BatchMax = 32>
struct ImportTransaction final {
    using B = typename Task::backend_type;
    using Owner = typename Task::owner_type;

    static auto run(Task& task, const TaskSpec& spec, uint32_t first, uint32_t count,
                    const ImportBinding* bindings, sys::cap::CapRef* output) noexcept -> status_t {
        if (count > BatchMax || !spec.valid() || first > spec.row()->import_count
            || count > spec.row()->import_count - first || (count && (!bindings || !output)))
            return STATUS_BAD_ARGS;
        for (uint32_t i = 0; i < count; ++i) {
            const auto row = *spec.import(first + i);
            const auto& src = bindings[i];
            if (!src.borrowed.cap || src.borrowed.cap.cspace != 0 || row.mode == DEPLOY_IMPORT_MOVE)
                return STATUS_BAD_ARGS;
            if (row.source_class == DEPLOY_IMPORT_SOURCE_AUTHORITY
                && (!src.borrowed.valid() || !attenuation::within(row.attenuation, src.borrowed.limit, row.mode)))
                return STATUS_DENIED;
            if (row.mode == DEPLOY_IMPORT_TYPED_DELEGATE
                && (!src.descriptor || src.descriptor.cspace != 0
                    || src.descriptor_offset % CAP_ATTENUATION_SIZE
                    || src.descriptor_offset > DEPLOY_PAGE_SIZE - CAP_ATTENUATION_SIZE))
                return STATUS_BAD_ARGS;
        }
        // A failed batch closes only capabilities it created. Failed closes
        // stay owned by the task and are retried by its normal teardown.
        uint32_t installed{};
        const auto fail = [&](status_t status) {
            while (installed) {
                const auto result = task.drop(output[--installed]);
                if (result != STATUS_OK) status = result;
                output[installed] = {};
            }
            return status;
        };
        for (uint32_t i = 0; i < count; ++i) {
            const auto row = *spec.import(first + i);
            const auto& src = bindings[i];
            sys::SysResult result{};
            switch (row.mode) {
            case DEPLOY_IMPORT_DUPLICATE:
                result = B::duplicate(src.borrowed.cap, task.cspace(), row.attenuation.rights);
                break;
            case DEPLOY_IMPORT_TYPED_DELEGATE:
                result = B::typed_delegate(src.borrowed.cap, task.cspace(), src.descriptor, src.descriptor_offset);
                break;
            case DEPLOY_IMPORT_CHANNEL_MINT:
                result = B::channel_mint(src.borrowed.cap, task.cspace(), row.attenuation.words[1], row.attenuation.rights);
                break;
            default: return fail(STATUS_BAD_ARGS);
            }
            Owner cap{{result.value, task.cspace().selector}};
            if (result.status != STATUS_OK || !cap)
                return fail(result.status == STATUS_OK ? STATUS_INVALID_CAP : result.status);
            const auto saved = task.keep(std::move(cap));
            if (!saved) return fail(STATUS_NO_MEMORY);
            output[installed++] = *saved;
        }
        return STATUS_OK;
    }
};

} // namespace deploy
