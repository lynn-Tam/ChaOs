#pragma once

#include <optional>
#include <utility>


#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <libk/span.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/detail/space.hpp>
#include <servers/deploy/detail/task.hpp>
#include <servers/runtime/service.hpp>

namespace deploy {

struct source final {
    const char* name{};
    sys::cap::CapRef cap{};
    CapView ceiling{};
};

struct options final {
    ImageSource image_source{};
    const boot::Args* arguments{};
    cap_t terminal_events{};
    word_t close_badge{};
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
    auto close() noexcept -> status_t {
        if (plan_.borrowed()) return STATUS_BUSY;
        plan_ = {};
        const auto status = scratch_.close();
        return status == STATUS_OK ? bundle_.close() : status;
    }

};

// Only TaskTable owns task state/generations. CompletionSet retains each final
// result after resources close, until its unique receiver consumes or detaches.
template<size_t Capacity, size_t AuthorityCapacity = 16>
class tasks final {
    using Backend = sys::cap::SyscallBackend;
    using Space = TaskSpace<kTaskLocalCapacity, kTaskImportRemoteCapacity, Backend>;
    using Completions = CompletionSet<Capacity>;
    using Table = TaskTable<TaskRecord<Space>, Completions, Capacity>;
    using Builder = TaskBuilder<Table, Completions>;
    using Authorities = AuthoritySet<AuthorityCapacity>;

    struct Source final { ByteView name{}; AuthorityId authority{}; };
    sys::cap::CapRef pool_{};
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
    auto discard(TaskId id, status_t status,
                 std::optional<typename Completions::Receiver>& receiver) noexcept -> bool {
        if (!receiver || !receiver->valid()) return false;
        for (;;) {
            const auto closed = table_.continue_close(id);
            if (closed == STATUS_OK) break;
            if (!retryable(closed)) return false;
            sys::yield();
        }
        const auto result = receiver->take();
        return result && result->task == id
            && result->reason == CloseReason::ConstructionFailure && result->status == status;
    }
    static void checked(bool result) noexcept {
        if (!result) sys::exit(STATUS_INTERNAL);
    }

public:
    struct handle final {
        TaskId id{};
        std::optional<typename Completions::Receiver> receiver{};
        // Borrowed receive authority supplied by the caller. TaskSpace retains
        // only Signal for terminal publication; it cannot receive this event.
        cap_t events{};
        PlanTaskId plan_task{};
        // Checked identities only; the provider TaskRecord owns registrations.
        AuthorityId exports[DEPLOY_TASK_EXPORT_MAX]{};
        auto token() const noexcept -> uint64_t {
            return (uint64_t{id.generation} << 32) | id.slot;
        }
    };

    static auto name(const char* text) noexcept -> ByteView {
        return {reinterpret_cast<const uint8_t*>(text), sys::service::length(text)};
    }
    void open(const boot::BootView& info) noexcept {
        pool_ = {sys::service::capability(info, BOOT_POOL), 0};
        cpus_ = info.cpu_count();
    }
    auto load(program& program, const boot::BootView& info,
              cap_t package, size_t package_size,
              uintptr_t address = 0x10000000, uintptr_t scratch = 0x18000000) noexcept -> status_t {
        auto status = program.close();
        if (status != STATUS_OK) return status;
        const sys::cap::CapRef vspace{sys::service::capability(info, BOOT_VSPACE), 0};
        const auto size = Window::round_size(package_size);
        status = program.bundle_.open(vspace, {package, 0}, Window{address, size}, package_size);
        if (status != STATUS_OK) return status;
        const auto* package_view = program.bundle_.view();
        boot::Module manifest{};
        if (!package_view || !package_view->find("manifest", manifest) || !manifest.data_module())
            return STATUS_BAD_ARGS;
        const auto bytes = manifest.data();
        auto parsed = ManifestView::parse(bytes.data(), bytes.size(), manifest_workspace_);
        if (!parsed || !parsed.value().validate_boot_bundle(*package_view, manifest_workspace_))
            return STATUS_BAD_ARGS;
        auto decoded = program.plans_.decode(parsed.value());
        if (!decoded) return STATUS_BAD_ARGS;
        program.plan_ = std::move(decoded.value());
        word_t scratch_size = 0;
        auto lease = program.plan_.lease();
        if (!lease) return STATUS_INTERNAL;
        for (uint32_t i = 0; i < program.plan_.task_count(); ++i) {
            auto required = required_scratch_size(lease->task(i), *program.bundle_.view());
            if (!required) return STATUS_BAD_ARGS;
            if (*required > scratch_size) scratch_size = *required;
        }
        return program.scratch_.open(vspace, Window{scratch, scratch_size}, Window{address, size});
    }
    auto load(program& program, const boot::BootView& info) noexcept -> status_t {
        return load(program, info, sys::service::capability(info, BOOT_BUNDLE), info.bundle_size());
    }
    auto add(const char* label, cap_t cap, uint16_t kind, uint64_t rights,
             uint64_t first = 0, uint64_t count = 0,
             uint64_t access = 0, uint64_t types = 0) noexcept -> status_t {
        if (source_count_ == AuthorityCapacity || cap == 0 || source(name(label)).valid())
            return STATUS_BAD_ARGS;
        const CapView ceiling{
            .version = CAP_ATTENUATION_VERSION_CURRENT,
            .kind = kind, .size = CAP_ATTENUATION_SIZE,
            .rights = rights, .words = {first, count, access, types}};
        auto id = journal_.register_source(authorities_, {cap, 0}, source_count_ + 1, ceiling);
        if (!id) return STATUS_DENIED;
        sources_[source_count_++] = {name(label), *id};
        return STATUS_OK;
    }
    auto add_boot_sources(const boot::BootView& info) noexcept -> status_t {
        auto status = add("domain", sys::service::capability(info, BOOT_DOMAIN),
                          OBJECT_KIND_SCHED_DOMAIN, RIGHT_DUPLICATE | RIGHT_CONTROL);
        if (status != STATUS_OK) return status;
        return add("bundle", sys::service::capability(info, BOOT_BUNDLE),
                   OBJECT_KIND_MEMORY, RIGHT_DUPLICATE | RIGHT_MAP | RIGHT_INSPECT,
                   0, Window::round_size(info.bundle_size()) / 4096, VM_READ, 0);
    }
    // Validate the whole graph before publishing any task. Names select an
    // already authorized root or a declared provider export; they grant no rights.
    auto validate_graph(const program& program) const noexcept -> status_t {
        const auto& plan = program.plan_;
        if (plan.task_count() == 0 || plan.task_count() > Capacity) return STATUS_BAD_ARGS;
        for (uint32_t t = 0; t < plan.task_count(); ++t) {
            const auto& task = *plan.task(t);
            for (uint32_t x = 0; x < task.executions.count; ++x)
                if (!source(plan.symbol(plan.execution(task.executions.first + x)->domain)).valid())
                    return STATUS_DENIED;
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
                            return STATUS_DENIED;
                        ++matches;
                    }
                }
                if (matches != 1) return STATUS_DENIED;
            }
        }
        return STATUS_OK;
    }
    auto launch(program& program, ByteView name, status_t& status, options options = {},
                libk::Span<const handle*> providers = {}) noexcept -> std::optional<handle> {
        status = STATUS_BAD_ARGS;
        const auto index = program.plan_.find_task(name);
        auto lease = program.plan_.lease();
        if (!index || !lease) return std::nullopt;
        auto task = lease->task(*index);
        if (options.admit != nullptr && !options.admit(task, name)) { status = STATUS_DENIED; return std::nullopt; }
        boot::Args defaults;
        const boot::Args* arguments = options.arguments;
        if (arguments == nullptr && !task.row()->arguments.empty()) {
            const auto encoded = task.symbol(task.row()->arguments);
            if (!defaults.decode(reinterpret_cast<const char*>(encoded.data()), encoded.size()))
                return std::nullopt;
            arguments = &defaults;
        }
        RegistrationJournal<DEPLOY_TASK_BOOTSTRAP_MAX> temporary;
        AuthorityId overrides[DEPLOY_TASK_BOOTSTRAP_MAX]{};
        auto retire = libk::on_scope_exit([&]() noexcept { checked(temporary.retire_all() == STATUS_OK); });
        if (options.sources.size() > DEPLOY_TASK_BOOTSTRAP_MAX) return std::nullopt;
        for (size_t i = 0; i < options.sources.size(); ++i) {
            const auto& source = options.sources[i];
            auto id = temporary.register_source(authorities_, source.cap,
                AuthorityCapacity + 1 + i, source.ceiling);
            if (!id) { status = STATUS_DENIED; return std::nullopt; }
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
                        if (bindings.imports[i].valid()) { status = STATUS_BAD_ARGS; return std::nullopt; }
                        bindings.imports[i] = provider->exports[e];
                    }
                }
                for (size_t s = 0; s < options.sources.size(); ++s)
                    if (task.symbol(import.source).equals(tasks::name(options.sources[s].name)))
                        bindings.imports[i] = overrides[s];
                if (!bindings.imports[i].valid()) { status = STATUS_DENIED; return std::nullopt; }
            }
        }
        sys::cap::OwnedCap terminal;
        if (options.terminal_events != 0) {
            const auto copied = sys::cap_duplicate(options.terminal_events, 0, RIGHT_SIGNAL);
            if (copied.status != STATUS_OK) { status = copied.status; return std::nullopt; }
            terminal = sys::cap::OwnedCap{{copied.value, 0}};
        }
        auto pending = Builder::begin(completions_, table_, std::move(*lease), *index);
        if (!pending) { status = STATUS_BUSY; return std::nullopt; }
        auto builder = std::move(*pending);
        handle handle{builder.record()->id(), builder.take_receiver(), options.terminal_events};
        TaskConstructionInput<Backend, Authorities> input{
            .parent_pool = pool_, .bundle = &program.bundle_, .scratch = &program.scratch_,
            .runtime_cpu_count = cpus_, .bindings = &bindings, .image_source = options.image_source,
            .arguments = arguments,
            .terminal_notification = options.terminal_events != 0 ? &terminal : nullptr, .workspace = workspace_};
        status = builder.construct(input, authorities_);
        if (status == STATUS_OK && !builder.commit_prepared()) status = STATUS_INTERNAL;
        if (status != STATUS_OK) {
            if (builder.valid()) checked(builder.fail(CloseReason::ConstructionFailure, status));
            checked(discard(handle.id, status, handle.receiver));
            return std::nullopt;
        }
        handle.plan_task = task.id;
        for (uint32_t e = 0; e < task.row()->exports.count; ++e) {
            const auto exported = table_.register_prepared_export(handle.id, e, authorities_);
            if (!exported) {
                status = STATUS_DENIED;
                checked(table_.begin_close(handle.id, CloseReason::ConstructionFailure, status));
                checked(discard(handle.id, status, handle.receiver));
                return std::nullopt;
            }
            handle.exports[e] = *exported;
        }
        status = table_.start(handle.id);
        if (status != STATUS_OK) {
            checked(table_.begin_close(handle.id, CloseReason::ConstructionFailure, status));
            checked(discard(handle.id, status, handle.receiver));
            return std::nullopt;
        }
        if (options.close_badge != 0)
            table_.close_events(handle.id, {options.terminal_events, 0}, options.close_badge);
        return handle;
    }
    auto launch(program& program, const char* text, status_t& status) noexcept -> std::optional<handle> {
        return launch(program, name(text), status);
    }
    auto wait(handle& handle) noexcept -> status_t {
        for (;;) {
            const auto result = collect(handle);
            if (result.status == STATUS_OK)
                return static_cast<status_t>(result.value);
            if (result.status != STATUS_WOULD_BLOCK && !retryable(result.status))
                return result.status;
            if (closing_needs_poll(handle)) { sys::yield(); continue; }
            const auto local = handle.events == 0 ? table_.terminal_notification(handle.id)
                                                 : std::optional<sys::cap::CapRef>{sys::cap::CapRef{handle.events, 0}};
            if (!local) return STATUS_INVALID_CAP;
            const auto wake = sys::notification_wait(local->selector);
            if (wake.status != STATUS_OK) return wake.status;
            notify(handle, wake.value);
        }
    }
    auto stop(handle& handle) noexcept -> status_t {
        const auto status = request_stop(handle);
        return status == STATUS_OK ? wait(handle) : status;
    }
    auto observe(const handle& handle) noexcept -> sys::SysResult { return table_.observe_terminal(handle.id); }
    auto ready(const handle& handle) noexcept -> status_t {
        if (table_.ready(handle.id)) return STATUS_OK;
        const auto status = table_.consume_readiness(handle.id);
        return status == STATUS_RETRY ? STATUS_WOULD_BLOCK : status;
    }

    // With close_badge configured, closing advances without waiting for pool
    // refund. The caller services other producers while teardown is pending.
    auto poll(handle& handle) noexcept -> status_t {
        if (!handle.receiver || !handle.receiver->valid()) return STATUS_INVALID_CAP;
        if (handle.receiver->ready()) return STATUS_OK;
        if (table_.tag(handle.id) != TaskSlotTag::Closing) {
            const auto observed = table_.observe_terminal(handle.id);
            if (observed.status != STATUS_OK) return observed.status;
            if (observed.value == 0) return STATUS_WOULD_BLOCK;
            const auto status = static_cast<status_t>(static_cast<int64_t>(observed.value2));
            const auto consumed = table_.consume_terminal(handle.id, observed);
            if (consumed != STATUS_OK) return consumed;
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
    void notify(handle& handle, word_t badges) noexcept {
        table_.observe_close(handle.id, badges);
    }
    auto request_stop(handle& handle) noexcept -> status_t {
        if (!handle.receiver || !handle.receiver->valid()) return STATUS_INVALID_CAP;
        if (handle.receiver->ready() || closing(handle)) return STATUS_OK;
        // A terminal result already published by the execution wins over a
        // later stop request. A live execution instead closes as CANCELED.
        const auto status = poll(handle);
        if (status != STATUS_WOULD_BLOCK) return retryable(status) ? STATUS_OK : status;
        return table_.terminate(handle.id, CloseReason::Explicit, STATUS_CANCELED)
            ? STATUS_OK : STATUS_INTERNAL;
    }
    auto collect(handle& handle) noexcept -> sys::SysResult {
        const auto status = poll(handle);
        if (status != STATUS_OK) return {.status = status};
        const auto result = handle.receiver->take();
        checked(result && result->task == handle.id);
        return {.status = STATUS_OK, .value = static_cast<word_t>(result->status)};
    }
    auto result(const handle& handle) const noexcept -> std::optional<CompletionResult> {
        return handle.receiver ? handle.receiver->result() : std::nullopt;
    }

};

} // namespace deploy
