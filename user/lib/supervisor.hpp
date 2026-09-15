#pragma once

#include <libk/noncopyable.hpp>
#include <user/lib/deployment_syscall.hpp>
#include <user/lib/task_supervision.hpp>
#include <user/lib/service.hpp>

namespace myos::deploy {

struct LaunchOptions final {
    ImageSource image_source{};
    const bootstrap::Arguments* arguments{};
    myos_cap_t terminal_events{};
    myos_word_t close_badge{};
    bool (*admit)(const TaskPlanView&) noexcept{};
};

// Program owns the bytes borrowed by its immutable plan and live tasks.
// Stable storage is required because PlanLease refers to PlanSet's control.
class Program final : private libk::noncopyable_nonmovable {
    template<size_t, size_t> friend class Supervisor;
    cap::MappedBundle bundle_{};
    cap::ScratchWindow scratch_{};
    PlanSet<1> plans_{};
    DeploymentPlan plan_{};
public:
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
    void open(const bootstrap::BootstrapView& info) noexcept {
        pool_ = {service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL), 0};
        cpus_ = info.cpu_count();
    }
    auto load(Program& program, const bootstrap::BootstrapView& info,
              myos_cap_t package, size_t package_size,
              uintptr_t address = 0x10000000, uintptr_t scratch = 0x18000000) noexcept -> myos_status_t {
        auto status = program.close();
        if (status != MYOS_STATUS_OK) return status;
        const cap::CapRef vspace{service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE), 0};
        const auto size = Window::round_size(package_size);
        status = program.bundle_.open(vspace, {package, 0}, Window{address, size}, package_size);
        if (status != MYOS_STATUS_OK) return status;
        if (!supervision::decode_plan(program.bundle_, manifest_workspace_, program.plans_, program.plan_, 0))
            return MYOS_STATUS_BAD_ARGS;
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
    auto load(Program& program, const bootstrap::BootstrapView& info) noexcept -> myos_status_t {
        return load(program, info, service::capability(info, MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE), info.bundle_size());
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
    auto launch(Program& program, ByteView name, myos_status_t& status, LaunchOptions options = {}) noexcept -> libk::optional<Handle> {
        status = MYOS_STATUS_BAD_ARGS;
        const auto index = program.plan_.find_task(name);
        auto lease = program.plan_.lease();
        if (!index || !lease) return libk::nullopt;
        auto task = lease->task(*index);
        if (options.admit != nullptr && !options.admit(task)) { status = MYOS_STATUS_DENIED; return libk::nullopt; }
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
        cap::OwnedCap terminal;
        if (options.terminal_events != 0) {
            const auto copied = cap_duplicate(options.terminal_events, 0, MYOS_RIGHT_SIGNAL);
            if (copied.status != MYOS_STATUS_OK) { status = copied.status; return libk::nullopt; }
            terminal = cap::OwnedCap{{copied.value, 0}};
        }
        auto pending = Builder::begin(completions_, table_, libk::move(*lease), *index);
        if (!pending) { status = MYOS_STATUS_BUSY; return libk::nullopt; }
        auto builder = libk::move(*pending);
        Handle handle{builder.record()->id(), builder.take_receiver()};
        TaskConstructionInput<Backend, Authorities> input{
            .parent_pool = pool_, .bundle = &program.bundle_, .scratch = &program.scratch_,
            .runtime_cpu_count = cpus_, .bindings = &bindings, .image_source = options.image_source,
            .arguments = options.arguments,
            .terminal_notification = options.terminal_events != 0 ? &terminal : nullptr, .workspace = workspace_};
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
        if (options.close_badge != 0)
            table_.close_events(handle.id, {options.terminal_events, 0}, options.close_badge);
        return handle;
    }
    auto launch(Program& program, const char* text, myos_status_t& status) noexcept -> libk::optional<Handle> {
        return launch(program, name(text), status);
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

    // Poll never waits for a child. Closing advances one bounded pass; the
    // caller services other producers and retries while teardown is pending.
    auto poll(Handle& handle) noexcept -> myos_status_t {
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
    auto closing(const Handle& handle) const noexcept -> bool {
        return table_.tag(handle.id) == TaskSlotTag::Closing;
    }
    auto closing_needs_poll(const Handle& handle) noexcept -> bool {
        return closing(handle) && !table_.close_waiting(handle.id);
    }
    void notify(Handle& handle, myos_word_t badges) noexcept {
        table_.observe_close(handle.id, badges);
    }
    auto request_stop(Handle& handle) noexcept -> myos_status_t {
        if (!handle.receiver || !handle.receiver->valid()) return MYOS_STATUS_INVALID_CAP;
        if (handle.receiver->ready() || closing(handle)) return MYOS_STATUS_OK;
        // A terminal result already published by the execution wins over a
        // later stop request. A live execution instead closes as CANCELED.
        const auto status = poll(handle);
        if (status != MYOS_STATUS_WOULD_BLOCK) return retryable(status) ? MYOS_STATUS_OK : status;
        return table_.terminate(handle.id, CloseReason::Explicit, MYOS_STATUS_CANCELED)
            ? MYOS_STATUS_OK : MYOS_STATUS_INTERNAL;
    }
    auto collect(Handle& handle) noexcept -> SysResult {
        const auto status = poll(handle);
        if (status != MYOS_STATUS_OK) return {.status = status};
        const auto result = handle.receiver->take();
        checked(result && result->task == handle.id);
        return {.status = MYOS_STATUS_OK, .value = static_cast<myos_word_t>(result->status)};
    }

};

} // namespace myos::deploy
