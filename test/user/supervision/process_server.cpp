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
#include <uapi/bootstrap.h>
#include <uapi/object.h>
#include <uapi/resource.h>
#include <uapi/status.h>
#include <uapi/vm.h>

/*
 * The process service has no ambient loader state: its only deployment policy
 * is the typed manifest in the delegated BootBundle.
 * It owns the ordinary Task table and uses the same TaskBuilder transaction
 * as every later user service.
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

constexpr myos_word_t BundleAddress = 0x1000'0000;
constexpr myos_word_t ScratchAddress = 0x1800'0000;

struct Runtime final {
    /* Declaration order is the lifetime contract.  Task records are gone
     * before the plan, the source journal, and the mappings are torn down. */
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
    /* Report service failure through the execution terminal.  A malformed
     * bootstrap or deployment failure must not masquerade as a page fault. */
    myos::exit(status);
}

[[nodiscard]] auto deploy_proof(
    myos::cap::CapRef parent_pool,
    myos::cap::CapRef root_domain,
    myos::cap::CapRef root_bundle,
    uint32_t runtime_cpu_count) noexcept -> bool {
    const auto index = runtime.plan.find_task("proof");
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
        UINT64_C(0x534552564552444f),
        domain_ceiling)
        .value_or(myos::deploy::AuthorityId{});
    bindings.imports[3] = bindings.domains[0];
    bindings.imports[4] = runtime.journal.register_source(
        runtime.authorities,
        root_bundle,
        UINT64_C(0x5345525645524255),
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
    const myos_status_t started = runtime.table.start(id);
    if (started != MYOS_STATUS_OK) {
        if (!runtime.table.begin_close(
                id, myos::deploy::CloseReason::ConstructionFailure,
                started)) {
            return false;
        }
        const bool cleaned = myos::deploy::supervision::take_completion(
            runtime.table, id,
            myos::deploy::CloseReason::ConstructionFailure,
            started, receiver);
        static_cast<void>(cleaned);
        return false;
    }
    const auto* const started_record = runtime.table.record(id);
    if (started_record == nullptr) {
        return false;
    }
    const bool reached_running =
        started_record->state() == myos::deploy::TaskState::Running;
    myos_status_t terminal_status = MYOS_STATUS_INTERNAL;
    if (!myos::deploy::supervision::observe_and_close(
            runtime.table, id, receiver, terminal_status)) {
        return false;
    }
    /* This service owns the ordinary Task policy: a non-OK child terminal is
     * a failed deployment, so the service reports its own explicit failure. */
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
    const auto root_vspace = bootstrap->cap(MYOS_BOOTSTRAP_CAP_VSPACE);
    const auto root_pool = bootstrap->cap(MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const auto root_domain = bootstrap->cap(MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN);
    const auto root_bundle = bootstrap->cap(MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE);
    if (!root_vspace || !root_pool || !root_domain || !root_bundle
        || !open_views(
               *root_vspace, *root_bundle, bootstrap->bundle_size())
        || !myos::deploy::supervision::decode_plan(
               runtime.bundle,
               runtime.manifest_workspace,
               runtime.plans,
               runtime.plan,
               5)) {
        fail(MYOS_STATUS_INTERNAL);
    }

    const auto proof_index = runtime.plan.find_task("proof");
    const auto proof_lease = runtime.plan.lease();
    const myos::deploy::TaskPlanView proof_task =
        proof_index && proof_lease
        ? proof_lease->task(*proof_index)
        : myos::deploy::TaskPlanView{};
    const myos::boot::Bundle* const package = runtime.bundle.view();
    const myos_word_t bundle_window_size = myos::deploy::Window::round_size(
        static_cast<myos_word_t>(runtime.bundle.size()));
    const auto scratch_size = package != nullptr && proof_task.valid()
        ? myos::deploy::required_scratch_size(proof_task, *package)
        : libk::nullopt;
    if (package == nullptr || !proof_task.valid() || bundle_window_size == 0
        || !scratch_size
        || runtime.scratch.open(
               *root_vspace,
               myos::deploy::Window{ScratchAddress, *scratch_size},
               myos::deploy::Window{BundleAddress, bundle_window_size})
            != MYOS_STATUS_OK) {
        fail(MYOS_STATUS_INTERNAL);
    }

    bool complete = deploy_proof(
        *root_pool,
        *root_domain,
        *root_bundle,
        bootstrap->cpu_count());
    if (!close_sources() || !close_views()) {
        complete = false;
    }
    if (!complete) {
        fail(MYOS_STATUS_INTERNAL);
    }
    myos::exit();
}
