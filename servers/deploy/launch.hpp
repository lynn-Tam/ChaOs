#pragma once

#include <optional>
#include <utility>


#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <libk/span.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/detail/task.hpp>
#include <servers/runtime/service.hpp>

namespace deploy {

struct source final {
    const char* name{};
    sys::cap::CapRef cap{};
    CapView ceiling{};
};

struct options final {
    ElfSrc elf_source{};
    const boot::Args* arguments{};
    cap_t exit_events{};
    word_t close_badge{};
    libk::Span<const source> sources{};
    bool (*admit)(const TaskSpec&, ByteView) noexcept{};
};

// Owns immutable bundle bytes. Construction borrows its validated manifest;
// running tasks retain actual capabilities, not manifest references.
class program final : private libk::noncopyable_nonmovable {
    template<size_t, size_t> friend class tasks;
    BundleMap<> bundle_{};
    Map<> scratch_{};
    Manifest plan_{};
    uint64_t gen_{};
public:
    auto plan() const noexcept -> const Manifest& { return plan_; }
    auto close() noexcept -> status_t {
        plan_ = {};
        const auto status = scratch_.close();
        return status == STATUS_OK ? bundle_.close() : status;
    }

};

// One task slot owns resources through close, then retains the exit until collect.
template<size_t Capacity, size_t SourceCapacity = 16>
class tasks final {
    using Backend = sys::cap::SyscallBackend;
    using Table = TaskTable<TaskRecord<Backend>, Capacity>;

    struct Source final { ByteView name{}; CapSrc cap{}; };
    sys::cap::CapRef pool_{};
    uint32_t cpus_{};
    Source sources_[SourceCapacity]{};
    size_t source_count_{};
    ManifestWorkspace manifest_workspace_{};
    Table table_{};
    BuildBuf workspace_{};

    auto source(ByteView name) const noexcept -> CapSrc {
        for (size_t i = 0; i < source_count_; ++i)
            if (sources_[i].name.equals(name)) return sources_[i].cap;
        return {};
    }
    static void checked(bool result) noexcept {
        if (!result) sys::exit(STATUS_INTERNAL);
    }

public:
    struct handle final {
        TaskId id{};
        // Borrowed receive capability supplied by the caller. task retains
        // only Signal for exit publication; it cannot receive this event.
        cap_t events{};
        const deploy::program* recipe{};
        uint32_t row{};
        uint64_t recipe_gen{};
        auto key() const noexcept -> uint64_t {
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
        if (program.gen_ == UINT64_MAX) return STATUS_BUSY;
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
        auto parsed = Manifest::parse(bytes.data(), bytes.size(), manifest_workspace_);
        if (!parsed || !parsed.value().validate_boot_bundle(*package_view, manifest_workspace_))
            return STATUS_BAD_ARGS;
        program.plan_ = *parsed;
        ++program.gen_;
        word_t scratch_size = 0;
        for (uint32_t i = 0; i < program.plan_.task_count(); ++i) {
            auto required = required_scratch_size(TaskSpec{&program.plan_, i}, *program.bundle_.view());
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
        if (source_count_ == SourceCapacity || cap == 0 || source(name(label)).valid())
            return STATUS_BAD_ARGS;
        const CapView ceiling{
            .version = CAP_ATTENUATION_VERSION_CURRENT,
            .kind = kind, .size = CAP_ATTENUATION_SIZE,
            .rights = rights, .words = {first, count, access, types}};
        const CapSrc borrowed{{cap, 0}, ceiling};
        if (!borrowed.valid()) return STATUS_DENIED;
        sources_[source_count_++] = {name(label), borrowed};
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
            const auto task = *plan.task(t);
            for (uint32_t x = 0; x < task.execution_count; ++x)
                if (!source(plan.string(plan.execution(task.execution_first + x)->domain)).valid())
                    return STATUS_DENIED;
            for (uint32_t i = 0; i < task.import_count; ++i) {
                const auto imported = *plan.import(task.import_first + i);
                if (imported.source_class != DEPLOY_IMPORT_SOURCE_AUTHORITY) continue;
                const auto label = plan.string(imported.source);
                size_t matches = source(label).valid() ? 1 : 0;
                for (uint32_t p = 0; p < plan.task_count(); ++p) {
                    const auto provider = *plan.task(p);
                    for (uint32_t e = 0; e < provider.export_count; ++e) {
                        const auto exported = *plan.export_record(provider.export_first + e);
                        if (!plan.string(exported.key).equals(label)) continue;
                        bool required{};
                        for (uint32_t d = 0; d < task.dependency_count; ++d) {
                            const auto edge = *plan.dependency(task.dependency_first + d);
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
    // A returned handle must be collected even when status reports construction failure.
    auto launch(program& program, ByteView name, status_t& status, options options = {},
                libk::Span<const handle*> providers = {}) noexcept -> std::optional<handle> {
        status = STATUS_BAD_ARGS;
        const auto index = program.plan_.find_task(name);
        if (!index) return std::nullopt;
        const TaskSpec task{&program.plan_, *index};
        if (bool(options.admit) && !options.admit(task, name)) { status = STATUS_DENIED; return std::nullopt; }
        boot::Args defaults;
        const boot::Args* arguments = options.arguments;
        if (!arguments && !task.row()->arguments.empty()) {
            const auto encoded = task.string(task.row()->arguments);
            if (!defaults.decode(reinterpret_cast<const char*>(encoded.data()), encoded.size()))
                return std::nullopt;
            arguments = &defaults;
        }
        if (options.sources.size() > DEPLOY_TASK_BOOTSTRAP_MAX) return std::nullopt;
        for (const auto& src : options.sources)
            if (!CapSrc{src.cap, src.ceiling}.valid()) { status = STATUS_DENIED; return std::nullopt; }
        TaskBindings bindings{};
        for (uint32_t i = 0; i < task.row()->execution_count; ++i)
            bindings.domains[i] = source(task.string(task.execution(i)->domain));
        for (uint32_t i = 0; i < task.row()->import_count; ++i) {
            const auto import = *task.import(i);
            if (import.source_class == DEPLOY_IMPORT_SOURCE_AUTHORITY) {
                bindings.imports[i] = source(task.string(import.source));
                for (const auto* provider : providers) {
                    if (!provider || provider->recipe != &program || provider->recipe_gen != program.gen_
                        || table_.tag(provider->id) != TaskSlotTag::Record) continue;
                    const auto row = program.plan_.task(provider->row);
                    for (uint32_t e = 0; e < row->export_count; ++e) {
                        const auto exported = *program.plan_.export_record(row->export_first + e);
                        if (!program.plan_.string(exported.key).equals(task.string(import.source))) continue;
                        if (bindings.imports[i].valid()) { status = STATUS_BAD_ARGS; return std::nullopt; }
                        const auto cap = table_.export_cap(provider->id, e);
                        if (!cap) { status = STATUS_DENIED; return std::nullopt; }
                        bindings.imports[i] = *cap;
                    }
                }
                for (size_t s = 0; s < options.sources.size(); ++s)
                    if (task.string(import.source).equals(tasks::name(options.sources[s].name)))
                        bindings.imports[i] = {options.sources[s].cap, options.sources[s].ceiling};
                if (!bindings.imports[i].valid()) { status = STATUS_DENIED; return std::nullopt; }
            }
        }
        sys::cap::OwnedCap exit_signal;
        if (options.exit_events != 0) {
            const auto copied = sys::cap_duplicate(options.exit_events, 0, RIGHT_SIGNAL);
            if (copied.status != STATUS_OK) { status = copied.status; return std::nullopt; }
            exit_signal = sys::cap::OwnedCap{{copied.value, 0}};
        }
        BuildArgs<Backend> input{
            .parent_pool = pool_, .bundle = &program.bundle_, .scratch = &program.scratch_,
            .runtime_cpu_count = cpus_, .bindings = &bindings, .elf_source = options.elf_source,
            .arguments = arguments,
            .exit_notification = options.exit_events != 0 ? &exit_signal : nullptr, .workspace = workspace_};
        const auto spawned = table_.spawn(program.plan_, *index, input,
            options.close_badge ? sys::cap::CapRef{options.exit_events, 0} : sys::cap::CapRef{},
            options.close_badge);
        status = spawned.status;
        if (!spawned.id.valid()) return std::nullopt;
        handle handle{spawned.id, options.exit_events, &program, *index, program.gen_};
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
            const auto local = handle.events == 0 ? table_.exit_notification(handle.id)
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
    auto observe(const handle& handle) noexcept -> sys::SysResult { return table_.observe_exit(handle.id); }
    auto ready(const handle& handle) noexcept -> status_t {
        if (table_.ready(handle.id)) return STATUS_OK;
        const auto status = table_.consume_readiness(handle.id);
        return status == STATUS_RETRY ? STATUS_WOULD_BLOCK : status;
    }

    // With close_badge configured, closing advances without waiting for pool
    // refund. The caller services other producers while teardown is pending.
    auto poll(handle& handle) noexcept -> status_t {
        if (table_.result(handle.id)) return STATUS_OK;
        if (table_.tag(handle.id) == TaskSlotTag::Retired || table_.tag(handle.id) == TaskSlotTag::Vacant) return STATUS_INVALID_CAP;
        if (table_.tag(handle.id) != TaskSlotTag::Closing) {
            const auto observed = table_.observe_exit(handle.id);
            if (observed.status != STATUS_OK) return observed.status;
            if (observed.value == 0) return STATUS_WOULD_BLOCK;
            const auto status = static_cast<status_t>(static_cast<int64_t>(observed.value2));
            checked(table_.begin_close(handle.id, CloseReason::Exited, status));
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
        if (table_.tag(handle.id) == TaskSlotTag::Retired || table_.tag(handle.id) == TaskSlotTag::Vacant) return STATUS_INVALID_CAP;
        if (table_.result(handle.id) || closing(handle)) return STATUS_OK;
        // An exit result already published by the execution wins over a
        // later stop request. A live execution instead closes as CANCELED.
        const auto status = poll(handle);
        if (status != STATUS_WOULD_BLOCK) return retryable(status) ? STATUS_OK : status;
        return table_.begin_close(handle.id, CloseReason::Explicit, STATUS_CANCELED)
            ? STATUS_OK : STATUS_INTERNAL;
    }
    auto collect(handle& handle) noexcept -> sys::SysResult {
        const auto status = poll(handle);
        if (status != STATUS_OK) return {.status = status};
        const auto result = table_.reap(handle.id);
        checked(result && result->task == handle.id);
        return {.status = STATUS_OK, .value = static_cast<word_t>(result->status)};
    }
    auto result(const handle& handle) const noexcept -> std::optional<Exit> {
        return table_.result(handle.id);
    }

};

} // namespace deploy
