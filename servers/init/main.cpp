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
 * Root init owns platform bootstrap, durable service objects, and the Task
 * records it constructs through the decoded deployment plan.  Every child
 * image still enters through TaskBuilder; the policy below only sequences the
 * real service relations and their checked readiness edges.
 */
namespace {

using Backend = myos::cap::SyscallBackend;
using Space = myos::deploy::TaskSpace<
    myos::deploy::kTaskLocalCapacity,
    myos::deploy::kTaskImportRemoteCapacity,
    Backend>;
using Record = myos::deploy::TaskRecord<Space>;
using Completions = myos::deploy::CompletionSet<3>;
using Table = myos::deploy::TaskTable<Record, Completions, 3>;
using Builder = myos::deploy::TaskBuilder<Table, Completions>;
using Authorities = myos::deploy::AuthoritySet<8>;
using Journal = myos::deploy::RegistrationJournal<8>;
using Workspace = myos::deploy::TaskConstructionWorkspace<Authorities>;
using Plans = myos::deploy::PlanSet<1>;

constexpr myos_word_t PageSize = MYOS_DEPLOY_PAGE_SIZE;
constexpr myos_word_t BundleAddress = 0x1000'0000;
constexpr myos_word_t ScratchAddress = 0x1800'0000;
constexpr myos_word_t UartAddress = 0x3001'0000;
constexpr myos_word_t PagerBackingKey = 1;
constexpr myos_word_t PagerPageLimit = 1;

struct CoreAuthorities final {
    myos::deploy::AuthorityId domain{};
    myos::deploy::AuthorityId bundle{};
    myos::deploy::AuthorityId pager{};
    myos::deploy::AuthorityId device_memory{};
    myos::deploy::AuthorityId irq{};
};

using Receiver = Completions::Receiver;

struct TaskHandle final {
    myos::deploy::TaskId id{};
    libk::optional<Receiver> receiver{};
};

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
    myos::cap::OwnedCap pager{};
    CoreAuthorities core{};
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

[[nodiscard]] auto register_core_authorities(
    myos::cap::CapRef root_pool,
    myos::cap::CapRef root_domain,
    myos::cap::CapRef root_bundle,
    myos::cap::CapRef uart_memory,
    myos::cap::CapRef uart_irq) noexcept -> bool {
    if (!root_pool || !root_domain || !root_bundle || !uart_memory
        || !uart_irq || runtime.pager) {
        return false;
    }

    const myos::SysResult pager = myos::pager_create(
        root_pool.selector, PagerBackingKey, PagerPageLimit);
    if (pager.status != MYOS_STATUS_OK || pager.value == 0) {
        return false;
    }
    runtime.pager = myos::cap::OwnedCap{
        myos::cap::CapRef{pager.value, 0}};

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
    const myos_cap_attenuation pager_ceiling{
        .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
        .kind = MYOS_OBJECT_KIND_PAGER,
        .size = MYOS_CAP_ATTENUATION_SIZE,
        .rights = MYOS_RIGHT_SERVE | MYOS_RIGHT_SUPPLY,
        .words = {PagerPageLimit, 0, 0, 0, 0, 0},
    };
    const myos_cap_attenuation device_ceiling{
        .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
        .kind = MYOS_OBJECT_KIND_MEMORY,
        .size = MYOS_CAP_ATTENUATION_SIZE,
        .rights = MYOS_RIGHT_MAP,
        .words = {0, 1, MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_DEVICE},
    };
    const myos_cap_attenuation irq_ceiling{
        .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
        .kind = MYOS_OBJECT_KIND_IRQ,
        .size = MYOS_CAP_ATTENUATION_SIZE,
        .rights = MYOS_RIGHT_ROUTE | MYOS_RIGHT_OBSERVE | MYOS_RIGHT_ACK,
        .words = {},
    };

    runtime.core.domain = runtime.journal.register_source(
        runtime.authorities,
        root_domain,
        UINT64_C(0x524f4f54444f4d41),
        domain_ceiling)
        .value_or(myos::deploy::AuthorityId{});
    runtime.core.bundle = runtime.journal.register_source(
        runtime.authorities,
        root_bundle,
        UINT64_C(0x524f4f5442554e44),
        bundle_ceiling)
        .value_or(myos::deploy::AuthorityId{});
    runtime.core.pager = runtime.journal.register_source(
        runtime.authorities,
        runtime.pager.reference(),
        UINT64_C(0x4455525041474552),
        pager_ceiling)
        .value_or(myos::deploy::AuthorityId{});
    runtime.core.device_memory = runtime.journal.register_source(
        runtime.authorities,
        uart_memory,
        UINT64_C(0x5541525444455649),
        device_ceiling)
        .value_or(myos::deploy::AuthorityId{});
    runtime.core.irq = runtime.journal.register_source(
        runtime.authorities,
        uart_irq,
        UINT64_C(0x5541525449525145),
        irq_ceiling)
        .value_or(myos::deploy::AuthorityId{});
    return runtime.core.domain.valid() && runtime.core.bundle.valid()
        && runtime.core.pager.valid() && runtime.core.device_memory.valid()
        && runtime.core.irq.valid();
}

template<size_t N>
[[nodiscard]] auto prepare_task(
    const char (&name)[N],
    myos::cap::CapRef parent_pool,
    const myos::deploy::TaskAuthorityBindings& bindings,
    uint32_t runtime_cpu_count) noexcept -> libk::optional<TaskHandle> {
    const auto index = runtime.plan.find_task(name);
    if (!index) {
        return libk::nullopt;
    }
    auto lease = runtime.plan.lease();
    if (!lease) {
        return libk::nullopt;
    }
    auto pending = Builder::begin(
        runtime.completions, runtime.table, libk::move(*lease), *index);
    if (!pending) {
        return libk::nullopt;
    }
    Builder builder = libk::move(*pending);
    const auto* reservation = builder.record();
    if (reservation == nullptr) {
        return libk::nullopt;
    }
    const myos::deploy::TaskId id = reservation->id();
    auto receiver = builder.take_receiver();
    if (!receiver) {
        return libk::nullopt;
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
        return libk::nullopt;
    }
    if (!builder.commit_prepared()) {
        const bool cleaned = myos::deploy::supervision::close_failed(
            runtime.table, id, MYOS_STATUS_INTERNAL, builder, receiver);
        static_cast<void>(cleaned);
        return libk::nullopt;
    }

    return TaskHandle{.id = id, .receiver = libk::move(receiver)};
}

[[nodiscard]] auto start_task(TaskHandle& task) noexcept -> bool {
    const myos_status_t started = runtime.table.start(task.id);
    if (started != MYOS_STATUS_OK) {
        if (!runtime.table.begin_close(
                task.id,
                myos::deploy::CloseReason::ConstructionFailure,
                started)) {
            return false;
        }
        const bool cleaned = myos::deploy::supervision::take_completion(
            runtime.table,
            task.id,
            myos::deploy::CloseReason::ConstructionFailure,
            started,
            task.receiver);
        static_cast<void>(cleaned);
        return false;
    }
    return true;
}

[[nodiscard]] auto wait_readiness(const TaskHandle& task) noexcept -> bool {
    for (;;) {
        const auto* record = runtime.table.record(task.id);
        if (record == nullptr
            || record->state() != myos::deploy::TaskState::Running) {
            return false;
        }
        const myos_status_t status = runtime.table.consume_readiness(task.id);
        if (status == MYOS_STATUS_OK) {
            return true;
        }
        if (status != MYOS_STATUS_RETRY && status != MYOS_STATUS_BUSY) {
            return false;
        }
        myos::yield();
    }
}

[[nodiscard]] auto task_bindings(
    myos::deploy::AuthorityId domain,
    myos::deploy::AuthorityId bundle) noexcept
    -> myos::deploy::TaskAuthorityBindings {
    myos::deploy::TaskAuthorityBindings bindings{};
    bindings.domains[0] = domain;
    bindings.imports[3] = domain;
    bindings.imports[4] = bundle;
    return bindings;
}

[[nodiscard]] auto consumer_bindings() noexcept
    -> myos::deploy::TaskAuthorityBindings {
    auto bindings = task_bindings(
        runtime.core.domain, runtime.core.bundle);
    for (auto& pager : bindings.pagers) {
        pager = runtime.core.pager;
    }
    return bindings;
}

[[nodiscard]] auto pager_bindings(
    myos::deploy::AuthorityId target) noexcept
    -> myos::deploy::TaskAuthorityBindings {
    auto bindings = task_bindings(
        runtime.core.domain, runtime.core.bundle);
    bindings.imports[5] = runtime.core.pager;
    bindings.imports[6] = target;
    return bindings;
}

[[nodiscard]] auto uart_bindings() noexcept
    -> myos::deploy::TaskAuthorityBindings {
    auto bindings = task_bindings(
        runtime.core.domain, runtime.core.bundle);
    bindings.imports[5] = runtime.core.device_memory;
    bindings.imports[6] = runtime.core.irq;
    return bindings;
}

[[nodiscard]] auto deploy_process_server(
    myos::cap::CapRef parent_pool,
    uint32_t runtime_cpu_count) noexcept -> bool {
    auto bindings = task_bindings(
        runtime.core.domain, runtime.core.bundle);
    auto pending = prepare_task(
        "process-server", parent_pool, bindings, runtime_cpu_count);
    if (!pending) {
        return false;
    }
    TaskHandle task = libk::move(*pending);
    runtime.console.text("init: process-server-prepared\n");
    if (!start_task(task)) {
        return false;
    }
    const auto* started_record = runtime.table.record(task.id);
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
            runtime.table, task.id, task.receiver, terminal_status)) {
        return false;
    }
    /* Init supervises process_server as a service boundary; its non-OK
     * terminal is therefore a failed deployment rather than root success. */
    return reached_running && terminal_status == MYOS_STATUS_OK;
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
    const auto uart_irq = bootstrap.cap(MYOS_BOOTSTRAP_CAP_IRQ);
    if (!root_vspace || !root_pool || !root_domain || !root_bundle
        || !uart_memory || !uart_irq
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

    const myos::boot::Bundle* const package = runtime.bundle.view();
    const myos_word_t bundle_window_size = myos::deploy::Window::round_size(
        static_cast<myos_word_t>(runtime.bundle.size()));
    myos_word_t scratch_size{};
    if (package != nullptr && bundle_window_size != 0) {
        for (uint32_t index = 0; index < runtime.plan.task_count(); ++index) {
            const auto lease = runtime.plan.lease();
            const auto task = lease ? lease->task(index)
                                    : myos::deploy::TaskPlanView{};
            if (!task.valid()) {
                return false;
            }
            const auto required = myos::deploy::required_scratch_size(
                task, *package);
            if (!required || *required > scratch_size) {
                scratch_size = required ? *required : 0;
            }
        }
    }
    if (package == nullptr || bundle_window_size == 0 || scratch_size == 0
        || runtime.scratch.open(
               *root_vspace,
               myos::deploy::Window{ScratchAddress, scratch_size},
               myos::deploy::Window{BundleAddress, bundle_window_size})
            != MYOS_STATUS_OK) {
        return false;
    }
    runtime.console.text("init: boot-root\n");
    runtime.console.text("init: manifest-decoded\n");

    if (!register_core_authorities(
            *root_pool, *root_domain, *root_bundle,
            *uart_memory, *uart_irq)) {
        return false;
    }
    runtime.console.text("init: core-authorities\n");

    runtime.console.text("init: process-server-constructing\n");
    if (!deploy_process_server(*root_pool, bootstrap.cpu_count())) {
        return false;
    }

    auto consumer_pending = prepare_task(
        "consumer", *root_pool, consumer_bindings(), bootstrap.cpu_count());
    if (!consumer_pending) {
        return false;
    }
    TaskHandle consumer = libk::move(*consumer_pending);
    runtime.console.text("init: consumer-prepared\n");
    const auto target_authority = runtime.table.register_prepared_export(
        consumer.id, 0, runtime.authorities);
    if (!target_authority) {
        return false;
    }
    runtime.console.text("init: consumer-exported\n");

    auto pager_pending = prepare_task(
        "pager-worker", *root_pool,
        pager_bindings(*target_authority), bootstrap.cpu_count());
    if (!pager_pending) {
        return false;
    }
    TaskHandle pager = libk::move(*pager_pending);
    runtime.console.text("init: pager-prepared\n");
    if (!start_task(pager) || !wait_readiness(pager)) {
        return false;
    }
    runtime.console.text("init: pager-ready\n");

    auto uart_pending = prepare_task(
        "uart-worker", *root_pool, uart_bindings(), bootstrap.cpu_count());
    if (!uart_pending) {
        return false;
    }
    TaskHandle uart = libk::move(*uart_pending);
    runtime.console.text("init: uart-prepared\n");
    if (!start_task(uart) || !wait_readiness(uart)) {
        return false;
    }
    runtime.console.text("init: uart-ready\n");

    /* All image sources have been retired by TaskBuilder before this point;
     * release the two init mappings while every child keeps its own copied
     * MemoryObjects and VSpace projections. */
    if (!close_views()) {
        return false;
    }
    runtime.console.text("init: bundle-views-closed\n");

    if (!start_task(consumer)) {
        return false;
    }
    runtime.console.text("init: consumer-running\n");
    const auto* consumer_record = runtime.table.record(consumer.id);
    if (consumer_record == nullptr
        || consumer_record->accounting().total_bytes
            > consumer_record->plan().row()->critical_bytes) {
        return false;
    }
    myos_status_t terminal_status = MYOS_STATUS_INTERNAL;
    if (!myos::deploy::supervision::observe_and_close(
            runtime.table, consumer.id, consumer.receiver, terminal_status)
        || terminal_status != MYOS_STATUS_OK) {
        return false;
    }
    runtime.console.text("init: consumer-terminal\n");
    return true;
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
