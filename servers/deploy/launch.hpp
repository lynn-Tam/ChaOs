#pragma once

#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <libk/span.hpp>
#include <user/abi/objects.hpp>
#include <servers/deploy/detail/space.hpp>
#include <servers/deploy/detail/task.hpp>
#include <user/server_rt/service.hpp>

namespace deploy {

struct source final {
    const char* name{};
    myos::cap::CapRef cap{};
    myos_cap_attenuation ceiling{};
};

struct options final {
    ImageSource image_source{};
    const myos::bootstrap::Arguments* arguments{};
    myos_cap_t terminal_events{};
    myos_word_t close_badge{};
    libk::Span<const source> sources{};
    bool (*admit)(const TaskPlanView&, ByteView) noexcept{};
};

// program owns the bytes borrowed by its immutable plan and live tasks.
// Stable storage is required because PlanLease refers to PlanSet's control.
class program final : private libk::noncopyable_nonmovable {
    template<size_t, size_t> friend class tasks;
    MappedBundle<> bundle_{};
    ScratchWindow<> scratch_{};
    PlanSet<1> plans_{};
    DeploymentPlan plan_{};
public:
    auto plan() const noexcept -> const DeploymentPlan& { return plan_; }
    auto close() noexcept -> myos_status_t {
        if (plan_.borrowed()) return MYOS_STATUS_BUSY;
        plan_ = {};
        const auto status = scratch_.close();
        return status == MYOS_STATUS_OK ? bundle_.close() : status;
    }

};

// Only TaskTable owns task state/generations. CompletionSet retains each final
// result after resources close, until its unique receiver consumes or detaches.
template<size_t Capacity, size_t AuthorityCapacity = 16>
class tasks final {
    using Backend = myos::cap::SyscallBackend;
    using Space = TaskSpace<kTaskLocalCapacity, kTaskImportRemoteCapacity, Backend>;
    using Completions = CompletionSet<Capacity>;
    using Table = TaskTable<TaskRecord<Space>, Completions, Capacity>;
    using Builder = TaskBuilder<Table, Completions>;
    using Authorities = AuthoritySet<AuthorityCapacity>;

    struct Source final { ByteView name{}; AuthorityId authority{}; };
    myos::cap::CapRef pool_{};
    uint32_t cpus_{};
    Authorities authorities_{};
    RegistrationJournal<AuthorityCapacity> journal_{};
    Source sources_[AuthorityCapacity]{};
    size_t source_count_{};
    ManifestWorkspace manifest_workspace_{};
    Completions completions_{};
    Table table_{};
    TaskConstructionWorkspace<Authorities> workspace_{};

    auto source(ByteView name) const noexcept -> AuthorityId {
        for (size_t i = 0; i < source_count_; ++i)
            if (sources_[i].name.equals(name)) return sources_[i].authority;
        return {};
    }
    auto discard(TaskId id, myos_status_t status,
                 libk::optional<typename Completions::Receiver>& receiver) noexcept -> bool {
        if (!receiver || !receiver->valid()) return false;
        for (;;) {
            const auto closed = table_.continue_close(id);
            if (closed == MYOS_STATUS_OK) break;
            if (!retryable(closed)) return false;
            myos::yield();
        }
        const auto result = receiver->take();
        return result && result->task == id
            && result->reason == CloseReason::ConstructionFailure && result->status == status;
    }
    static void checked(bool result) noexcept {
        if (!result) myos::exit(MYOS_STATUS_INTERNAL);
    }

public:
    struct handle final {
        TaskId id{};
        libk::optional<typename Completions::Receiver> receiver{};
        // Borrowed receive authority supplied by the caller. TaskSpace retains
        // only Signal for terminal publication; it cannot receive this event.
        myos_cap_t events{};
        PlanTaskId plan_task{};
        // Checked identities only; the provider TaskRecord owns registrations.
        AuthorityId exports[DEPLOY_TASK_EXPORT_MAX]{};
        auto token() const noexcept -> uint64_t {
            return (uint64_t{id.generation} << 32) | id.slot;
        }
    };

    static auto name(const char* text) noexcept -> ByteView {
        return {reinterpret_cast<const uint8_t*>(text), myos::service::length(text)};
    }
    void open(const myos::bootstrap::BootstrapView& info) noexcept {
        pool_ = {myos::service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL), 0};
        cpus_ = info.cpu_count();
    }
    auto load(program& program, const myos::bootstrap::BootstrapView& info,
              myos_cap_t package, size_t package_size,
              uintptr_t address = 0x10000000, uintptr_t scratch = 0x18000000) noexcept -> myos_status_t {
        auto status = program.close();
        if (status != MYOS_STATUS_OK) return status;
        const myos::cap::CapRef vspace{myos::service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE), 0};
        const auto size = Window::round_size(package_size);
        status = program.bundle_.open(vspace, {package, 0}, Window{address, size}, package_size);
        if (status != MYOS_STATUS_OK) return status;
        const auto* package_view = program.bundle_.view();
        myos::boot::Module manifest{};
        if (!package_view || !package_view->find("manifest", manifest) || !manifest.data_module())
            return MYOS_STATUS_BAD_ARGS;
        const auto bytes = manifest.data();
        auto parsed = ManifestView::parse(bytes.data(), bytes.size(), manifest_workspace_);
        if (!parsed || !parsed.value().validate_boot_bundle(*package_view, manifest_workspace_))
            return MYOS_STATUS_BAD_ARGS;
        auto decoded = program.plans_.decode(parsed.value());
        if (!decoded) return MYOS_STATUS_BAD_ARGS;
        program.plan_ = libk::move(decoded.value());
        myos_word_t scratch_size = 0;
        auto lease = program.plan_.lease();
        if (!lease) return MYOS_STATUS_INTERNAL;
        for (uint32_t i = 0; i < program.plan_.task_count(); ++i) {
            auto required = required_scratch_size(lease->task(i), *program.bundle_.view());
            if (!required) return MYOS_STATUS_BAD_ARGS;
            if (*required > scratch_size) scratch_size = *required;
        }
        return program.scratch_.open(vspace, Window{scratch, scratch_size}, Window{address, size});
    }
    auto load(program& program, const myos::bootstrap::BootstrapView& info) noexcept -> myos_status_t {
        return load(program, info, myos::service::capability(info, MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE), info.bundle_size());
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
    auto add_boot_sources(const myos::bootstrap::BootstrapView& info) noexcept -> myos_status_t {
        auto status = add("domain", myos::service::capability(info, MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN),
                          MYOS_OBJECT_KIND_SCHED_DOMAIN, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONTROL);
        if (status != MYOS_STATUS_OK) return status;
        return add("bundle", myos::service::capability(info, MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE),
                   MYOS_OBJECT_KIND_MEMORY, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_MAP | MYOS_RIGHT_INSPECT,
                   0, Window::round_size(info.bundle_size()) / 4096, MYOS_VM_READ, MYOS_VM_NORMAL);
    }
    // Validate the whole graph before publishing any task. Names select an
    // already authorized root or a declared provider export; they grant no rights.
    auto validate_graph(const program& program) const noexcept -> myos_status_t {
        const auto& plan = program.plan_;
        if (plan.task_count() == 0 || plan.task_count() > Capacity) return MYOS_STATUS_BAD_ARGS;
        for (uint32_t t = 0; t < plan.task_count(); ++t) {
            const auto& task = *plan.task(t);
            for (uint32_t x = 0; x < task.executions.count; ++x)
                if (!source(plan.symbol(plan.execution(task.executions.first + x)->domain)).valid())
                    return MYOS_STATUS_DENIED;
            for (uint32_t i = 0; i < task.imports.count; ++i) {
                const auto& imported = *plan.import(task.imports.first + i);
                if (imported.source_class != DEPLOY_IMPORT_SOURCE_AUTHORITY) continue;
                const auto label = plan.symbol(imported.source);
                size_t matches = source(label).valid() ? 1 : 0;
                for (uint32_t p = 0; p < plan.task_count(); ++p) {
                    const auto& provider = *plan.task(p);
                    for (uint32_t e = 0; e < provider.exports.count; ++e) {
                        const auto& exported = *plan.export_record(provider.exports.first + e);
                        if (!plan.symbol(exported.key).equals(label)) continue;
                        bool required{};
                        for (uint32_t d = 0; d < task.dependencies.count; ++d) {
                            const auto& edge = *plan.dependency(task.dependencies.first + d);
                            if (edge.target == p && edge.kind == DEPLOY_DEPENDENCY_REQUIRED
                                && (edge.flags & (DEPLOY_DEPENDENCY_STARTUP | DEPLOY_DEPENDENCY_READINESS))) required = true;
                        }
                        if (!required || exported.source_class != DEPLOY_EXPORT_PREPARED_KEY)
                            return MYOS_STATUS_DENIED;
                        ++matches;
                    }
                }
                if (matches != 1) return MYOS_STATUS_DENIED;
            }
        }
        return MYOS_STATUS_OK;
    }
    auto launch(program& program, ByteView name, myos_status_t& status, options options = {},
                libk::Span<const handle*> providers = {}) noexcept -> libk::optional<handle> {
        status = MYOS_STATUS_BAD_ARGS;
        const auto index = program.plan_.find_task(name);
        auto lease = program.plan_.lease();
        if (!index || !lease) return libk::nullopt;
        auto task = lease->task(*index);
        if (options.admit != nullptr && !options.admit(task, name)) { status = MYOS_STATUS_DENIED; return libk::nullopt; }
        myos::bootstrap::Arguments defaults;
        const myos::bootstrap::Arguments* arguments = options.arguments;
        if (arguments == nullptr && !task.row()->arguments.empty()) {
            const auto encoded = task.symbol(task.row()->arguments);
            if (!defaults.decode(reinterpret_cast<const char*>(encoded.data()), encoded.size()))
                return libk::nullopt;
            arguments = &defaults;
        }
        RegistrationJournal<MYOS_BOOTSTRAP_MAX_IMPORTS> temporary;
        AuthorityId overrides[MYOS_BOOTSTRAP_MAX_IMPORTS]{};
        auto retire = libk::on_scope_exit([&]() noexcept { checked(temporary.retire_all() == MYOS_STATUS_OK); });
        if (options.sources.size() > MYOS_BOOTSTRAP_MAX_IMPORTS) return libk::nullopt;
        for (size_t i = 0; i < options.sources.size(); ++i) {
            const auto& source = options.sources[i];
            auto id = temporary.register_source(authorities_, source.cap,
                AuthorityCapacity + 1 + i, source.ceiling);
            if (!id) { status = MYOS_STATUS_DENIED; return libk::nullopt; }
            overrides[i] = *id;
        }
        TaskAuthorityBindings bindings{};
        for (uint32_t i = 0; i < task.row()->executions.count; ++i)
            bindings.domains[i] = source(task.symbol(task.execution(i)->domain));
        for (uint32_t i = 0; i < task.row()->imports.count; ++i) {
            const auto& import = *task.import(i);
            if (import.source_class == DEPLOY_IMPORT_SOURCE_AUTHORITY) {
                bindings.imports[i] = source(task.symbol(import.source));
                for (const auto* provider : providers) {
                    if (provider == nullptr || provider->plan_task.plan != task.id.plan
                        || table_.tag(provider->id) != TaskSlotTag::Record) continue;
                    const auto* row = program.plan_.task(provider->plan_task.index);
                    for (uint32_t e = 0; e < row->exports.count; ++e) {
                        const auto& exported = *program.plan_.export_record(row->exports.first + e);
                        if (!program.plan_.symbol(exported.key).equals(task.symbol(import.source))) continue;
                        if (bindings.imports[i].valid()) { status = MYOS_STATUS_BAD_ARGS; return libk::nullopt; }
                        bindings.imports[i] = provider->exports[e];
                    }
                }
                for (size_t s = 0; s < options.sources.size(); ++s)
                    if (task.symbol(import.source).equals(tasks::name(options.sources[s].name)))
                        bindings.imports[i] = overrides[s];
                if (!bindings.imports[i].valid()) { status = MYOS_STATUS_DENIED; return libk::nullopt; }
            }
        }
        myos::cap::OwnedCap terminal;
        if (options.terminal_events != 0) {
            const auto copied = myos::cap_duplicate(options.terminal_events, 0, MYOS_RIGHT_SIGNAL);
            if (copied.status != MYOS_STATUS_OK) { status = copied.status; return libk::nullopt; }
            terminal = myos::cap::OwnedCap{{copied.value, 0}};
        }
        auto pending = Builder::begin(completions_, table_, libk::move(*lease), *index);
        if (!pending) { status = MYOS_STATUS_BUSY; return libk::nullopt; }
        auto builder = libk::move(*pending);
        handle handle{builder.record()->id(), builder.take_receiver(), options.terminal_events};
        TaskConstructionInput<Backend, Authorities> input{
            .parent_pool = pool_, .bundle = &program.bundle_, .scratch = &program.scratch_,
            .runtime_cpu_count = cpus_, .bindings = &bindings, .image_source = options.image_source,
            .arguments = arguments,
            .terminal_notification = options.terminal_events != 0 ? &terminal : nullptr, .workspace = workspace_};
        status = builder.construct(input, authorities_);
        if (status == MYOS_STATUS_OK && !builder.commit_prepared()) status = MYOS_STATUS_INTERNAL;
        if (status != MYOS_STATUS_OK) {
            if (builder.valid()) checked(builder.fail(CloseReason::ConstructionFailure, status));
            checked(discard(handle.id, status, handle.receiver));
            return libk::nullopt;
        }
        handle.plan_task = task.id;
        for (uint32_t e = 0; e < task.row()->exports.count; ++e) {
            const auto exported = table_.register_prepared_export(handle.id, e, authorities_);
            if (!exported) {
                status = MYOS_STATUS_DENIED;
                checked(table_.begin_close(handle.id, CloseReason::ConstructionFailure, status));
                checked(discard(handle.id, status, handle.receiver));
                return libk::nullopt;
            }
            handle.exports[e] = *exported;
        }
        status = table_.start(handle.id);
        if (status != MYOS_STATUS_OK) {
            checked(table_.begin_close(handle.id, CloseReason::ConstructionFailure, status));
            checked(discard(handle.id, status, handle.receiver));
            return libk::nullopt;
        }
        if (options.close_badge != 0)
            table_.close_events(handle.id, {options.terminal_events, 0}, options.close_badge);
        return handle;
    }
    auto launch(program& program, const char* text, myos_status_t& status) noexcept -> libk::optional<handle> {
        return launch(program, name(text), status);
    }
    auto wait(handle& handle) noexcept -> myos_status_t {
        for (;;) {
            const auto result = collect(handle);
            if (result.status == MYOS_STATUS_OK)
                return static_cast<myos_status_t>(result.value);
            if (result.status != MYOS_STATUS_WOULD_BLOCK && !retryable(result.status))
                return result.status;
            if (closing_needs_poll(handle)) { myos::yield(); continue; }
            const auto local = handle.events == 0 ? table_.terminal_notification(handle.id)
                                                 : libk::optional<myos::cap::CapRef>{myos::cap::CapRef{handle.events, 0}};
            if (!local) return MYOS_STATUS_INVALID_CAP;
            const auto wake = myos::notification_wait(local->selector);
            if (wake.status != MYOS_STATUS_OK) return wake.status;
            notify(handle, wake.value);
        }
    }
    auto stop(handle& handle) noexcept -> myos_status_t {
        const auto status = request_stop(handle);
        return status == MYOS_STATUS_OK ? wait(handle) : status;
    }
    auto observe(const handle& handle) noexcept -> myos::SysResult { return table_.observe_terminal(handle.id); }
    auto ready(const handle& handle) noexcept -> myos_status_t {
        if (table_.ready(handle.id)) return MYOS_STATUS_OK;
        const auto status = table_.consume_readiness(handle.id);
        return status == MYOS_STATUS_RETRY ? MYOS_STATUS_WOULD_BLOCK : status;
    }

    // With close_badge configured, closing advances without waiting for pool
    // refund. The caller services other producers while teardown is pending.
    auto poll(handle& handle) noexcept -> myos_status_t {
        if (!handle.receiver || !handle.receiver->valid()) return MYOS_STATUS_INVALID_CAP;
        if (handle.receiver->ready()) return MYOS_STATUS_OK;
        if (table_.tag(handle.id) != TaskSlotTag::Closing) {
            const auto observed = table_.observe_terminal(handle.id);
            if (observed.status != MYOS_STATUS_OK) return observed.status;
            if (observed.value == 0) return MYOS_STATUS_WOULD_BLOCK;
            const auto status = static_cast<myos_status_t>(static_cast<int64_t>(observed.value2));
            const auto consumed = table_.consume_terminal(handle.id, observed);
            if (consumed != MYOS_STATUS_OK) return consumed;
            checked(table_.begin_close(handle.id, CloseReason::Terminal, status));
        }
        return table_.continue_close(handle.id);
    }
    auto closing(const handle& handle) const noexcept -> bool {
        return table_.tag(handle.id) == TaskSlotTag::Closing;
    }
    auto closing_needs_poll(const handle& handle) noexcept -> bool {
        return closing(handle) && !table_.close_waiting(handle.id);
    }
    void notify(handle& handle, myos_word_t badges) noexcept {
        table_.observe_close(handle.id, badges);
    }
    auto request_stop(handle& handle) noexcept -> myos_status_t {
        if (!handle.receiver || !handle.receiver->valid()) return MYOS_STATUS_INVALID_CAP;
        if (handle.receiver->ready() || closing(handle)) return MYOS_STATUS_OK;
        // A terminal result already published by the execution wins over a
        // later stop request. A live execution instead closes as CANCELED.
        const auto status = poll(handle);
        if (status != MYOS_STATUS_WOULD_BLOCK) return retryable(status) ? MYOS_STATUS_OK : status;
        return table_.terminate(handle.id, CloseReason::Explicit, MYOS_STATUS_CANCELED)
            ? MYOS_STATUS_OK : MYOS_STATUS_INTERNAL;
    }
    auto collect(handle& handle) noexcept -> myos::SysResult {
        const auto status = poll(handle);
        if (status != MYOS_STATUS_OK) return {.status = status};
        const auto result = handle.receiver->take();
        checked(result && result->task == handle.id);
        return {.status = MYOS_STATUS_OK, .value = static_cast<myos_word_t>(result->status)};
    }
    auto result(const handle& handle) const noexcept -> libk::optional<CompletionResult> {
        return handle.receiver ? handle.receiver->result() : libk::nullopt;
    }

};

} // namespace deploy
