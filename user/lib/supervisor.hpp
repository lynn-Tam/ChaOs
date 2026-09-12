#pragma once

#include <user/lib/deployment_syscall.hpp>
#include <user/lib/task_supervision.hpp>
#include <user/lib/service.hpp>

namespace myos::deploy {

// A supervisor owns one plan, its construction sources and its TaskTable.
// Handles only carry the table identity and the unique completion receiver.
// All status/lifecycle decisions read TaskTable's canonical observations.
template<size_t Capacity, size_t AuthorityCapacity = 16>
class Supervisor final {
    using Backend = cap::SyscallBackend;
    using Space = TaskSpace<kTaskLocalCapacity, kTaskImportRemoteCapacity, Backend>;
    using Completions = CompletionSet<Capacity>;
    using Table = TaskTable<TaskRecord<Space>, Completions, Capacity>;
    using Builder = TaskBuilder<Table, Completions>;
    using Authorities = AuthoritySet<AuthorityCapacity>;

    struct Source final { ByteView name{}; AuthorityId authority{}; };
    cap::CapRef pool_{};
    uint32_t cpus_{};
    cap::MappedBundle bundle_{};
    cap::ScratchWindow scratch_{};
    Authorities authorities_{};
    RegistrationJournal<AuthorityCapacity> journal_{};
    Source sources_[AuthorityCapacity]{};
    size_t source_count_{};
    PlanSet<1> plans_{};
    ManifestWorkspace manifest_workspace_{};
    DeploymentPlan plan_{};
    Completions completions_{};
    Table table_{};
    TaskConstructionWorkspace<Authorities> workspace_{};

    auto source(ByteView name) const noexcept -> AuthorityId {
        for (size_t i = 0; i < source_count_; ++i)
            if (sources_[i].name.equals(name)) return sources_[i].authority;
        return {};
    }
    static void checked(bool result) noexcept {
        if (!result) myos::exit(MYOS_STATUS_INTERNAL);
    }

public:
    struct Handle final {
        TaskId id{};
        libk::optional<typename Completions::Receiver> receiver{};
        auto token() const noexcept -> uint64_t {
            return (uint64_t{id.generation} << 32) | id.slot;
        }
    };

    static auto name(const char* text) noexcept -> ByteView {
        return {reinterpret_cast<const uint8_t*>(text), service::length(text)};
    }
    auto open(const bootstrap::BootstrapView& info) noexcept -> myos_status_t {
        return open(info, service::capability(info, MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE), info.bundle_size());
    }
    // Replacing a package preserves TaskTable generations and registered
    // authority ceilings. No outstanding task may borrow the old plan/image.
    auto unload() noexcept -> myos_status_t {
        if (!table_.empty()) return MYOS_STATUS_BUSY;
        plan_ = {};
        auto status = scratch_.close();
        if (status != MYOS_STATUS_OK) return status;
        return bundle_.close();
    }
    auto open(const bootstrap::BootstrapView& info, myos_cap_t package, size_t package_size) noexcept
        -> myos_status_t {
        const auto unloaded = unload();
        if (unloaded != MYOS_STATUS_OK) return unloaded;
        pool_ = {service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL), 0};
        cpus_ = info.cpu_count();
        const cap::CapRef vspace{service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE), 0};
        const cap::CapRef bundle{package, 0};
        const auto size = Window::round_size(package_size);
        auto status = bundle_.open(vspace, bundle, Window{0x10000000, size}, package_size);
        if (status != MYOS_STATUS_OK) return status;
        if (!supervision::decode_plan(bundle_, manifest_workspace_, plans_, plan_, 0))
            return MYOS_STATUS_BAD_ARGS;
        myos_word_t scratch_size = 0;
        auto lease = plan_.lease();
        if (!lease) return MYOS_STATUS_INTERNAL;
        for (uint32_t i = 0; i < plan_.task_count(); ++i) {
            auto required = required_scratch_size(lease->task(i), *bundle_.view());
            if (!required) return MYOS_STATUS_BAD_ARGS;
            if (*required > scratch_size) scratch_size = *required;
        }
        return scratch_.open(vspace, Window{0x18000000, scratch_size}, Window{0x10000000, size});
    }
    auto add(const char* label, myos_cap_t cap, uint16_t kind, uint64_t rights,
             uint64_t first = 0, uint64_t count = 0,
             uint64_t access = 0, uint64_t types = 0) noexcept -> myos_status_t {
        if (source_count_ == AuthorityCapacity || cap == 0 || source(name(label)).valid())
            return MYOS_STATUS_BAD_ARGS;
        const myos_cap_attenuation ceiling{
            .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
            .kind = kind, .size = MYOS_CAP_ATTENUATION_SIZE,
            .rights = rights, .words = {first, count, access, types}};
        auto id = journal_.register_source(authorities_, {cap, 0}, source_count_ + 1, ceiling);
        if (!id) return MYOS_STATUS_DENIED;
        sources_[source_count_++] = {name(label), *id};
        return MYOS_STATUS_OK;
    }
    auto add_boot_sources(const bootstrap::BootstrapView& info) noexcept -> myos_status_t {
        auto status = add("domain", service::capability(info, MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN),
                          MYOS_OBJECT_KIND_SCHED_DOMAIN, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONTROL);
        if (status != MYOS_STATUS_OK) return status;
        return add("bundle", service::capability(info, MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE),
                   MYOS_OBJECT_KIND_MEMORY, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_MAP | MYOS_RIGHT_INSPECT,
                   0, Window::round_size(info.bundle_size()) / 4096, MYOS_VM_READ, MYOS_VM_NORMAL);
    }
    auto launch(ByteView name, myos_status_t& status) noexcept -> libk::optional<Handle> {
        status = MYOS_STATUS_BAD_ARGS;
        const auto index = plan_.find_task(name);
        auto lease = plan_.lease();
        if (!index || !lease) return libk::nullopt;
        auto task = lease->task(*index);
        TaskAuthorityBindings bindings{};
        for (uint32_t i = 0; i < task.row()->executions.count; ++i)
            bindings.domains[i] = source(task.symbol(task.execution(i)->domain));
        for (uint32_t i = 0; i < task.row()->imports.count; ++i) {
            const auto& import = *task.import(i);
            if (import.source_class == MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY) {
                bindings.imports[i] = source(task.symbol(import.source));
                if (!bindings.imports[i].valid()) { status = MYOS_STATUS_DENIED; return libk::nullopt; }
            }
        }
        auto pending = Builder::begin(completions_, table_, libk::move(*lease), *index);
        if (!pending) { status = MYOS_STATUS_BUSY; return libk::nullopt; }
        auto builder = libk::move(*pending);
        Handle handle{builder.record()->id(), builder.take_receiver()};
        TaskConstructionInput<Backend, Authorities> input{
            .parent_pool = pool_, .bundle = &bundle_, .scratch = &scratch_,
            .runtime_cpu_count = cpus_, .bindings = &bindings, .workspace = workspace_};
        status = builder.construct(input, authorities_);
        if (status == MYOS_STATUS_OK && !builder.commit_prepared()) status = MYOS_STATUS_INTERNAL;
        if (status != MYOS_STATUS_OK) {
            checked(supervision::close_failed(table_, handle.id, status, builder, handle.receiver));
            return libk::nullopt;
        }
        status = table_.start(handle.id);
        if (status != MYOS_STATUS_OK) {
            checked(table_.begin_close(handle.id, CloseReason::ConstructionFailure, status));
            checked(supervision::take_completion(table_, handle.id, CloseReason::ConstructionFailure, status, handle.receiver));
            return libk::nullopt;
        }
        return handle;
    }
    auto launch(const char* text, myos_status_t& status) noexcept -> libk::optional<Handle> {
        return launch(name(text), status);
    }
    auto wait(Handle& handle) noexcept -> myos_status_t {
        myos_status_t terminal = MYOS_STATUS_INTERNAL;
        checked(supervision::observe_and_close(table_, handle.id, handle.receiver, terminal));
        return terminal;
    }
    auto stop(Handle& handle) noexcept -> myos_status_t {
        checked(table_.begin_close(handle.id, CloseReason::Explicit, MYOS_STATUS_CANCELED));
        checked(supervision::take_completion(table_, handle.id, CloseReason::Explicit,
                                            MYOS_STATUS_CANCELED, handle.receiver));
        return MYOS_STATUS_CANCELED;
    }
    auto observe(const Handle& handle) noexcept -> SysResult { return table_.observe_terminal(handle.id); }

};

} // namespace myos::deploy
