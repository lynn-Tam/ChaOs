#include <utility>
#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <sys/start.hpp>
#include <servers/deploy/detail/plan.hpp>
#include <servers/deploy/detail/space.hpp>
#include <servers/deploy/detail/authority.hpp>
#include <servers/deploy/detail/task.hpp>
#include <servers/uart/port.hpp>
#include <uapi/bootstrap.h>
#include <uapi/resource.h>
#include <uapi/status.h>
#include <uapi/vm.h>

/*
 * Unit 4 is a boot-root production caller.  The generated manifest is linked
 * below by the Meson graph; its bytes are emitted by the test manifest
 * generator, while this
 * file only orchestrates the already decoded plan and observes the normal
 * ownership/completion paths.
 */
namespace myos::task_builder_fixture {
extern const uint8_t manifest[];
extern const size_t manifest_size;
} // namespace myos::task_builder_fixture

namespace {

using Backend = myos::cap::SyscallBackend;
using Space = deploy::TaskSpace<
    deploy::kTaskLocalCapacity,
    deploy::kTaskImportRemoteCapacity,
    Backend>;
using Record = deploy::TaskRecord<Space>;
using Completions = deploy::CompletionSet<1>;
using Table = deploy::TaskTable<Record, Completions, 1>;
using Builder = deploy::TaskBuilder<Table, Completions>;
using Authorities = deploy::AuthoritySet<4>;
using Workspace = deploy::TaskConstructionWorkspace<Authorities>;
using Plans = deploy::PlanSet<1>;
using SourceSpace = deploy::TaskSpace<16, 4, Backend>;
using Source = deploy::RegisteredSpace<SourceSpace, 2>;

constexpr myos_word_t PageSize = DEPLOY_PAGE_SIZE;
constexpr myos_word_t BundleAddress = 0x1000'0000;
constexpr myos_word_t ScratchAddress = 0x1800'0000;
constexpr myos_word_t ScratchSize = 0x20'0000;
constexpr myos_word_t UartAddress = 0x3001'0000;
constexpr myos_word_t ParentMemory = 12 * 1024 * 1024;
constexpr myos_word_t ParentCaps = 1024;
constexpr myos_word_t ParentKinds = MYOS_RESOURCE_E7_KINDS;
constexpr myos_word_t SourceMemory = 128 * 1024;
constexpr myos_word_t SourceCaps = 128;
constexpr myos_word_t SourceKinds = MYOS_RESOURCE_E4_KINDS;
constexpr myos_word_t SourceDomainRights =
    MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_DELEGATE | MYOS_RIGHT_INSPECT
    | MYOS_RIGHT_CONTROL | MYOS_RIGHT_DESTROY | MYOS_RIGHT_REVOKE;

[[nodiscard]] constexpr auto retryable(myos_status_t status) noexcept -> bool {
    return status == MYOS_STATUS_BUSY || status == MYOS_STATUS_RETRY;
}

struct Console final {
    myos::cap::OwnedCap region{};
    myos::uart::Port port{0};
    deploy::LeasePhase phase{deploy::LeasePhase::Empty};

    [[nodiscard]] auto open(
        myos::cap::CapRef vspace,
        myos::cap::CapRef memory) noexcept -> bool {
        if (phase != deploy::LeasePhase::Empty
            && phase != deploy::LeasePhase::Closed) {
            return false;
        }
        port = myos::uart::Port{0};
        const myos::SysResult created = myos::vm_slice(
            vspace.selector,
            UartAddress,
            PageSize,
            MYOS_VM_READ | MYOS_VM_WRITE,
            MYOS_VM_DEVICE,
            MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
        if (created.status != MYOS_STATUS_OK || created.value == 0) {
            return false;
        }
        region = myos::cap::OwnedCap{
            myos::cap::CapRef{created.value, 0}};
        phase = deploy::LeasePhase::Ready;
        const myos::SysResult mapped_result = myos::vm_map(
            region.selector(), memory.selector, UartAddress, PageSize, 0,
            MYOS_VM_READ | MYOS_VM_WRITE);
        if (!deploy::committed(mapped_result.status)) {
            return false;
        }
        phase = deploy::LeasePhase::Mapped;
        port = myos::uart::Port{UartAddress};
        port.reset();
        /* The mapping is the authoritative lifetime; a null Port is an
         * invalid observation, but cleanup still owns the committed mapping. */
        if (!port.valid()) {
            return false;
        }
        return true;
    }

    [[nodiscard]] auto close() noexcept -> bool {
        /* Port is a borrow over the UART mapping.  Invalidate that borrow
         * before the first committed teardown step and never use it again. */
        port = myos::uart::Port{0};
        for (;;) {
            switch (phase) {
            case deploy::LeasePhase::Empty:
            case deploy::LeasePhase::Closed:
                return true;
            case deploy::LeasePhase::Mapped:
            case deploy::LeasePhase::Unmapping: {
                phase = deploy::LeasePhase::Unmapping;
                const myos_status_t status = myos::vm_unmap(
                    region.selector(), UartAddress, PageSize).status;
                if (deploy::committed(status)) {
                    phase = deploy::LeasePhase::Ready;
                    continue;
                }
                if (retryable(status)) {
                    myos::yield();
                    continue;
                }
                Backend::ownership_fault(status);
            }
            case deploy::LeasePhase::Ready:
            case deploy::LeasePhase::Destroying: {
                phase = deploy::LeasePhase::Destroying;
                const myos_status_t status = myos::vm_clear(
                    region.selector()).status;
                if (deploy::committed(status)) {
                    phase = deploy::LeasePhase::Closing;
                    continue;
                }
                if (retryable(status)) {
                    myos::yield();
                    continue;
                }
                Backend::ownership_fault(status);
            }
            case deploy::LeasePhase::Closing: {
                const myos_status_t status = region.close();
                if (status == MYOS_STATUS_OK) {
                    region = {};
                    phase = deploy::LeasePhase::Closed;
                    return true;
                }
                if (retryable(status)) {
                    myos::yield();
                    continue;
                }
                Backend::ownership_fault(status);
            }
            }
        }
    }

    void text(const char* value) noexcept {
        myos::uart::Writer writer{port};
        writer.write(value);
    }

    template<libk::fmt::fixed_string F, typename... Args>
    [[nodiscard]] auto print(const Args&... args) noexcept -> bool {
        return myos::uart::Printer{
            myos::uart::Writer{port}}.template print<F>(args...);
    }
};

struct Runtime final {
    Console console{};
    myos::cap::OwnedCap parent{};
    deploy::MappedBundle<> bundle{};
    deploy::ScratchWindow<> scratch{};
    Source source{};
    Authorities authorities{};
    Plans plans{};
    deploy::ManifestWorkspace manifest_workspace{};
    Table table{};
    Completions completions{};
    Workspace workspace{};
    deploy::AuthorityId domain{};
    deploy::AuthorityId typed_source{};
    deploy::DeploymentPlan plan{};
    const void* bootstrap{};
    myos_word_t bootstrap_size{};
    uint32_t cpu_count{};
    bool source_open{};
    bool parent_open{};
    bool bundle_open{};
    bool scratch_open{};
};

/* Static storage keeps the bounded plan/table/workspace out of the bootstrap
 * stack.  The exit syscall does not run C++ static destructors, so all live
 * kernel owners are explicitly drained by run(). */
Runtime runtime{};

[[nodiscard]] constexpr auto page_round(myos_word_t size) noexcept
    -> myos_word_t {
    return size <= static_cast<myos_word_t>(-1) - (PageSize - 1)
        ? (size + PageSize - 1) & ~(PageSize - 1)
        : 0;
}

[[nodiscard]] auto drain_task(deploy::TaskId id) noexcept -> bool {
    for (size_t attempt = 0; attempt < 256; ++attempt) {
        const myos_status_t status = runtime.table.continue_close(id);
        if (status == MYOS_STATUS_OK) {
            return true;
        }
        if (!retryable(status)) {
            return false;
        }
        myos::yield();
    }
    return false;
}

[[nodiscard]] auto source_ceiling(
    myos_object_kind_t kind,
    myos_word_t rights) noexcept -> myos_cap_attenuation {
    myos_cap_attenuation value{};
    value.version = MYOS_CAP_ATTENUATION_VERSION_CURRENT;
    value.kind = kind;
    value.size = MYOS_CAP_ATTENUATION_SIZE;
    value.rights = rights;
    if (kind == MYOS_OBJECT_KIND_MEMORY) {
        value.words[0] = 0;
        value.words[1] = 1;
        value.words[2] = MYOS_VM_READ | MYOS_VM_WRITE;
        value.words[3] = MYOS_VM_NORMAL;
    }
    return value;
}

[[nodiscard]] auto setup_source(
    myos_cap_t root_pool,
    myos_cap_t root_domain) noexcept -> bool {
    SourceSpace source_space{};
    const myos_status_t opened = source_space.open(
            myos::cap::CapRef{root_pool, 0},
            SourceMemory,
            SourceCaps,
            SourceKinds,
            32,
            1);
    static_cast<void>(runtime.console.print<"task-builder-test: source-status={}\n">(
        opened));
    if (opened != MYOS_STATUS_OK) {
        return false;
    }
    const myos::SysResult duplicated = myos::cap_duplicate(
        root_domain, 0, SourceDomainRights);
    static_cast<void>(runtime.console.print<"task-builder-test: duplicate-status={}\n">(
        duplicated.status));
    if (duplicated.status != MYOS_STATUS_OK || duplicated.value == 0) {
        return false;
    }
    myos::cap::OwnedCap domain_owner{
        myos::cap::CapRef{duplicated.value, 0}};
    const auto domain_slot = source_space.adopt_local(
        myos::cap::OwnedCap{domain_owner.release()},
        MYOS_OBJECT_KIND_SCHED_DOMAIN);
    if (!domain_slot) {
        return false;
    }
    const auto source_pool = source_space.pool();
    if (!source_pool) {
        return false;
    }
    const myos::SysResult memory = myos::memory_create(
        source_pool->selector, PageSize, MYOS_VM_READ | MYOS_VM_WRITE);
    static_cast<void>(runtime.console.print<"task-builder-test: source-memory-status={}\n">(
        memory.status));
    if (memory.status != MYOS_STATUS_OK || memory.value == 0) {
        return false;
    }
    myos::cap::OwnedCap memory_owner{
        myos::cap::CapRef{memory.value, 0}};
    const auto memory_slot = source_space.adopt_local(
        myos::cap::OwnedCap{memory_owner.release()},
        MYOS_OBJECT_KIND_MEMORY);
    if (!memory_slot) {
        return false;
    }
    if (!runtime.source.adopt(std::move(source_space))) {
        return false;
    }
    const auto domain_id = runtime.source.register_source(
        runtime.authorities,
        *domain_slot,
        UINT64_C(0x535441474545444f),
        source_ceiling(MYOS_OBJECT_KIND_SCHED_DOMAIN, MYOS_RIGHT_MASK));
    const auto memory_id = runtime.source.register_source(
        runtime.authorities,
        *memory_slot,
        UINT64_C(0x5354414745454d45),
        source_ceiling(MYOS_OBJECT_KIND_MEMORY, MYOS_RIGHT_MASK));
    if (!domain_id || !memory_id) {
        return false;
    }
    runtime.domain = *domain_id;
    runtime.typed_source = *memory_id;
    runtime.source_open = true;
    return true;
}

[[nodiscard]] auto setup_views(
    const myos::bootstrap::BootstrapView& bootstrap) noexcept -> bool {
    const myos_cap_t root_vspace = bootstrap.selector(
        MYOS_BOOTSTRAP_CAP_VSPACE);
    const myos_cap_t root_bundle = bootstrap.selector(
        MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE);
    const myos_cap_t root_pool = bootstrap.selector(
        MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const myos_cap_t root_domain = bootstrap.selector(
        MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN);
    const myos_cap_t uart_memory = bootstrap.selector(
        MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY);
    if (root_vspace == 0 || root_bundle == 0 || root_pool == 0
        || root_domain == 0 || uart_memory == 0) {
        return false;
    }
    if (!runtime.console.open(
            myos::cap::CapRef{root_vspace, 0},
            myos::cap::CapRef{uart_memory, 0})) {
        return false;
    }
    runtime.console.text("task-builder-test: boot-root\n");

    const myos::SysResult parent = Backend::resource_create_child(
        myos::cap::CapRef{root_pool, 0},
        ParentMemory,
        ParentCaps,
        ParentKinds);
    static_cast<void>(runtime.console.print<"task-builder-test: parent-status={}\n">(
        parent.status));
    if (parent.status != MYOS_STATUS_OK || parent.value == 0) {
        return false;
    }
    runtime.parent = myos::cap::OwnedCap{
        myos::cap::CapRef{parent.value, 0}};
    runtime.parent_open = true;
    runtime.bootstrap = bootstrap.data();
    runtime.bootstrap_size = sizeof(myos_bootstrap_info);
    runtime.cpu_count = bootstrap.cpu_count();
    runtime.console.text("task-builder-test: parent\n");

    const myos_word_t bundle_window = page_round(
        bootstrap.bundle_size());
    if (bundle_window == 0
        || runtime.bundle.open(
               myos::cap::CapRef{root_vspace, 0},
               myos::cap::CapRef{root_bundle, 0},
               deploy::Window{BundleAddress, bundle_window},
               bootstrap.bundle_size()) != MYOS_STATUS_OK) {
        return false;
    }
    runtime.bundle_open = true;
    runtime.console.text("task-builder-test: bundle\n");
    if (runtime.scratch.open(
            myos::cap::CapRef{root_vspace, 0},
            deploy::Window{ScratchAddress, ScratchSize},
            deploy::Window{BundleAddress, bundle_window})
        != MYOS_STATUS_OK) {
        return false;
    }
    runtime.scratch_open = true;
    runtime.console.text("task-builder-test: scratch\n");
    return setup_source(root_pool, root_domain);
}

[[nodiscard]] auto decode_plan() noexcept -> bool {
    const auto manifest = deploy::ManifestView::parse(
        myos::task_builder_fixture::manifest,
        myos::task_builder_fixture::manifest_size,
        runtime.manifest_workspace);
    if (!manifest) {
        return false;
    }
    auto decoded = deploy::DeploymentPlan::decode(
        manifest.value(), runtime.plans);
    if (!decoded) {
        return false;
    }
    runtime.plan = std::move(decoded.value());
    return runtime.plan.task_count() == 5
        && runtime.bundle.view() != nullptr;
}

[[nodiscard]] auto observe_completion(
    deploy::TaskId id,
    deploy::CloseReason reason,
    myos_status_t status,
    deploy::TaskBuilder<Table, Completions>& builder,
    std::optional<Completions::Receiver>& receiver) noexcept -> bool {
    if (builder.valid() && !builder.fail(reason, status)) {
        return false;
    }
    if (runtime.table.closing(id) == nullptr || !drain_task(id)) {
        return false;
    }
    if (!receiver || !receiver->valid()) {
        return false;
    }
    const auto result = receiver->take();
    return result.has_value()
        && result->task == id
        && result->reason == reason
        && result->status == status;
}

[[nodiscard]] auto expected_critical_bytes(uint32_t task_index) noexcept
    -> std::optional<uint64_t> {
    if (task_index != 3 && task_index != 4) {
        return std::nullopt;
    }
    const myos::boot::Bundle* const package = runtime.bundle.view();
    if (package == nullptr) {
        return std::nullopt;
    }
    myos::boot::Module child{};
    if (!package->find("child", child) || child.segment_count() != 2) {
        return std::nullopt;
    }
    myos::boot::Segment text{};
    myos::boot::Segment data{};
    if (!child.segment(0, text) || !child.segment(1, data)
        || (text.access & MYOS_BOOT_SEGMENT_EXECUTE) == 0
        || (text.access & MYOS_BOOT_SEGMENT_WRITE) != 0
        || (data.access & MYOS_BOOT_SEGMENT_WRITE) == 0
        || (data.access & MYOS_BOOT_SEGMENT_EXECUTE) != 0
        || text.file_size == 0 || text.memory_size == 0
        || text.memory_size > PageSize
        || data.file_size != sizeof(uint64_t)
        || data.memory_size != 3 * PageSize
        || data.memory_size - data.file_size < 2 * PageSize) {
        return std::nullopt;
    }
    const deploy::PlanTask* const row = runtime.plan.task(task_index);
    if (row == nullptr
        || row->mappings.count != (task_index == 3 ? 4U : 5U)) {
        return std::nullopt;
    }
    const auto mapping = [&](uint32_t local) noexcept
        -> const deploy::PlanMapping* {
        return runtime.plan.mapping(row->mappings.first + local);
    };
    const auto* const code = mapping(0);
    const auto* const data_mapping = mapping(1);
    const auto* const stack = mapping(2);
    const auto* const bootstrap = mapping(3);
    if (code == nullptr || data_mapping == nullptr || stack == nullptr
        || bootstrap == nullptr
        || code->source != DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
        || code->segment != 0 || code->critical != DEPLOY_CRITICAL_CODE
        || data_mapping->source
            != DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
        || data_mapping->segment != 1
        || data_mapping->critical != DEPLOY_CRITICAL_NONE
        || stack->source != DEPLOY_MAPPING_SOURCE_ZERO
        || stack->critical
            != (task_index == 4 ? DEPLOY_CRITICAL_STACK
                                 : DEPLOY_CRITICAL_NONE)
        || bootstrap->source != DEPLOY_MAPPING_SOURCE_ZERO
        || bootstrap->critical != DEPLOY_CRITICAL_BOOTSTRAP) {
        return std::nullopt;
    }
    if (task_index == 4) {
        const auto* const descriptor = mapping(4);
        if (descriptor == nullptr
            || descriptor->source != DEPLOY_MAPPING_SOURCE_ZERO
            || descriptor->critical != DEPLOY_CRITICAL_NONE) {
            return std::nullopt;
        }
    }
    const uint64_t expected = task_index == 3 ? 2 * PageSize : 3 * PageSize;
    if (row->critical_bytes != expected) {
        return std::nullopt;
    }
    return expected;
}

[[nodiscard]] auto run_cut(uint32_t task_index) noexcept -> bool {
    auto plan_lease = runtime.plan.lease();
    if (!plan_lease) {
        return false;
    }
    auto builder_value = Builder::begin(
        runtime.completions,
        runtime.table,
        std::move(*plan_lease),
        task_index);
    if (!builder_value) {
        return false;
    }
    Builder builder = std::move(*builder_value);
    const auto id = builder.record()->id();
    auto receiver = builder.take_receiver();
    if (!receiver) {
        return false;
    }
    deploy::TaskAuthorityBindings bindings{};
    bindings.domains[0] = runtime.domain;
    bindings.imports[0] = runtime.typed_source;
    deploy::TaskConstructionInput<Backend, Authorities> input{
        .parent_pool = runtime.parent.reference(),
        .bundle = &runtime.bundle,
        .scratch = &runtime.scratch,
        .bootstrap = runtime.bootstrap,
        .bootstrap_size = runtime.bootstrap_size,
        .runtime_cpu_count = runtime.cpu_count,
        .bindings = &bindings,
        .workspace = runtime.workspace};
    const myos_status_t constructed = builder.construct(
        input, runtime.authorities);
    static_cast<void>(runtime.console.print<"task-builder-test: cut={} status={}\n">(
        task_index, constructed));
    if (constructed == MYOS_STATUS_NO_MEMORY) {
        const auto* failed = runtime.table.closing(id);
        if (failed != nullptr) {
            static_cast<void>(runtime.console.print<
                "task-builder-test: diag-no-memory accounted={} budget={}\\n">(
                failed->record().accounting().total_bytes,
                failed->record().plan().row()->critical_bytes));
        }
    }

    if (task_index == 4) {
        if (constructed != MYOS_STATUS_OK || !builder.commit_prepared()) {
            return observe_completion(
                id,
                deploy::CloseReason::ConstructionFailure,
                constructed == MYOS_STATUS_OK
                    ? MYOS_STATUS_INTERNAL : constructed,
                builder,
                receiver);
        }
        const auto* prepared = runtime.table.record(id);
        const auto expected = expected_critical_bytes(task_index);
        if (prepared == nullptr
            || prepared->state() != deploy::TaskState::Prepared
            || !expected
            || prepared->accounting().total_bytes != *expected) {
            return false;
        }
        if (!runtime.table.begin_close(
                id, deploy::CloseReason::Explicit, MYOS_STATUS_OK)
            || !drain_task(id)) {
            return false;
        }
        const auto result = receiver->take();
        return result.has_value()
            && result->task == id
            && result->reason == deploy::CloseReason::Explicit
            && result->status == MYOS_STATUS_OK;
    }

    const myos_status_t expected = task_index == 0
        ? MYOS_STATUS_NO_MEMORY : MYOS_STATUS_INVALID_CAP;
    if (constructed != expected) {
        return false;
    }

    if (task_index != 0) {
        const auto* closing = runtime.table.closing(id);
        if (closing == nullptr) {
            runtime.console.text("task-builder-test: diag-no-closing\n");
            return false;
        }
        const bool source_ok = runtime.authorities.lease(runtime.typed_source)
            .has_value();
        runtime.console.text(source_ok
            ? "task-builder-test: diag-source-ok\n"
            : "task-builder-test: diag-source-miss\n");
        if (!source_ok) {
            return false;
        }
        const auto& relation = closing->record().projections().relations[0];
        const bool relation_ok = relation.valid();
        runtime.console.text(relation_ok
            ? "task-builder-test: diag-relation-ok\n"
            : "task-builder-test: diag-relation-miss\n");
        if (task_index == 2 && !relation_ok) {
            return false;
        }
        if (task_index == 3) {
            const auto expected = expected_critical_bytes(task_index);
            if (!expected
                || closing->record().accounting().total_bytes != *expected) {
                return false;
            }
        }
    }
    const bool completion_ok = observe_completion(
            id,
            deploy::CloseReason::ConstructionFailure,
            expected,
            builder,
            receiver);
    runtime.console.text(completion_ok
        ? "task-builder-test: diag-completion-ok\n"
        : "task-builder-test: diag-completion-miss\n");
    if (!completion_ok) {
        return false;
    }
    return runtime.workspace.empty();
}

[[nodiscard]] auto close_source_with_lease() noexcept -> bool {
    auto held = runtime.authorities.lease(runtime.typed_source);
    if (!held || runtime.source.close() != MYOS_STATUS_BUSY) {
        return false;
    }
    if (runtime.authorities.lease(runtime.typed_source)) {
        return false;
    }
    held->release();
    for (size_t attempt = 0; attempt < 64; ++attempt) {
        const myos_status_t status = runtime.source.close();
        if (status == MYOS_STATUS_OK) {
            runtime.source_open = false;
            return true;
        }
        if (!retryable(status)) {
            return false;
        }
        myos::yield();
    }
    return false;
}

[[nodiscard]] auto cleanup(bool publish_success) noexcept -> bool {
    if (runtime.source_open) {
        runtime.console.text("task-builder-test: diag-cleanup-source-start\n");
        if (!close_source_with_lease()) {
            return false;
        }
        runtime.console.text("task-builder-test: diag-cleanup-source-done\n");
    }
    if (runtime.scratch_open) {
        runtime.console.text("task-builder-test: diag-cleanup-scratch-start\n");
        if (runtime.scratch.close() != MYOS_STATUS_OK) {
            return false;
        }
        runtime.scratch_open = false;
        runtime.console.text("task-builder-test: diag-cleanup-scratch-done\n");
    }
    if (runtime.bundle_open) {
        runtime.console.text("task-builder-test: diag-cleanup-bundle-start\n");
        if (runtime.bundle.close() != MYOS_STATUS_OK) {
            return false;
        }
        runtime.bundle_open = false;
        runtime.console.text("task-builder-test: diag-cleanup-bundle-done\n");
    }
    if (runtime.parent_open) {
        runtime.console.text("task-builder-test: diag-cleanup-parent-start\n");
        const myos_status_t closed = Backend::resource_close(
            runtime.parent.reference());
        if (closed != MYOS_STATUS_OK) {
            return false;
        }
        runtime.console.text("task-builder-test: diag-cleanup-parent-close\n");
        if (runtime.parent.close() != MYOS_STATUS_OK) {
            return false;
        }
        runtime.parent_open = false;
        runtime.console.text("task-builder-test: diag-cleanup-parent-done\n");
    }
    if (publish_success) {
        /* This is the sole success marker.  All non-console owners above are
         * exact-closed while the UART borrow is still valid. */
        runtime.console.text("task-builder-test: passed\n");
    }
    if (!runtime.console.port.valid()) {
        return runtime.console.close();
    }
    runtime.console.text("task-builder-test: diag-cleanup-console-start\n");
    return runtime.console.close();
}

[[nodiscard]] auto run(
    const myos::bootstrap::BootstrapView& bootstrap) noexcept -> bool {
    bool complete = setup_views(bootstrap);
    if (complete) {
        complete = decode_plan();
    }
    if (complete) {
        runtime.console.text("task-builder-test: manifest-decoded\n");
        if (!run_cut(0)) {
            complete = false;
        }
        if (complete) {
            runtime.console.text(
                "task-builder-test: budget-probe-ok\n");
        }
        for (uint32_t cut = 1; complete && cut < 5; ++cut) {
            if (!run_cut(cut)) {
                complete = false;
                break;
            }
        }
    }
    if (complete) {
        runtime.console.text("task-builder-test: cuts-ok\n");
    } else if (runtime.console.phase != deploy::LeasePhase::Empty
               && runtime.console.phase != deploy::LeasePhase::Closed) {
        runtime.console.text("task-builder-test: failed\n");
    }
    return cleanup(complete) && complete;
}

} // namespace

extern "C" [[noreturn]] void myos_main(
    const void* bootstrap,
    myos_word_t bootstrap_size) noexcept {
    const auto info = myos::bootstrap::BootstrapView::parse(
        bootstrap, bootstrap_size);
    if (!info || info->bundle_size() == 0) {
        myos::exit();
    }
    if (!run(*info)) {
        Backend::ownership_fault(MYOS_STATUS_INTERNAL);
    }
    myos::exit();
}
