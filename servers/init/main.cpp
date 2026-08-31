#include <stddef.h>
#include <stdint.h>

#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <user/lib/bootstrap.hpp>
#include <user/lib/boot_bundle.hpp>
#include <user/lib/deployment_plan.hpp>
#include <user/lib/deployment_syscall.hpp>
#include <user/lib/task_authority.hpp>
#include <user/lib/task_supervision.hpp>
#include <user/lib/task_transaction.hpp>
#include <user/lib/syscall.hpp>
#include <user/lib/uart.hpp>
#include <uapi/bootstrap.h>
#include <uapi/object.h>
#include <uapi/resource.h>
#include <uapi/status.h>
#include <uapi/vm.h>

/*
 * Root init owns only platform bootstrap and one process_server Task record.
 * Ordinary workload state belongs to process_server; this file contains no
 * proof/UART worker loader or second deployment state machine.
 */
namespace {

using Backend = myos::cap::SyscallBackend;
using Space = myos::deploy::TaskSpace<
    myos::deploy::kTaskLocalCapacity,
    myos::deploy::kTaskImportRemoteCapacity,
    Backend>;
using Record = myos::deploy::TaskRecord<Space>;
using Completions = myos::deploy::CompletionSet<1>;
using Table = myos::deploy::TaskTable<Record, Completions, 1>;
using Builder = myos::deploy::TaskBuilder<Table, Completions>;
using Authorities = myos::deploy::AuthoritySet<4>;
using Journal = myos::deploy::RegistrationJournal<4>;
using Workspace = myos::deploy::TaskConstructionWorkspace<Authorities>;
using Plans = myos::deploy::PlanSet<1>;

constexpr myos_word_t PageSize = MYOS_DEPLOY_PAGE_SIZE;
constexpr myos_word_t BundleAddress = 0x1000'0000;
constexpr myos_word_t ScratchAddress = 0x1800'0000;
constexpr myos_word_t UartAddress = 0x3001'0000;

struct Console final {
    myos::cap::OwnedCap region{};
    myos::uart::Port port{0};
    myos::deploy::LeasePhase phase{myos::deploy::LeasePhase::Empty};

    [[nodiscard]] auto open(
        myos::cap::CapRef vspace,
        myos::cap::CapRef memory) noexcept -> bool {
        if ((phase != myos::deploy::LeasePhase::Empty
                && phase != myos::deploy::LeasePhase::Closed)
            || !vspace || !memory || vspace.cspace != 0
            || memory.cspace != 0) {
            return false;
        }
        port = myos::uart::Port{0};
        const myos::SysResult created = myos::vm_create_region(
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
        phase = myos::deploy::LeasePhase::Ready;
        const myos::SysResult mapped = myos::vm_map(
            region.selector(), memory.selector, UartAddress, PageSize, 0,
            MYOS_VM_READ | MYOS_VM_WRITE);
        if (!myos::deploy::committed(mapped.status)) {
            return false;
        }
        phase = myos::deploy::LeasePhase::Mapped;
        port = myos::uart::Port{UartAddress};
        port.initialize();
        return port.valid();
    }

    [[nodiscard]] auto close() noexcept -> bool {
        port = myos::uart::Port{0};
        for (;;) {
            switch (phase) {
            case myos::deploy::LeasePhase::Empty:
            case myos::deploy::LeasePhase::Closed:
                return true;
            case myos::deploy::LeasePhase::Mapped:
            case myos::deploy::LeasePhase::Unmapping: {
                phase = myos::deploy::LeasePhase::Unmapping;
                const myos_status_t status = myos::vm_unmap(
                    region.selector(), UartAddress, PageSize).status;
                if (myos::deploy::committed(status)) {
                    phase = myos::deploy::LeasePhase::Ready;
                    continue;
                }
                if (myos::deploy::retryable(status)) {
                    myos::yield();
                    continue;
                }
                Backend::ownership_fault(status);
            }
            case myos::deploy::LeasePhase::Ready:
            case myos::deploy::LeasePhase::Destroying: {
                phase = myos::deploy::LeasePhase::Destroying;
                const myos_status_t status = myos::vm_destroy_region(
                    region.selector()).status;
                if (myos::deploy::committed(status)) {
                    phase = myos::deploy::LeasePhase::Closing;
                    continue;
                }
                if (myos::deploy::retryable(status)) {
                    myos::yield();
                    continue;
                }
                Backend::ownership_fault(status);
            }
            case myos::deploy::LeasePhase::Closing: {
                const myos_status_t status = region.close();
                if (status == MYOS_STATUS_OK) {
                    region = {};
                    phase = myos::deploy::LeasePhase::Closed;
                    return true;
                }
                if (myos::deploy::retryable(status)) {
                    myos::yield();
                    continue;
                }
                Backend::ownership_fault(status);
            }
            }
        }
    }

    void text(const char* value) noexcept {
        myos::uart::Writer{port}.write(value);
    }
};

struct Runtime final {
    Console console{};
    myos::cap::MappedBundle bundle{};
    myos::cap::ScratchWindow scratch{};
    Authorities authorities{};
    Journal journal{};
    Plans plans{};
    myos::deploy::ManifestWorkspace manifest_workspace{};
    myos::deploy::DeploymentPlan plan{};
    Completions completions{};
    Table table{};
    Workspace workspace{};
};

Runtime runtime{};

[[noreturn]] void fail(myos_status_t status) noexcept {
    /* Root-supervisor setup failures are explicit terminal results, not
     * intentionally triggered user faults. */
    myos::exit(status);
}

[[nodiscard]] auto deploy_process_server(
    myos::cap::CapRef parent_pool,
    myos::cap::CapRef root_domain,
    myos::cap::CapRef root_bundle,
    uint32_t runtime_cpu_count) noexcept -> bool {
    const auto index = runtime.plan.find_task("process-server");
    if (!index) {
        return false;
    }
    auto lease = runtime.plan.lease();
    if (!lease) {
        return false;
    }
    auto pending = Builder::begin(
        runtime.completions, runtime.table, libk::move(*lease), *index);
    if (!pending) {
        return false;
    }
    Builder builder = libk::move(*pending);
    const auto* reservation = builder.record();
    if (reservation == nullptr) {
        return false;
    }
    const myos::deploy::TaskId id = reservation->id();
    auto receiver = builder.take_receiver();
    if (!receiver) {
        return false;
    }

    const myos_cap_attenuation domain_ceiling{
        .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
        .kind = MYOS_OBJECT_KIND_SCHED_DOMAIN,
        .size = MYOS_CAP_ATTENUATION_SIZE,
        .rights = MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONTROL,
        .words = {},
    };
    const myos_cap_attenuation bundle_ceiling{
        .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
        .kind = MYOS_OBJECT_KIND_MEMORY,
        .size = MYOS_CAP_ATTENUATION_SIZE,
        .rights = MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_MAP
            | MYOS_RIGHT_INSPECT,
        .words = {0, 1, MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_NORMAL},
    };
    myos::deploy::TaskAuthorityBindings bindings{};
    bindings.domains[0] = runtime.journal.register_source(
        runtime.authorities,
        root_domain,
        UINT64_C(0x524f4f54444f4d41),
        domain_ceiling)
        .value_or(myos::deploy::AuthorityId{});
    bindings.imports[3] = bindings.domains[0];
    bindings.imports[4] = runtime.journal.register_source(
        runtime.authorities,
        root_bundle,
        UINT64_C(0x524f4f5442554e44),
        bundle_ceiling)
        .value_or(myos::deploy::AuthorityId{});
    if (!bindings.domains[0].valid() || !bindings.imports[4].valid()) {
        const bool cleaned = myos::deploy::supervision::close_failed(
            runtime.table, id, MYOS_STATUS_DENIED, builder, receiver);
        static_cast<void>(cleaned);
        return false;
    }

    myos::deploy::TaskConstructionInput<Backend, Authorities> input{
        .parent_pool = parent_pool,
        .bundle = &runtime.bundle,
        .scratch = &runtime.scratch,
        .bootstrap = nullptr,
        .bootstrap_size = 0,
        .runtime_cpu_count = runtime_cpu_count,
        .bindings = &bindings,
        .workspace = runtime.workspace};
    const myos_status_t constructed = builder.construct(
        input, runtime.authorities);
    if (constructed != MYOS_STATUS_OK) {
        const bool cleaned = myos::deploy::supervision::close_failed(
            runtime.table, id, constructed, builder, receiver);
        static_cast<void>(cleaned);
        return false;
    }
    if (!builder.commit_prepared()) {
        const bool cleaned = myos::deploy::supervision::close_failed(
            runtime.table, id, MYOS_STATUS_INTERNAL, builder, receiver);
        static_cast<void>(cleaned);
        return false;
    }
    runtime.console.text("init: process-server-prepared\n");
    const myos_status_t started = runtime.table.start(id);
    if (started != MYOS_STATUS_OK) {
        if (!runtime.table.begin_close(
                id,
                myos::deploy::CloseReason::ConstructionFailure,
                started)) {
            return false;
        }
        const bool cleaned = myos::deploy::supervision::take_completion(
            runtime.table,
            id,
            myos::deploy::CloseReason::ConstructionFailure,
            started,
            receiver);
        static_cast<void>(cleaned);
        return false;
    }
    const auto* started_record = runtime.table.record(id);
    if (started_record == nullptr) {
        return false;
    }
    const bool reached_running =
        started_record->state() == myos::deploy::TaskState::Running;
    if (reached_running) {
        runtime.console.text("init: process-server-running\n");
    }
    myos_status_t terminal_status = MYOS_STATUS_INTERNAL;
    if (!myos::deploy::supervision::observe_and_close(
            runtime.table, id, receiver, terminal_status)) {
        return false;
    }
    /* Init supervises process_server as a service boundary; its non-OK
     * terminal is therefore a failed deployment rather than root success. */
    return reached_running && terminal_status == MYOS_STATUS_OK;
}

[[nodiscard]] auto close_sources() noexcept -> bool {
    for (;;) {
        const myos_status_t status = runtime.journal.retire_all();
        if (status == MYOS_STATUS_OK) {
            break;
        }
        if (!myos::deploy::retryable(status)) {
            return false;
        }
        myos::yield();
    }
    return runtime.authorities.live_leases() == 0;
}

[[nodiscard]] auto close_views() noexcept -> bool {
    for (;;) {
        const myos_status_t status = runtime.scratch.close();
        if (status == MYOS_STATUS_OK) {
            break;
        }
        if (!myos::deploy::retryable(status)) {
            return false;
        }
        myos::yield();
    }
    for (;;) {
        const myos_status_t status = runtime.bundle.close();
        if (status == MYOS_STATUS_OK) {
            return true;
        }
        if (!myos::deploy::retryable(status)) {
            return false;
        }
        myos::yield();
    }
}

[[nodiscard]] auto open_views(
    myos::cap::CapRef root_vspace,
    myos::cap::CapRef root_bundle,
    uint64_t bundle_size) noexcept -> bool {
    const myos_word_t window_size = myos::deploy::Window::round_size(
        static_cast<myos_word_t>(bundle_size));
    return window_size != 0
        && runtime.bundle.open(
               root_vspace,
               root_bundle,
               myos::deploy::Window{BundleAddress, window_size},
               bundle_size)
            == MYOS_STATUS_OK;
}

[[nodiscard]] auto run(
    const myos::bootstrap::BootstrapView& bootstrap) noexcept -> bool {
    const auto root_vspace = bootstrap.cap(MYOS_BOOTSTRAP_CAP_VSPACE);
    const auto root_pool = bootstrap.cap(MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const auto root_domain = bootstrap.cap(MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN);
    const auto root_bundle = bootstrap.cap(MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE);
    const auto uart_memory = bootstrap.cap(MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY);
    if (!root_vspace || !root_pool || !root_domain || !root_bundle
        || !uart_memory
        || !runtime.console.open(*root_vspace, *uart_memory)
        || !open_views(*root_vspace, *root_bundle, bootstrap.bundle_size())
               || !myos::deploy::supervision::decode_plan(
               runtime.bundle,
               runtime.manifest_workspace,
               runtime.plans,
               runtime.plan,
               5)) {
        return false;
    }

    const auto process_index = runtime.plan.find_task("process-server");
    const auto process_lease = runtime.plan.lease();
    const myos::deploy::TaskPlanView process_task =
        process_index && process_lease
        ? process_lease->task(*process_index)
        : myos::deploy::TaskPlanView{};
    const myos::boot::Bundle* const package = runtime.bundle.view();
    const myos_word_t bundle_window_size = myos::deploy::Window::round_size(
        static_cast<myos_word_t>(runtime.bundle.size()));
    const auto scratch_size = package != nullptr && process_task.valid()
        ? myos::deploy::required_scratch_size(process_task, *package)
        : libk::nullopt;
    if (package == nullptr || !process_task.valid() || bundle_window_size == 0
        || !scratch_size
        || runtime.scratch.open(
               *root_vspace,
               myos::deploy::Window{ScratchAddress, *scratch_size},
               myos::deploy::Window{BundleAddress, bundle_window_size})
            != MYOS_STATUS_OK) {
        return false;
    }
    runtime.console.text("init: boot-root\n");
    runtime.console.text("init: manifest-decoded\n");

    const bool deployed = deploy_process_server(
        *root_pool,
        *root_domain,
        *root_bundle,
        bootstrap.cpu_count());
    if (deployed) {
        runtime.console.text("init: process-server-terminal\n");
    }
    const bool sources_closed = close_sources();
    if (sources_closed) {
        runtime.console.text("init: authorities-retired\n");
    }
    const bool views_closed = close_views();
    if (views_closed) {
        runtime.console.text("init: bundle-views-closed\n");
    }
    return deployed && sources_closed && views_closed;
}

} // namespace

extern "C" [[noreturn]] void myos_main(
    myos_word_t bootstrap_address,
    myos_word_t bootstrap_size) noexcept {
    const auto bootstrap = myos::bootstrap::BootstrapView::parse(
        reinterpret_cast<const void*>(bootstrap_address), bootstrap_size);
    if (!bootstrap || bootstrap->cpu_count() == 0
        || bootstrap->bundle_size() == 0) {
        fail(MYOS_STATUS_BAD_ARGS);
    }
    const bool complete = run(*bootstrap);
    if (!complete) {
        fail(MYOS_STATUS_INTERNAL);
    }
    /* Init is the supervising owner of the root console authority.  Keep its
     * mapping and execution alive after the one-shot service deployment; a
     * later durable-service loop will consume this same owner. */
    runtime.console.text("init: complete\n");
    for (;;) {
        myos::yield();
    }
}
