#pragma once

// User task ownership, construction and same-slot exit collection.

#include <concepts>
#include <algorithm>
#include <array>
#include <stddef.h>
#include <stdint.h>

#include <libk/assert.hpp>
#include <libk/scope_guard.hpp>
#include <libk/checked_arithmetic.hpp>
#include <optional>
#include <utility>
#include <variant>
#include <servers/deploy/format.h>
#include <uapi/start.h>
#include <uapi/ipc.h>
#include <uapi/abi.h>

#include <servers/deploy/format.hpp>
#include <servers/deploy/detail/elf.hpp>
#include <servers/deploy/detail/caps.hpp>
#include <sys/start.hpp>

namespace deploy {

struct TaskId final {
    uint32_t slot{};
    uint32_t generation{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return generation != 0;
    }

    constexpr auto operator==(const TaskId&) const noexcept -> bool = default;
};

enum class TaskState : uint8_t { Building, Running, Closing };

enum class TaskSlotTag : uint8_t {
    Vacant,
    Record,
    Closing,
    Ready,
    Retired,
};

enum class CloseReason : uint16_t {
    ConstructionFailure,
    Exited,
    Explicit,
    SourceRevoked,
    Internal,
};

// Sources stay alive through the synchronous construction call.
struct TaskBindings final {
    CapSrc domains[DEPLOY_TASK_EXECUTION_MAX]{};
    CapSrc pagers[DEPLOY_TASK_MAPPING_MAX]{};
    CapSrc imports[DEPLOY_TASK_IMPORT_MAX]{};
};

/*
 * Bounded, caller-owned scratch for one finite construction.  This is not a
 * second policy or ownership record: every selector is adopted by task
 * immediately, while these arrays only hold materialization
 * metadata until the call returns.  Keeping it outside the construction
 * frame makes the bounded workspace/lifetime contract explicit to the real
 * construction caller.
 */
struct BuildRefs final {
    sys::cap::CapRef vspace{};
    sys::cap::CapRef cspace{};
    sys::cap::CapRef bootstrap{};
    sys::cap::CapRef mappings[DEPLOY_TASK_MAPPING_MAX]{};
    sys::cap::CapRef objects[DEPLOY_TASK_OBJECT_MAX]{};
    /* ChannelMint/Channel construction has two destination selectors for one
     * manifest object.  Keep the second projection parallel to the decoded
     * object row instead of consuming an unrelated adjacent row. */
    sys::cap::CapRef object_b[DEPLOY_TASK_OBJECT_MAX]{};
    sys::cap::CapRef executions[DEPLOY_TASK_EXECUTION_MAX]{};
    sys::cap::CapRef scheduling_contexts[DEPLOY_TASK_EXECUTION_MAX]{};
    sys::cap::CapRef imports[DEPLOY_TASK_IMPORT_MAX]{};
    sys::cap::CapRef relations[DEPLOY_TASK_DEPENDENCY_MAX]{};
};

struct BuildBuf final {
    using image_type = ElfMap<32, 1>;
    BuildRefs refs{};

    // Borrowed construction data only. The synchronous caller serializes reuse.
    void clear() noexcept {
        refs = {};
        image.clear();
        const auto zero = []<typename T, size_t N>(T (&a)[N]) noexcept {
            std::fill_n(a, N, T{});
        };
        zero(image_entries);
        zero(mapping_regions);
        zero(mapping_addresses);
        zero(mapping_sizes);
        zero(mapping_first);
        zero(mapping_access);
        zero(mapping_done);
        zero(imports);
        zero(import_bindings);
        for (auto& bytes : import_descriptor_bytes) zero(bytes);
        import_descriptor = bootstrap_memory = {};
    }

    image_type image{};
    uintptr_t image_entries[DEPLOY_TASK_IMAGE_MAX]{};
    sys::cap::CapRef mapping_regions[DEPLOY_TASK_MAPPING_MAX]{};
    word_t mapping_addresses[DEPLOY_TASK_MAPPING_MAX]{};
    word_t mapping_sizes[DEPLOY_TASK_MAPPING_MAX]{};
    word_t mapping_first[DEPLOY_TASK_MAPPING_MAX]{};
    word_t mapping_access[DEPLOY_TASK_MAPPING_MAX]{};
    bool mapping_done[DEPLOY_TASK_MAPPING_MAX]{};
    sys::cap::CapRef imports[kImportBatchMax]{};
    ImportBinding import_bindings[kImportBatchMax]{};
    sys::cap::CapRef import_descriptor{};
    /* Writable carrier retained only until the generated bootstrap envelope
     * is sealed.  The mapped bootstrap region remains the published child
     * projection after this selector is closed. */
    sys::cap::CapRef bootstrap_memory{};
    uint8_t import_descriptor_bytes[kImportBatchMax][
        CAP_ATTENUATION_SIZE]{};
};

template<typename B>
struct BuildArgs final {
    using workspace_type = BuildBuf;

    sys::cap::CapRef parent_pool{};
    BundleMap<B>* bundle{};
    Map<B>* scratch{};
    const void* bootstrap{};
    size_t bootstrap_size{};
    // Placement and the bootstrap envelope share the actual runtime topology.
    uint32_t runtime_cpu_count{};
    const TaskBindings* bindings{};
    ElfSrc elf_source{};
    const boot::Args* arguments{};
    // Optional observer supplied by a resident supervisor. The accepted
    // selector is moved into task; application imports cannot gain rights
    // beyond that selector. The supervisor waits on its own Notification.
    sys::cap::BasicOwnedCap<B>* exit_notification{};
    workspace_type& workspace;
};

template<typename B>
concept ConstructionBackend = requires(
        sys::cap::CapRef pool,
        sys::cap::CapRef vspace,
        sys::cap::CapRef cspace,
        sys::cap::CapRef descriptor,
        sys::cap::CapRef domain,
        sys::cap::CapRef target,
        sys::cap::CapRef notification,
        word_t words) {
    { B::memory_create_pager(pool, words, words, descriptor) }
        -> std::same_as<sys::SysResult>;
    { B::sc_create(pool, domain, words, words, words, words) }
        -> std::same_as<sys::SysResult>;
    { B::sc_bind(domain, target) } -> std::same_as<status_t>;
    { B::thread_create(pool, vspace, cspace, descriptor, words) }
        -> std::same_as<sys::SysResult>;
    { B::notification_create(pool, words) } -> std::same_as<sys::SysResult>;
    { B::channel_create(pool, words, words, words, words) }
        -> std::same_as<sys::SysResult>;
    { B::pager_create(pool) } -> std::same_as<sys::SysResult>;
    { B::endpoint_create(pool, vspace, cspace, descriptor, words) }
        -> std::same_as<sys::SysResult>;
    { B::exit_bind(target, notification, words) }
        -> std::same_as<status_t>;
};

struct ResidentAccounting final {
    uint64_t total_bytes{};
    uint64_t by_class[DEPLOY_CRITICAL_IPC_HEADER + 1U]{};
};

struct Exit final {
    TaskId task{};
    CloseReason reason{};
    status_t status{};
};

template<typename Backend = sys::cap::SyscallBackend>
class TaskRecord final {
public:
    using backend_type = Backend;
    using owner_type = sys::cap::BasicOwnedCap<Backend>;

    TaskRecord(TaskId id, const Manifest& plan, uint32_t plan_task) noexcept
        : id_(id), plan_{&plan, plan_task} {
        libk_assert(id_.valid() && plan_.valid());
        const auto row = plan_.row();
        exec_count_ = row->execution_count;
        readiness_policy_ = static_cast<deploy_readiness_policy_t>(row->readiness);
    }

    TaskRecord(const TaskRecord&) = delete;
    auto operator=(const TaskRecord&) -> TaskRecord& = delete;

    TaskRecord() noexcept = default;
    TaskRecord(TaskRecord&&) noexcept = default;
    auto operator=(TaskRecord&&) noexcept -> TaskRecord& = default;
    ~TaskRecord() noexcept { if (has_resources()) backend_type::ownership_fault(STATUS_BUSY); }

    [[nodiscard]] constexpr auto id() const noexcept -> TaskId { return id_; }
    [[nodiscard]] constexpr auto state() const noexcept -> TaskState {
        return state_;
    }
    [[nodiscard]] auto readiness() const noexcept -> deploy_readiness_policy_t {
        return readiness_policy_;
    }
    [[nodiscard]] auto ready() const noexcept -> bool {
        if (state_ != TaskState::Running) {
            return false;
        }
        return readiness() == DEPLOY_READINESS_EXPLICIT
            ? readiness_ready_ : true;
    }
    [[nodiscard]] auto plan() const noexcept -> TaskSpec { return plan_; }
    [[nodiscard]] constexpr auto accounting() const noexcept
        -> const ResidentAccounting& {
        return accounting_;
    }
    [[nodiscard]] constexpr auto has_resources() const noexcept -> bool {
        return bool(pool_);
    }

    auto pool() const noexcept -> sys::cap::CapRef { return pool_.reference(); }
    auto cspace() const noexcept -> sys::cap::CapRef { return cspace_.reference(); }
    auto vspace() const noexcept -> sys::cap::CapRef { return vspace_.reference(); }

    auto keep(owner_type&& cap) noexcept -> std::optional<sys::cap::CapRef> {
        if (!cap || !pool_) return std::nullopt;
        const auto ref = cap.reference();
        const auto save = [&](auto& caps) -> bool {
            const auto it = std::find_if(caps.begin(), caps.end(), [](const auto& p) { return !p; });
            if (it == caps.end()) return false;
            *it = std::move(cap);
            return true;
        };
        if (ref.cspace != 0 && ref.cspace != cspace_.selector()) return std::nullopt;
        return (ref.cspace == 0 ? save(owned_) : save(imported_))
            ? std::optional{ref} : std::nullopt;
    }
    auto drop(sys::cap::CapRef cap) noexcept -> status_t {
        for (auto& owner : owned_)
            if (owner.reference() == cap && owner) return owner.close();
        for (auto& owner : imported_)
            if (owner.reference() == cap && owner) return owner.close();
        return STATUS_INVALID_CAP;
    }
    auto open(sys::cap::CapRef parent, word_t memory, word_t caps, word_t kinds,
              word_t slots, word_t pages) noexcept -> status_t {
        if (pool_) return STATUS_BUSY;
        const auto group = backend_type::resource_create_child(parent, memory, caps, kinds);
        if (group.status != STATUS_OK || !group.value)
            return group.status == STATUS_OK ? STATUS_INVALID_CAP : group.status;
        pool_ = owner_type{{group.value, 0}};
        group_state_ = GroupState::Live;
        const auto vm = backend_type::vspace_create(pool());
        if (vm.value) vspace_ = owner_type{{vm.value, 0}};
        if (vm.status != STATUS_OK || !vm.value)
            return vm.status == STATUS_OK ? STATUS_INVALID_CAP : vm.status;
        const auto cs = backend_type::cspace_create(pool(), slots, pages);
        if (cs.value) cspace_ = owner_type{{cs.value, 0}};
        return cs.status == STATUS_OK && !cs.value ? STATUS_INVALID_CAP : cs.status;
    }
    auto close() noexcept -> status_t {
        if (!pool_) return STATUS_OK;
        // Child selectors need their parent-held CSpace capability until closed.
        for (auto& cap : imported_) {
            const auto status = cap.close();
            if (status != STATUS_OK) return status;
        }
        auto status = cspace_.close();
        if (status != STATUS_OK) return status;
        for (auto& cap : owned_) {
            status = cap.close();
            if (status != STATUS_OK) return status;
        }
        status = vspace_.close();
        if (status != STATUS_OK) return status;
        if (group_state_ == GroupState::Live) {
            if constexpr (requires { backend_type::resource_close_async(pool(), close_events_, close_badge_); }) {
                if (close_events_) {
                    status = backend_type::resource_close_async(pool(), close_events_, close_badge_);
                    if (status != STATUS_OK) return status;
                    group_state_ = GroupState::Waiting;
                } else {
                    status = backend_type::resource_close(pool());
                    if (status != STATUS_OK) return status;
                    group_state_ = GroupState::Drained;
                }
            } else {
                status = backend_type::resource_close(pool());
                if (status != STATUS_OK) return status;
                group_state_ = GroupState::Drained;
            }
        }
        if (group_state_ == GroupState::Waiting) return STATUS_BUSY;
        return pool_.close();
    }
    void close_events(sys::cap::CapRef events, word_t badge) noexcept {
        close_events_ = events;
        close_badge_ = badge;
    }
    void observe_close(word_t badges) noexcept {
        if (group_state_ == GroupState::Waiting && (badges & close_badge_))
            group_state_ = GroupState::Drained;
    }

    static void ownership_fault(status_t status) noexcept {
        backend_type::ownership_fault(status);
    }

    // Construction is synchronous; partial starts remain owned on failure.
    [[nodiscard]] auto start() noexcept -> status_t {
        if (state_ != TaskState::Building) return STATUS_BUSY;
        if (exec_count_ == 0 || exec_count_ > threads_.size()) return STATUS_BAD_ARGS;
        for (uint32_t i = 0; i < exec_count_; ++i)
            if (!threads_[i]) return STATUS_INVALID_CAP;
        for (uint32_t i = 0; i < exec_count_; ++i) {
            const auto result = backend_type::execution_start(threads_[i]);
            if (result.status != STATUS_OK) return result.status;
        }
        state_ = TaskState::Running;
        plan_ = {};
        return STATUS_OK;
    }

private:
    [[nodiscard]] auto mutable_accounting() noexcept -> ResidentAccounting& {
        return accounting_;
    }

    template<typename B = backend_type>
    requires requires(sys::cap::CapRef notification) {
        { B::notification_take(notification) } -> std::same_as<sys::SysResult>;
    }
    [[nodiscard]] auto consume_readiness() noexcept -> status_t {
        if (readiness() != DEPLOY_READINESS_EXPLICIT
            || state_ != TaskState::Running) {
            return STATUS_BUSY;
        }
        if (readiness_ready_) {
            return STATUS_RETRY;
        }
        const auto notification = readiness_;
        if (!notification) {
            return STATUS_INVALID_CAP;
        }
        const sys::SysResult result = B::notification_take(notification);
        if (result.status != STATUS_OK) {
            return result.status;
        }
        if (result.value == 0) {
            return STATUS_RETRY;
        }
        readiness_ready_ = true;
        return STATUS_OK;
    }

    [[nodiscard]] auto exit_notification() const noexcept
        -> std::optional<sys::cap::CapRef> {
        if (state_ != TaskState::Running) {
            return std::nullopt;
        }
        if (exec_count_ != 1) {
            return std::nullopt;
        }
        const sys::cap::CapRef& relation = exit_event_;
        if (!bool(relation)) {
            return std::nullopt;
        }
        return relation;
    }

public:
    template<typename B = backend_type>
    requires requires(sys::cap::CapRef target) {
        { B::exit_query(target) } -> std::same_as<sys::SysResult>;
    }
    [[nodiscard]] auto observe_exit() const noexcept -> sys::SysResult {
        if (state_ != TaskState::Running) {
            return sys::SysResult{.status = STATUS_BUSY};
        }
        if (exec_count_ != 1) {
            /* The current production envelope is single-execution.  A
             * multi-execution aggregation policy needs its own explicit
             * exit contract and is not guessed here. */
            return sys::SysResult{.status = STATUS_BAD_ARGS};
        }
        const auto target = threads_[0];
        if (!target) {
            return sys::SysResult{.status = STATUS_INVALID_CAP};
        }
        return B::exit_query(target);
    }

private:
    [[nodiscard]] auto export_cap(uint32_t index) const noexcept -> std::optional<CapSrc> {
        if (index >= DEPLOY_TASK_EXPORT_MAX
            || state_ != TaskState::Running) return std::nullopt;
        const auto& entry = exports_[index];
        return entry.valid() ? std::optional{entry} : std::nullopt;
    }

private:
    template<typename, size_t, uint32_t>
    friend class TaskTable;
    TaskId id_{};
    TaskState state_{TaskState::Building};
    TaskSpec plan_{};
    uint32_t exec_count_{};
    deploy_readiness_policy_t readiness_policy_{};
    owner_type pool_{}, cspace_{}, vspace_{};
    std::array<owner_type, kTaskLocalCapacity> owned_{};
    std::array<owner_type, DEPLOY_TASK_IMPORT_MAX> imported_{};
    std::array<sys::cap::CapRef, DEPLOY_TASK_EXECUTION_MAX> threads_{};
    sys::cap::CapRef exit_event_{};
    enum class GroupState : uint8_t { Live, Waiting, Drained };
    GroupState group_state_{};
    sys::cap::CapRef close_events_{};
    word_t close_badge_{};
    std::array<CapSrc, DEPLOY_TASK_EXPORT_MAX> exports_{};
    // Signal capability goes to the child; only the supervisor consumes this event.
    sys::cap::CapRef readiness_{};
    ResidentAccounting accounting_{};
    bool readiness_ready_{};
    Exit exit_{};
};

// A slot owns its task from first allocation through teardown. An unread
// result occupies the same generation; exhausted generations are never reused.
template<typename Record, size_t Capacity = DEPLOY_TASK_MAX,
         uint32_t GenerationLimit = UINT32_MAX>
class TaskTable final {
    static_assert(Capacity != 0 && GenerationLimit != 0);
    using record_type = Record;
    using backend_type = typename Record::backend_type;
    using owner_type = typename Record::owner_type;
    struct Slot {
        uint32_t generation{1};
        std::variant<std::monostate, Record, Exit> payload;
    };
    Slot slots_[Capacity]{};

    auto slot(TaskId id) noexcept -> Slot* {
        return id.valid() && id.slot < Capacity && slots_[id.slot].generation == id.generation
            ? &slots_[id.slot] : nullptr;
    }
    auto slot(TaskId id) const noexcept -> const Slot* {
        return id.valid() && id.slot < Capacity && slots_[id.slot].generation == id.generation
            ? &slots_[id.slot] : nullptr;
    }
    auto task(TaskId id) noexcept -> Record* {
        auto* s = slot(id);
        return s ? std::get_if<Record>(&s->payload) : nullptr;
    }
    auto task(TaskId id) const noexcept -> const Record* {
        const auto* s = slot(id);
        return s ? std::get_if<Record>(&s->payload) : nullptr;
    }

public:
    struct Spawn { TaskId id{}; status_t status{STATUS_BUSY}; };
    TaskTable() noexcept = default;
    TaskTable(const TaskTable&) = delete;
    auto operator=(const TaskTable&) -> TaskTable& = delete;
    ~TaskTable() noexcept {
        for (const auto& s : slots_)
            if (!std::holds_alternative<std::monostate>(s.payload)) Record::ownership_fault(STATUS_BUSY);
    }

    auto spawn(const Manifest& plan, uint32_t index, const BuildArgs<backend_type>& args,
               sys::cap::CapRef events = {}, word_t badge = 0) noexcept -> Spawn {
        const TaskSpec spec{&plan, index};
        if (!spec.valid() || !imports_admissible(spec)) return {{}, STATUS_BAD_ARGS};
        for (uint32_t i = 0; i < Capacity; ++i) {
            auto& s = slots_[i];
            if (!s.generation || !std::holds_alternative<std::monostate>(s.payload)) continue;
            const TaskId id{i, s.generation};
            auto& r = s.payload.template emplace<Record>(id, plan, index);
            r.close_events(events, badge);
            auto status = build(r, args);
            if (status == STATUS_OK) status = r.start();
            if (status != STATUS_OK) {
                (void)begin_close(id, CloseReason::ConstructionFailure, status);
                // No hidden drain loop. Even an immediate close retains Exit
                // for the caller to consume before this slot can be reused.
                (void)continue_close(id);
            }
            return {id, status};
        }
        return {};
    }
    auto tag(TaskId id) const noexcept -> TaskSlotTag {
        const auto* s = slot(id);
        if (!s) return TaskSlotTag::Retired;
        if (std::holds_alternative<Exit>(s->payload)) return TaskSlotTag::Ready;
        if (const auto* r = std::get_if<Record>(&s->payload))
            return r->state_ == TaskState::Closing ? TaskSlotTag::Closing : TaskSlotTag::Record;
        return TaskSlotTag::Vacant;
    }
    auto observe_exit(TaskId id) const noexcept -> sys::SysResult {
        const auto* r = task(id);
        return r ? r->observe_exit() : sys::SysResult{.status = STATUS_INVALID_CAP};
    }
    auto exit_notification(TaskId id) const noexcept -> std::optional<sys::cap::CapRef> {
        const auto* r = task(id);
        return r ? r->exit_notification() : std::nullopt;
    }
    auto consume_readiness(TaskId id) noexcept -> status_t {
        auto* r = task(id);
        return r ? r->consume_readiness() : STATUS_INVALID_CAP;
    }
    auto ready(TaskId id) const noexcept -> bool {
        const auto* r = task(id);
        return r && r->ready();
    }
    auto export_cap(TaskId id, uint32_t index) const noexcept -> std::optional<CapSrc> {
        const auto* r = task(id);
        return r ? r->export_cap(index) : std::nullopt;
    }
    auto begin_close(TaskId id, CloseReason reason, status_t status) noexcept -> bool {
        auto* r = task(id);
        if (!r || r->state_ == TaskState::Closing) return false;
        r->exit_ = {id, reason, status};
        r->plan_ = {};
        r->state_ = TaskState::Closing;
        return true;
    }
    auto continue_close(TaskId id) noexcept -> status_t {
        auto* s = slot(id);
        auto* r = task(id);
        if (!r || r->state_ != TaskState::Closing) return STATUS_INVALID_CAP;
        const auto status = r->close();
        if (status != STATUS_OK) return status;
        const auto result = r->exit_;
        s->payload.template emplace<Exit>(result);
        return STATUS_OK;
    }
    auto result(TaskId id) const noexcept -> std::optional<Exit> {
        const auto* s = slot(id);
        if (s) if (const auto* e = std::get_if<Exit>(&s->payload)) return *e;
        return std::nullopt;
    }
    auto reap(TaskId id) noexcept -> std::optional<Exit> {
        const auto e = result(id);
        if (!e) return std::nullopt;
        auto& s = slots_[id.slot];
        s.payload.template emplace<std::monostate>();
        s.generation = s.generation == GenerationLimit ? 0 : s.generation + 1;
        return e;
    }
    void observe_close(TaskId id, word_t badges) noexcept {
        if (auto* r = task(id)) r->observe_close(badges);
    }
    auto close_waiting(TaskId id) const noexcept -> bool {
        const auto* r = task(id);
        return r && r->group_state_ == Record::GroupState::Waiting;
    }

private:
    [[nodiscard]] static auto build(
        Record& record, const BuildArgs<backend_type>& input) noexcept -> status_t
        requires ConstructionBackend<backend_type> {
        if (!input.bindings || input.runtime_cpu_count == 0
            || !input.scratch || !input.parent_pool) {
            return STATUS_BAD_ARGS;
        }
        auto& workspace = input.workspace;
        libk::scope_exit cleanup{[&]() noexcept { workspace.clear(); }};

        const TaskSpec task = record.plan();
        const auto row = task.row();
        if (!row) {
            return STATUS_BAD_ARGS;
        }
        /* Bootstrap rows own the envelope contents.  A caller-provided byte
         * snapshot is opaque and cannot coexist with that policy. */
        if (row->bootstrap_count != 0
            && (bool(input.bootstrap) || input.bootstrap_size != 0)) {
            return STATUS_BAD_ARGS;
        }

        bool typed_imports{};
        for (uint32_t i = 0; i < row->import_count; ++i)
            typed_imports |= task.import(i)->mode == DEPLOY_IMPORT_TYPED_DELEGATE;

        /* Plan references are global decoded-table indices while the typed
         * TaskSpec accessors intentionally take a task-local index.  Keep
         * the conversion at this construction boundary so every later
         * projection lookup addresses the same row that policy validated. */
        const auto mapping_local = [row](uint32_t reference) noexcept
            -> uint32_t {
            if (reference == DEPLOY_NO_INDEX
                || reference < row->mapping_first
                || reference - row->mapping_first >= row->mapping_count) {
                return DEPLOY_NO_INDEX;
            }
            return reference - row->mapping_first;
        };
        const uint32_t bootstrap_mapping =
            mapping_local(row->bootstrap_mapping);

        for (uint32_t i = 0; i < row->execution_count; ++i) {
            const auto& src = input.bindings->domains[i];
            if (!src.valid() || src.limit.kind != OBJECT_KIND_SCHED_DOMAIN) return STATUS_DENIED;
        }
        for (uint32_t i = 0; i < row->mapping_count; ++i) {
            if (task.mapping(i)->source != DEPLOY_MAPPING_SOURCE_PAGER) continue;
            const auto& src = input.bindings->pagers[i];
            if (!src.valid() || src.limit.kind != OBJECT_KIND_PAGER) return STATUS_DENIED;
        }
        if (!input.bundle || input.bundle->phase() != MapState::Mapped) {
            return STATUS_BAD_ARGS;
        }
        const status_t opened_status = record.open(
                input.parent_pool,
                static_cast<word_t>(row->pool_memory),
                static_cast<word_t>(row->pool_caps),
                static_cast<word_t>(row->kind_mask),
                static_cast<word_t>(row->cspace_slots),
                static_cast<word_t>(row->cspace_pages));
        if (opened_status != STATUS_OK) return opened_status;

        auto& projections = workspace.refs;
        projections.vspace = record.vspace();
        projections.cspace = record.cspace();
        const auto pool = record.pool();
        const auto vspace = projections.vspace;
        const auto cspace = projections.cspace;
        if (!pool || !vspace || !cspace) {
            return STATUS_INVALID_CAP;
        }

        using Materializer = ElfLoader<
            record_type,
            32,
            1>;
        using Image = typename Materializer::Image;
        Materializer materializer{
            record, *input.bundle, *input.scratch, input.elf_source};
        Image& image = workspace.image;
        uintptr_t (&image_entries)[DEPLOY_TASK_IMAGE_MAX] =
            workspace.image_entries;
        sys::cap::CapRef (&mapping_regions)[DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_regions;
        word_t (&mapping_addresses)[DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_addresses;
        word_t (&mapping_sizes)[DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_sizes;
        auto& mapping_first = workspace.mapping_first;
        word_t (&mapping_access)[DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_access;
        bool (&mapping_done)[DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_done;

        if (typed_imports) {
            const status_t status = materializer.materialize_descriptor(
                &workspace.import_descriptor_bytes[0][0],
                sizeof(workspace.import_descriptor_bytes),
                workspace.import_descriptor);
            if (status != STATUS_OK) {
                return status;
            }
        }

        const auto close_owner = [&](owner_type& owner) noexcept
            -> status_t {
            if (!owner) {
                return STATUS_OK;
            }
            const status_t status = owner.close();
            if (status != STATUS_OK) {
                backend_type::ownership_fault(status);
            }
            return status;
        };
        const auto adopt_result = [&](sys::SysResult result,
                                      obj_kind_t,
                                      sys::cap::CapRef& output) noexcept
            -> status_t {
            output = {};
            if (result.value == 0) {
                return result.status == STATUS_OK
                    ? STATUS_INVALID_CAP : result.status;
            }
            owner_type owner{sys::cap::CapRef{result.value, 0}};
            if (result.status != STATUS_OK) {
                static_cast<void>(close_owner(owner));
                return result.status;
            }
            const auto slot = record.keep(std::move(owner));
            if (!slot) {
                static_cast<void>(close_owner(owner));
                return STATUS_NO_MEMORY;
            }
            output = *slot;
            return STATUS_OK;
        };

        /* Pre-mapping local objects have no references to mappings. */
        sys::cap::CapRef relation_notification{};
        word_t relation_badge{};
        const auto role_source = [&](uint32_t role,
                                     ByteView& source) noexcept -> bool {
            source = {};
            size_t role_count = 0;
            for (uint32_t index = 0; index < row->bootstrap_count; ++index) {
                const auto bootstrap_row = task.bootstrap(index);
                if (!bootstrap_row || bootstrap_row->kind != role) {
                    continue;
                }
                ++role_count;
                const ByteView destination =
                    task.string(bootstrap_row->destination);
                size_t matches = 0;
                for (uint32_t import_index = 0;
                     import_index < row->import_count; ++import_index) {
                    const auto import = task.import(import_index);
                    if (!import
                        || import->source_class
                            != DEPLOY_IMPORT_SOURCE_TASK_KEY
                        || !task.string(import->destination).equals(destination)) {
                        continue;
                    }
                    source = task.string(import->source);
                    ++matches;
                }
                if (matches != 1) {
                    return false;
                }
            }
            return role_count <= 1;
        };
        ByteView service_key{};
        ByteView readiness_key{};
        if (!role_source(BOOT_EVENTS,
                         service_key)
            || !role_source(BOOT_READY,
                            readiness_key)) {
            return STATUS_BAD_ARGS;
        }
        for (uint32_t index = 0; index < row->object_count; ++index) {
            const auto object = task.object(index);
            if (!object) {
                return STATUS_BAD_ARGS;
            }
            if (object->kind == OBJECT_KIND_ENDPOINT
                || (object->flags & DEPLOY_OBJECT_POST_MAPPING) != 0) {
                continue;
            }
            sys::cap::CapRef slot{};
            status_t status = STATUS_BAD_ARGS;
            switch (object->kind) {
            case OBJECT_KIND_NOTIFICATION: {
                const auto key = task.string(object->output);
                if (bool(input.exit_notification) && !key.equals(service_key)
                    && !key.equals(readiness_key)) {
                    if (!*input.exit_notification) return STATUS_BAD_ARGS;
                    const auto adopted = record.keep(std::move(*input.exit_notification));
                    if (!adopted) return STATUS_NO_MEMORY;
                    slot = *adopted;
                    status = STATUS_OK;
                    break;
                }
                status = adopt_result(
                    backend_type::notification_create(
                        pool, object->args[0]),
                    object->kind, slot);
                break;
            }
            case OBJECT_KIND_CHANNEL: {
                const sys::SysResult created = backend_type::channel_create(
                    pool, object->args[0], object->args[1],
                    object->args[2], object->args[3]);
                owner_type first{};
                owner_type second{};
                if (created.value != 0) {
                    first = owner_type{sys::cap::CapRef{created.value, 0}};
                }
                if (created.value2 != 0) {
                    second = owner_type{sys::cap::CapRef{created.value2, 0}};
                }
                if (created.status != STATUS_OK
                    || !first || !second) {
                    static_cast<void>(close_owner(first));
                    static_cast<void>(close_owner(second));
                    status = created.status == STATUS_OK
                        ? STATUS_INVALID_CAP : created.status;
                    break;
                }
                const auto first_slot = record.keep(std::move(first));
                if (!first_slot) {
                    static_cast<void>(close_owner(first));
                    static_cast<void>(close_owner(second));
                    status = STATUS_NO_MEMORY;
                    break;
                }
                const auto second_slot = record.keep(std::move(second));
                if (!second_slot) {
                    static_cast<void>(close_owner(second));
                    status = STATUS_NO_MEMORY;
                    break;
                }
                projections.objects[index] = *first_slot;
                projections.object_b[index] = *second_slot;
                status = STATUS_OK;
                break;
            }
            case OBJECT_KIND_PAGER:
                status = adopt_result(
                    backend_type::pager_create(
                        pool),
                    object->kind, slot);
                break;
            default:
                status = STATUS_BAD_ARGS;
                break;
            }
            if (status != STATUS_OK) {
                return status;
            }
            if (object->kind != OBJECT_KIND_CHANNEL) {
                projections.objects[index] = slot;
            }
        }

        /* The thread exit relation is the one Notification not named
         * by a service/readiness bootstrap role.  This derives the relation
         * from the manifest graph rather than from object-row order. */
        for (uint32_t index = 0; index < row->object_count; ++index) {
            const auto object = task.object(index);
            if (!object || object->kind != OBJECT_KIND_NOTIFICATION
                || !bool(projections.objects[index])) {
                continue;
            }
            const ByteView output = task.string(object->output);
            if ((service_key.size() != 0 && output.equals(service_key))
                || (readiness_key.size() != 0 && output.equals(readiness_key))) {
                continue;
            }
            if (bool(relation_notification)) {
                return STATUS_BAD_ARGS;
            }
            relation_notification = projections.objects[index];
            relation_badge = object->args[0];
        }
        if (row->execution_count != 0 && !bool(relation_notification)) {
            return STATUS_BAD_ARGS;
        }

        /* Resolve and install every mapping exactly once. */
        for (uint32_t image_index = 0; image_index < row->image_count;
             ++image_index) {
            const auto image_row = task.image(image_index);
            if (!image_row) {
                return STATUS_BAD_ARGS;
            }
            const ByteView name = task.string(image_row->source);
            const status_t materialized_status =
                materializer.materialize(name, image);
            if (materialized_status != STATUS_OK) {
                return materialized_status;
            }
            image_entries[image_index] = image.entry;
            for (size_t segment = 0; segment < image.segments.size();
                 ++segment) {
                size_t matches = 0;
                for (uint32_t mapping_index = 0;
                     mapping_index < row->mapping_count; ++mapping_index) {
                    const auto mapping =
                        task.mapping(mapping_index);
                    if (!mapping
                        || mapping->source
                            != DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
                        || mapping->image
                            != row->image_first + image_index
                        || mapping->segment != segment) {
                        continue;
                    }
                    ++matches;
                    if (matches != 1 || mapping_done[mapping_index]) {
                        return STATUS_BAD_ARGS;
                    }
                    projections.mappings[mapping_index] = image.segments[segment].memory;
                    mapping_regions[mapping_index] =
                        image.segments[segment].region;
                    mapping_addresses[mapping_index] =
                        static_cast<word_t>(
                            image.segments[segment].address);
                    mapping_sizes[mapping_index] = image.segments[segment].size;
                    mapping_first[mapping_index] = image.segments[segment].first;
                    mapping_access[mapping_index] =
                        image.segments[segment].access;
                    mapping_done[mapping_index] = true;
                }
                if (matches != 1) {
                    return STATUS_BAD_ARGS;
                }
            }
            image.clear();
        }

        for (uint32_t mapping_index = 0;
             mapping_index < row->mapping_count; ++mapping_index) {
            if (mapping_done[mapping_index]) {
                continue;
            }
            const auto mapping = task.mapping(mapping_index);
            if (!mapping) {
                return STATUS_BAD_ARGS;
            }
            typename Image::Mapping materialized{};
            status_t status = STATUS_BAD_ARGS;
            if (mapping_index == bootstrap_mapping) {
                if ((!input.bootstrap && input.bootstrap_size != 0)
                    || input.bootstrap_size > mapping->size) {
                    return STATUS_BAD_ARGS;
                }
                status = materializer.materialize_zero(
                    static_cast<word_t>(mapping->address),
                    static_cast<word_t>(mapping->size),
                    VM_READ,
                    materialized);
                if (status == STATUS_OK && bool(input.bootstrap)
                    && input.bootstrap_size != 0) {
                    status = materializer.write(
                        materialized.memory,
                        static_cast<word_t>(mapping->size),
                        0,
                        input.bootstrap,
                        input.bootstrap_size);
                }
                /* A generated production envelope is populated after Imports,
                 * so retain this writable MemoryObject until that point.  A
                 * caller-supplied opaque record remains closed here. */
                if (status == STATUS_OK && row->bootstrap_count == 0) {
                    status = record.drop(materialized.memory);
                    materialized.memory = {};
                } else if (status == STATUS_OK) {
                    workspace.bootstrap_memory = materialized.memory;
                }
            } else if (mapping->source == DEPLOY_MAPPING_SOURCE_ZERO) {
                status = materializer.materialize_zero(
                    static_cast<word_t>(mapping->address),
                    static_cast<word_t>(mapping->size),
                    static_cast<word_t>(mapping->access),
                    materialized);
            } else if (mapping->source == DEPLOY_MAPPING_SOURCE_PAGER) {
                if (!input.bindings->pagers[mapping_index].valid()) {
                    return STATUS_INVALID_CAP;
                }
                status = materializer.materialize_paged(
                    input.bindings->pagers[mapping_index].cap,
                    static_cast<word_t>(mapping->address),
                    static_cast<word_t>(mapping->size),
                    static_cast<word_t>(mapping->access),
                    materialized);
            }
            if (status != STATUS_OK || !bool(materialized.region)) {
                return status == STATUS_OK
                    ? STATUS_INVALID_CAP : status;
            }
            projections.mappings[mapping_index] = bool(materialized.memory)
                ? materialized.memory
                : materialized.region;
            mapping_regions[mapping_index] = materialized.region;
            mapping_addresses[mapping_index] = materialized.address;
            mapping_sizes[mapping_index] = materialized.size;
            mapping_first[mapping_index] = materialized.first;
            mapping_access[mapping_index] = materialized.access;
            mapping_done[mapping_index] = true;
            if (mapping_index == bootstrap_mapping) {
                projections.bootstrap = projections.mappings[mapping_index];
            }
        }
        for (uint32_t mapping_index = 0;
             mapping_index < row->mapping_count; ++mapping_index) {
            if (!mapping_done[mapping_index]) {
                return STATUS_BAD_ARGS;
            }
        }

        ResidentAccounting accounting{};
        for (uint32_t mapping_index = 0;
             mapping_index < row->mapping_count; ++mapping_index) {
            const auto mapping = task.mapping(mapping_index);
            if (!mapping || mapping->critical
                    > DEPLOY_CRITICAL_IPC_HEADER) {
                return STATUS_BAD_ARGS;
            }
            if (mapping->critical == DEPLOY_CRITICAL_NONE) {
                continue;
            }
            const auto total = libk::checked_add(
                accounting.total_bytes,
                static_cast<uint64_t>(mapping_sizes[mapping_index]));
            const auto class_total = libk::checked_add(
                accounting.by_class[mapping->critical],
                static_cast<uint64_t>(mapping_sizes[mapping_index]));
            if (!total || !class_total) {
                return STATUS_BAD_ARGS;
            }
            accounting.total_bytes = *total;
            accounting.by_class[mapping->critical] = *class_total;
            const sys::cap::CapRef& projection =
                projections.mappings[mapping_index];
            if (projection && projection != mapping_regions[mapping_index]) {
                for (uint32_t prior = 0; prior < mapping_index; ++prior) {
                    const sys::cap::CapRef& previous =
                        projections.mappings[prior];
                    if (previous == projection) {
                        return STATUS_BAD_ARGS;
                    }
                }
            }
        }
        if (accounting.total_bytes > row->critical_bytes) {
            return STATUS_NO_MEMORY;
        }
        record.mutable_accounting() = accounting;

        /* Post-mapping Endpoint descriptors are snapshots into a zero mapping
         * and are constructed only after all image/stack mappings exist. */
        for (uint32_t object_index = 0; object_index < row->object_count;
             ++object_index) {
            const auto object = task.object(object_index);
            if (!object || object->kind != OBJECT_KIND_ENDPOINT) {
                continue;
            }
            const uint32_t descriptor_mapping = mapping_local(object->refs[0]);
            if (descriptor_mapping == DEPLOY_NO_INDEX) {
                return STATUS_BAD_ARGS;
            }
            const auto descriptor_row =
                task.mapping(descriptor_mapping);
            const auto descriptor_memory = projections.mappings[descriptor_mapping];
            if (!descriptor_row || !descriptor_memory
                || object->args[0] > descriptor_row->size
                || sizeof(EpDesc)
                    > descriptor_row->size - object->args[0]
                || row->execution_count != 1) {
                return STATUS_BAD_ARGS;
            }
            const auto execution = task.execution(0);
            if (!execution) {
                return STATUS_BAD_ARGS;
            }
            if (execution->image < row->image_first
                || execution->image >= row->image_first + row->image_count) {
                return STATUS_BAD_ARGS;
            }
            const size_t execution_image_index =
                execution->image - row->image_first;
            const uintptr_t execution_entry = execution->entry != 0
                ? static_cast<uintptr_t>(execution->entry)
                : image_entries[execution_image_index];
            const auto code_mapping = [&]() noexcept
                -> uint32_t {
                for (uint32_t index = 0; index < row->mapping_count;
                     ++index) {
                    const auto candidate = task.mapping(index);
                    if (bool(candidate)
                        && candidate->source
                            == DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
                        && candidate->image == execution->image
                        && (mapping_access[index] & VM_EXECUTE) != 0
                        && execution_entry >= mapping_addresses[index]
                        && execution_entry - mapping_addresses[index]
                            < mapping_sizes[index]) {
                        return index;
                    }
                }
                return DEPLOY_NO_INDEX;
            }();
            const uint32_t stack_mapping_index =
                mapping_local(execution->stack);
            if (stack_mapping_index == DEPLOY_NO_INDEX) {
                return STATUS_BAD_ARGS;
            }
            const auto stack_ref = projections.mappings[stack_mapping_index];
            if (code_mapping == DEPLOY_NO_INDEX || !stack_ref) {
                return STATUS_INVALID_CAP;
            }
            const auto code_ref = projections.mappings[code_mapping];
            const auto stack_row =
                task.mapping(stack_mapping_index);
            if (!code_ref || !stack_row
                || mapping_sizes[code_mapping] == 0) {
                return STATUS_INVALID_CAP;
            }
            EpDesc descriptor{};
            descriptor.version = ENDPOINT_VERSION;
            descriptor.flags = ENDPOINT_FLAGS_NONE;
            descriptor.entry = execution_entry;
            descriptor.code_memory = code_ref.selector;
            descriptor.code_page = mapping_first[code_mapping];
            descriptor.code_address = mapping_addresses[code_mapping];
            descriptor.code_pages = mapping_sizes[code_mapping]
                / DEPLOY_PAGE_SIZE;
            descriptor.stack_memory = stack_ref.selector;
            descriptor.stack_page = mapping_first[stack_mapping_index];
            descriptor.stack_address = mapping_addresses[stack_mapping_index];
            descriptor.stack_pages = mapping_sizes[stack_mapping_index]
                / DEPLOY_PAGE_SIZE;
            descriptor.stack_stride = mapping_sizes[stack_mapping_index];
            descriptor.activation_count = 1;
            descriptor.queue_capacity = 1;
            descriptor.max_depth = 1;
            descriptor.budget_floor_ns = 1;
            descriptor.urgency_ceiling = execution->urgency;
            if (execution->ipc != DEPLOY_NO_INDEX) {
                const uint32_t ipc_mapping_index =
                    mapping_local(execution->ipc);
                if (ipc_mapping_index == DEPLOY_NO_INDEX) {
                    return STATUS_BAD_ARGS;
                }
                const auto ipc_row =
                    task.mapping(ipc_mapping_index);
                const auto ipc_ref = projections.mappings[ipc_mapping_index];
                if (!ipc_row || !ipc_ref) {
                    return STATUS_INVALID_CAP;
                }
                descriptor.ipc.memory = ipc_ref.selector;
                descriptor.ipc.page = mapping_first[ipc_mapping_index];
                descriptor.ipc.address = mapping_addresses[ipc_mapping_index];
                descriptor.ipc.pages = mapping_sizes[ipc_mapping_index]
                    / DEPLOY_PAGE_SIZE;
                descriptor.ipc_stride = mapping_sizes[ipc_mapping_index];
            }
            status_t status = materializer.write(
                projections.mappings[descriptor_mapping],
                static_cast<word_t>(descriptor_row->size),
                static_cast<word_t>(object->args[0]),
                &descriptor,
                sizeof(descriptor));
            if (status != STATUS_OK) {
                return status;
            }
            sys::cap::CapRef endpoint{};
            status = adopt_result(
                backend_type::endpoint_create(
                    pool, vspace, cspace,
                    descriptor_memory,
                    static_cast<word_t>(object->args[0])),
                OBJECT_KIND_ENDPOINT,
                endpoint);
            if (status != STATUS_OK) {
                return status;
            }
            projections.objects[object_index] = endpoint;
        }

        /* Imports are bounded transactions; earlier successful batches remain
         * in the same unpublished task and are reclaimed by failure.  A
         * typed row consumes its canonical descriptor bytes from the one
         * task-owned carrier at its batch-local offset.  The phase is
         * declared here but invoked after every constructible TaskKey source
         * (including executions and scheduling contexts) exists. */
        const auto import_sources = [&]() noexcept -> status_t {
            uint32_t imported = 0;
            while (imported < row->import_count) {
            const uint32_t count = row->import_count - imported
                > kImportBatchMax
                ? static_cast<uint32_t>(kImportBatchMax)
                : row->import_count - imported;
            sys::cap::CapRef* const outputs = workspace.imports;
            for (size_t index = 0; index < kImportBatchMax; ++index) {
                outputs[index] = {};
                workspace.import_bindings[index] = {};
                for (uint8_t& byte : workspace.import_descriptor_bytes[index]) {
                    byte = 0;
                }
            }
                for (uint32_t index = 0; index < count; ++index) {
                    const auto import = task.import(imported + index);
                    if (!import) {
                        return STATUS_BAD_ARGS;
                    }
                    sys::cap::encode(
                        import->attenuation,
                        workspace.import_descriptor_bytes[index]);
                    ImportBinding& binding = workspace.import_bindings[index];
                    binding.borrowed = input.bindings->imports[imported + index];
                    if (import->source_class == DEPLOY_IMPORT_SOURCE_TASK_KEY) {
                        const ByteView source_key = task.string(import->source);
                        std::optional<sys::cap::CapRef> source{};
                        const auto matches = [source_key](ByteView candidate)
                            noexcept -> bool {
                            return source_key.size() != 0
                                && candidate.equals(source_key);
                        };
                        if (matches(task.string(row->pool_key))) {
                            source = record.pool();
                        }
                        const auto consider = [&source, matches](
                            ByteView key,
                            const sys::cap::CapRef& projection,
                            obj_kind_t) noexcept {
                            if (source.has_value() || !matches(key)) {
                                return;
                            }
                            source = projection;
                        };
                        consider(task.string(row->vspace_key),
                                 projections.vspace,
                                 OBJECT_KIND_VSPACE);
                        consider(task.string(row->cspace_key),
                                 projections.cspace,
                                 OBJECT_KIND_CSPACE);
                        for (uint32_t mapping = 0;
                             mapping < row->mapping_count && !source; ++mapping) {
                            const auto mapping_row = task.mapping(mapping);
                            if (!mapping_row) {
                                return STATUS_BAD_ARGS;
                            }
                            consider(task.string(mapping_row->produced),
                                     projections.mappings[mapping],
                                     OBJECT_KIND_MEMORY);
                            consider(task.string(mapping_row->region),
                                     mapping_regions[mapping],
                                     OBJECT_KIND_VSPACE);
                        }
                        for (uint32_t object = 0;
                             object < row->object_count && !source; ++object) {
                            const auto object_row = task.object(object);
                            if (!object_row) {
                                return STATUS_BAD_ARGS;
                            }
                            consider(task.string(object_row->output),
                                     projections.objects[object],
                                     object_row->kind);
                            consider(task.string(object_row->output_b),
                                     projections.object_b[object],
                                     object_row->kind);
                        }
                        for (uint32_t execution = 0;
                             execution < row->execution_count && !source;
                             ++execution) {
                            const auto execution_row =
                                task.execution(execution);
                            if (!execution_row) {
                                return STATUS_BAD_ARGS;
                            }
                            consider(task.string(execution_row->key),
                                     projections.executions[execution],
                                     OBJECT_KIND_THREAD);
                            consider(task.string(execution_row->sc),
                                     projections.scheduling_contexts[execution],
                                     OBJECT_KIND_SCHED_CONTEXT);
                        }
                        if (!source || source->cspace != 0) {
                            return STATUS_BAD_ARGS;
                        }
                        binding.borrowed.cap = source.value();
                    }
                    if (import->mode == DEPLOY_IMPORT_TYPED_DELEGATE) {
                        binding.descriptor = workspace.import_descriptor;
                        binding.descriptor_offset =
                            index * CAP_ATTENUATION_SIZE;
                    }
                }
                if (typed_imports) {
                    const status_t written = materializer.write(
                        workspace.import_descriptor,
                        DEPLOY_PAGE_SIZE,
                        0,
                        &workspace.import_descriptor_bytes[0][0],
                        sizeof(workspace.import_descriptor_bytes));
                    if (written != STATUS_OK) {
                        return written;
                    }
                }
                const status_t status = ImportTransaction<
                    record_type, kImportBatchMax>::run(
                    record, task, imported, count,
                        workspace.import_bindings, outputs);
                if (status != STATUS_OK) {
                    return status;
                }
                for (uint32_t index = 0; index < count; ++index) {
                    projections.imports[imported + index] = outputs[index];
                }
                imported += count;
            }

            if (bool(workspace.import_descriptor)) {
                const status_t closed = record.drop(
                    workspace.import_descriptor);
                if (closed != STATUS_OK) {
                    return closed;
                }
                workspace.import_descriptor = {};
            }
            return STATUS_OK;
        };

        /* Bootstrap rows are materialized only after every Import has been
         * adopted.  The envelope therefore contains selectors from the
         * admitted child projections, never source authorities or guessed
         * fixed slots.  The current production path deliberately supports one execution here;
         * extending this to multi-execution requires an explicit ABI for
         * per-execution bootstrap state.  Defer the phase until after target
         * construction so TaskKey sources are complete. */
        const size_t arg_offset = sizeof(BootHdr) + size_t{row->bootstrap_count} * sizeof(BootCap);
        const size_t arg_size = row->bootstrap_count && input.arguments
            ? input.arguments->data().size : 0;
        const auto generate_bootstrap = [&]() noexcept -> status_t {
        uint32_t readiness_roles = 0;
        sys::cap::CapRef readiness_source{};
        sys::cap::CapRef service_source{};
        if (row->bootstrap_count != 0) {
            if (bootstrap_mapping == DEPLOY_NO_INDEX
                || row->bootstrap_count > DEPLOY_TASK_BOOTSTRAP_MAX
                || row->execution_count != 1
                || input.runtime_cpu_count == 0
                || !bool(workspace.bootstrap_memory)) {
                return STATUS_BAD_ARGS;
            }
            const auto execution = task.execution(0);
            const uint32_t stack_mapping = !execution
                ? DEPLOY_NO_INDEX : mapping_local(execution->stack);
            const uint32_t execution_bootstrap = !execution
                ? DEPLOY_NO_INDEX : mapping_local(execution->bootstrap);
            if (!execution
                || stack_mapping == DEPLOY_NO_INDEX
                || execution_bootstrap != bootstrap_mapping
                || mapping_sizes[bootstrap_mapping] < sizeof(BootHdr)
                || !bool(mapping_regions[bootstrap_mapping])
                || mapping_addresses[stack_mapping] == 0
                || mapping_sizes[stack_mapping] == 0
                || execution->stack_top == 0
                || input.bundle->size() == 0) {
                return STATUS_BAD_ARGS;
            }

            BootHdr info{};
            info.magic = BOOT_MAGIC;
            info.major = BOOT_MAJOR;
            info.minor = BOOT_MINOR;
            info.count = row->bootstrap_count;
            info.size = arg_offset;
            if (info.size > mapping_sizes[bootstrap_mapping]
                || arg_size > mapping_sizes[bootstrap_mapping] - info.size)
                return STATUS_BAD_ARGS;

            info.cpu_count = input.runtime_cpu_count;
            info.stack_base = mapping_addresses[stack_mapping];
            info.stack_size = mapping_sizes[stack_mapping];
            info.boot_bundle_size = input.bundle->size();

            for (uint32_t bootstrap = 0;
                 bootstrap < row->bootstrap_count; ++bootstrap) {
                const auto bootstrap_row =
                    task.bootstrap(bootstrap);
                if (!bootstrap_row) {
                    return STATUS_BAD_ARGS;
                }
                const obj_kind_t expected_kind =
                    bootstrap_row->kind == 0 ? bootstrap_row->object_kind
                        : boot_kind(bootstrap_row->kind);
                if (expected_kind == OBJECT_KIND_INVALID) {
                    return STATUS_BAD_ARGS;
                }
                const ByteView destination =
                    task.string(bootstrap_row->destination);
                size_t import_index = 0;
                size_t matches = 0;
                for (uint32_t import = 0; import < row->import_count;
                     ++import) {
                    const auto import_row = task.import(import);
                    if (bool(import_row)
                        && task.string(import_row->destination)
                               .equals(destination)) {
                        import_index = import;
                        ++matches;
                    }
                }
                if (destination.size() == 0 || matches != 1) {
                    return STATUS_BAD_ARGS;
                }
                const auto import = task.import(import_index);
                const sys::cap::CapRef& projection =
                    projections.imports[import_index];
                if (!import || import->attenuation.kind != expected_kind) {
                    return STATUS_BAD_ARGS;
                }
                if (bootstrap_row->kind
                        == BOOT_READY) {
                    if (row->readiness != DEPLOY_READINESS_EXPLICIT
                        || ++readiness_roles != 1
                        || import->source_class
                            != DEPLOY_IMPORT_SOURCE_TASK_KEY
                        || import->mode != DEPLOY_IMPORT_DUPLICATE
                        || import->attenuation.rights
                            != RIGHT_SIGNAL) {
                        return STATUS_BAD_ARGS;
                    }
                    const ByteView source_key = task.string(import->source);
                    if (source_key.size() == 0) {
                        return STATUS_BAD_ARGS;
                    }
                    size_t source_matches = 0;
                    for (uint32_t object_index = 0;
                         object_index < row->object_count; ++object_index) {
                        const auto object =
                            task.object(object_index);
                        if (!object
                            || object->kind != OBJECT_KIND_NOTIFICATION) {
                            continue;
                        }
                        const auto consider = [&](ByteView key,
                                                  const sys::cap::CapRef& slot)
                            noexcept {
                            if (key.size() == 0 || !key.equals(source_key)) {
                                return;
                            }
                            ++source_matches;
                            readiness_source = slot;
                        };
                        consider(task.string(object->output),
                                 projections.objects[object_index]);
                        consider(task.string(object->output_b),
                                 projections.object_b[object_index]);
                    }
                    if (source_matches != 1 || !bool(readiness_source)) {
                        return STATUS_INVALID_CAP;
                    }
                    const sys::cap::CapRef relation = relation_notification;
                    if (bool(relation)
                        && readiness_source == relation) {
                        return STATUS_BAD_ARGS;
                    }
                    record.readiness_ = readiness_source;
                } else if (bootstrap_row->kind
                               == BOOT_EVENTS) {
                    if (import->source_class
                            != DEPLOY_IMPORT_SOURCE_TASK_KEY
                        || import->mode != DEPLOY_IMPORT_DUPLICATE) {
                        return STATUS_BAD_ARGS;
                    }
                    const ByteView source_key = task.string(import->source);
                    if (source_key.size() == 0) {
                        return STATUS_BAD_ARGS;
                    }
                    size_t source_matches = 0;
                    for (uint32_t object_index = 0;
                         object_index < row->object_count; ++object_index) {
                        const auto object =
                            task.object(object_index);
                        if (!object
                            || object->kind != OBJECT_KIND_NOTIFICATION) {
                            continue;
                        }
                        const auto consider = [&](ByteView key,
                                                  const sys::cap::CapRef& slot)
                            noexcept {
                            if (key.size() == 0 || !key.equals(source_key)) {
                                return;
                            }
                            ++source_matches;
                            service_source = slot;
                        };
                        consider(task.string(object->output),
                                 projections.objects[object_index]);
                        consider(task.string(object->output_b),
                                 projections.object_b[object_index]);
                    }
                    if (source_matches != 1 || !bool(service_source)) {
                        return STATUS_INVALID_CAP;
                    }
                }
                const auto reference = projection;
                if (!bool(projection) || !reference
                    || reference.cspace == 0) {
                    return STATUS_INVALID_CAP;
                }
                BootCap binding{};
                binding.role = bootstrap_row->kind;
                binding.kind = expected_kind;
                binding.handle = reference.selector;
                if (!binding.role) {
                    const ByteView name = task.string(bootstrap_row->name);
                    if (name.size() == 0 || name.size() >= BOOT_NAME_MAX
                        || !bootstrap_row->protocol || !bootstrap_row->major)
                        return STATUS_BAD_ARGS;
                    for (size_t i = 0; i < name.size(); ++i) binding.name[i] = static_cast<char>(name[i]);
                    binding.protocol = bootstrap_row->protocol;
                    binding.major = bootstrap_row->major;
                    binding.minor = bootstrap_row->minor;
                }
                const auto status = materializer.write(workspace.bootstrap_memory,
                    mapping_sizes[bootstrap_mapping], sizeof(info) + bootstrap * sizeof(BootCap),
                    &binding, sizeof(binding));
                if (status != STATUS_OK) return status;
            }

            status_t status = materializer.write(
                workspace.bootstrap_memory,
                mapping_sizes[bootstrap_mapping],
                0,
                &info,
                sizeof(info));
            if (status == STATUS_OK && arg_size) {
                status = materializer.write(workspace.bootstrap_memory,
                    mapping_sizes[bootstrap_mapping], arg_offset, input.arguments->data().bytes, arg_size);
            }
            if (status == STATUS_OK) {
                const auto memory = workspace.bootstrap_memory;
                status = memory
                    ? backend_type::memory_seal(memory)
                    : STATUS_INVALID_CAP;
            }
            if (status == STATUS_OK) {
                status = record.drop(workspace.bootstrap_memory);
            }
            if (status != STATUS_OK) {
                return status;
            }
            workspace.bootstrap_memory = {};
            projections.mappings[bootstrap_mapping] = mapping_regions[bootstrap_mapping];
            projections.bootstrap = projections.mappings[bootstrap_mapping];
        }
        if ((row->readiness == DEPLOY_READINESS_EXPLICIT)
                != (readiness_roles == 1)
            || (row->readiness != DEPLOY_READINESS_EXPLICIT
                && bool(record.readiness_))) {
            return STATUS_BAD_ARGS;
        }
        if (service_source && (service_source == relation_notification
            || service_source == readiness_source)) return STATUS_BAD_ARGS;
        return STATUS_OK;
        };

        /* Executions and their SCs are created before imports.  A descriptor
         * MemoryObject is retained by task until this unpublished task
         * either commits in Cut D or follows the strong-close path. */
        for (uint32_t index = 0; index < row->execution_count; ++index) {
            const auto execution = task.execution(index);
            if (!execution || !input.bindings->domains[index].valid()) {
                return STATUS_BAD_ARGS;
            }
            const uint32_t stack_mapping_index = mapping_local(execution->stack);
            const uint32_t bootstrap_mapping_index =
                mapping_local(execution->bootstrap);
            if (execution->image < row->image_first
                || execution->image >= row->image_first + row->image_count
                || stack_mapping_index == DEPLOY_NO_INDEX
                || bootstrap_mapping_index == DEPLOY_NO_INDEX) {
                return STATUS_BAD_ARGS;
            }
            const size_t image_index =
                execution->image - row->image_first;
            const auto stack_mapping =
                task.mapping(stack_mapping_index);
            const auto bootstrap_mapping =
                task.mapping(bootstrap_mapping_index);
            const auto stack = projections.mappings[stack_mapping_index];
            if (!stack_mapping || !bootstrap_mapping
                || !stack) {
                return STATUS_INVALID_CAP;
            }
            const auto entry = execution->entry != 0
                ? execution->entry : image_entries[image_index];
            if (entry == 0 || execution->stack_top == 0) {
                return STATUS_BAD_ARGS;
            }
            sys::cap::CapRef descriptor_slot{};
            status_t status = STATUS_BAD_ARGS;
            if (execution->model == DEPLOY_EXECUTION_THREAD) {
                ThreadInit descriptor{};
                descriptor.version = THREAD_START_VERSION;
                descriptor.flags = 0;
                descriptor.entry = entry;
                descriptor.stack = execution->stack_top;
                descriptor.arguments[0] = mapping_addresses[
                    bootstrap_mapping_index];
                descriptor.arguments[1] = mapping_sizes[
                    bootstrap_mapping_index];
                descriptor.arguments[2] = arg_size ? mapping_addresses[bootstrap_mapping_index] + arg_offset : 0;
                descriptor.arguments[3] = arg_size;
                if (execution->ipc != DEPLOY_NO_INDEX) {
                    const uint32_t ipc_mapping_index =
                        mapping_local(execution->ipc);
                    if (ipc_mapping_index == DEPLOY_NO_INDEX) {
                        return STATUS_BAD_ARGS;
                    }
                    const auto ipc_mapping =
                        task.mapping(ipc_mapping_index);
                    const auto ipc = !ipc_mapping
                        ? std::optional<sys::cap::CapRef>{}
                        : projections.mappings[ipc_mapping_index];
                    if (!ipc_mapping || !ipc) {
                        return STATUS_INVALID_CAP;
                    }
                    descriptor.ipc.memory = ipc->selector;
                    descriptor.ipc.page = mapping_first[ipc_mapping_index];
                    descriptor.ipc.address = mapping_addresses[
                        ipc_mapping_index];
                    descriptor.ipc.pages = mapping_sizes[ipc_mapping_index]
                        / DEPLOY_PAGE_SIZE;
                }
                status = materializer.materialize_descriptor(
                    &descriptor, sizeof(descriptor), descriptor_slot);
                if (status == STATUS_OK) {
                    const auto descriptor_ref = descriptor_slot;
                    status = descriptor_ref
                        ? adopt_result(
                              backend_type::thread_create(
                                  pool, vspace, cspace,
                                  descriptor_ref, 0),
                              OBJECT_KIND_THREAD,
                              projections.executions[index])
                        : STATUS_INVALID_CAP;
                }
            } else {
                status = STATUS_BAD_ARGS;
            }
            if (bool(descriptor_slot)) {
                const status_t closed =
                    record.drop(descriptor_slot);
                if (status == STATUS_OK && closed != STATUS_OK) {
                    status = closed;
                }
            }
            if (status != STATUS_OK
                || !bool(projections.executions[index])) {
                return status == STATUS_OK
                    ? STATUS_INVALID_CAP : status;
            }
            const auto execution_ref = projections.executions[index];
            const auto create_context = [&](uint32_t cpu) noexcept {
                return backend_type::sc_create(
                    pool, input.bindings->domains[index].cap,
                    static_cast<word_t>(execution->sc_budget),
                    static_cast<word_t>(execution->sc_period),
                    static_cast<word_t>(execution->urgency), cpu);
            };
            sys::SysResult context{};
            if (execution->home_cpu == DEPLOY_HOME_CPU_ANY) {
                // Distribute independent tasks, then probe each allowed CPU
                // at most once. The domain remains the admission authority.
                for (uint32_t attempt = 0; attempt < input.runtime_cpu_count; ++attempt) {
                    const auto cpu = (record.id().slot + index + attempt) % input.runtime_cpu_count;
                    context = create_context(cpu);
                    if (context.status != STATUS_BUSY) break;
                }
            } else context = create_context(execution->home_cpu);
            const auto sc = adopt_result(context,
                OBJECT_KIND_SCHED_CONTEXT,
                projections.scheduling_contexts[index]);
            if (status != STATUS_OK || !execution_ref
                || sc != STATUS_OK) {
                return status != STATUS_OK ? status : sc;
            }
            const auto sc_ref = projections.scheduling_contexts[index];
            if (!sc_ref) {
                return STATUS_INVALID_CAP;
            }
            const status_t sc_bind_status = backend_type::sc_bind(
                sc_ref, execution_ref);
            if (sc_bind_status != STATUS_OK) {
                return sc_bind_status;
            }
            if (bool(relation_notification)) {
                const auto notification = relation_notification;
                if (!notification) {
                    return STATUS_INVALID_CAP;
                }
                const status_t exit_status =
                    backend_type::exit_bind(
                        execution_ref, notification,
                        relation_badge);
                if (exit_status != STATUS_OK) {
                    return exit_status;
                }
                if (index < DEPLOY_TASK_DEPENDENCY_MAX) {
                    projections.relations[index] = relation_notification;
                }
            }
        }

        /* Every local source named by a TaskKey now exists in the caller's
         * current CSpace.  Imports adopt their destinations immediately, then
         * the generated bootstrap envelope records only those admitted child
         * selectors. */
        const status_t import_status = import_sources();
        if (import_status != STATUS_OK) {
            return import_status;
        }
        const status_t bootstrap_status = generate_bootstrap();
        if (bootstrap_status != STATUS_OK) {
            return bootstrap_status;
        }

        /* Endpoint descriptor mappings are snapshot sources.  Retire their
         * writable MemoryObject selectors only after all execution consumers
         * have taken their snapshots; a shared mapping therefore remains
         * usable through the final constructor use.  The mapped VSpace region
         * is the retained projection and object lifetime. */
        for (uint32_t object_index = 0; object_index < row->object_count;
             ++object_index) {
            const auto object = task.object(object_index);
            if (!object || object->kind != OBJECT_KIND_ENDPOINT) {
                continue;
            }
            const uint32_t descriptor_mapping = mapping_local(object->refs[0]);
            if (descriptor_mapping == DEPLOY_NO_INDEX) {
                return STATUS_BAD_ARGS;
            }
            sys::cap::CapRef& mapping_projection =
                projections.mappings[descriptor_mapping];
            if (mapping_projection == mapping_regions[descriptor_mapping]) {
                continue;
            }
            const sys::cap::CapRef region = mapping_regions[descriptor_mapping];
            if (!region) {
                return STATUS_INVALID_CAP;
            }
            const status_t status = record.drop(
                mapping_projection);
            if (status != STATUS_OK) {
                return status;
            }
            mapping_projection = region;
        }

        if (row->bootstrap_mapping != DEPLOY_NO_INDEX
            && !bool(projections.bootstrap)) {
            return STATUS_INVALID_CAP;
        }
        if (accounting.total_bytes > row->critical_bytes || !bind_exports(record, task, projections)) {
            return STATUS_INVALID_CAP;
        }
        std::copy_n(projections.executions, row->execution_count, record.threads_.begin());
        record.exit_event_ = projections.relations[0];
        return STATUS_OK;
    }
    [[nodiscard]] static auto bind_exports(
        record_type& record,
        const TaskSpec& task, const BuildRefs& projections) noexcept -> bool {
        const auto row = task.row();
        if (!row || row->export_count > DEPLOY_TASK_EXPORT_MAX) {
            return false;
        }
        record.exports_.fill({});

        const auto consider = [](ByteView source, ByteView key,
                                const sys::cap::CapRef& slot,
                                obj_kind_t declared_kind,
                                bool& found,
                                obj_kind_t& declared,
                                std::optional<sys::cap::CapRef>& result) noexcept -> bool {
            if (!key.equals(source)) {
                return true;
            }
            if (found) {
                return false;
            }
            found = true;
            declared = declared_kind;
            result = slot.cspace == 0 ? std::optional{slot} : std::nullopt;
            return true;
        };

        for (uint32_t index = 0; index < row->export_count; ++index) {
            const auto export_row = task.export_record(index);
            if (!export_row
                || !attenuation::valid_descriptor(export_row->ceiling, attenuation::DescriptorForm::Ceiling)) {
                return false;
            }
            if (export_row->source_class == DEPLOY_EXPORT_RUNTIME_READY) {
                /* RuntimeReady is intentionally withheld from construction;
                 * the current production path binds it during the real publication transition. */
                continue;
            }
            if (export_row->source_class != DEPLOY_EXPORT_PREPARED_KEY) {
                return false;
            }
            const ByteView source = task.string(export_row->source);
            if (source.size() == 0) {
                return false;
            }
            std::optional<sys::cap::CapRef> result{};
            bool found = false;
            obj_kind_t declared_kind = OBJECT_KIND_INVALID;
            const ByteView pool_key = task.string(row->pool_key);
            if (pool_key.equals(source)) {
                found = true;
                declared_kind = OBJECT_KIND_RESOURCE_POOL;
                result = record.pool();
            }
            const auto check_slot = [&](ByteView key,
                                        const sys::cap::CapRef& slot,
                                        obj_kind_t kind) noexcept {
                return consider(source, key, slot, kind, found,
                                declared_kind, result);
            };
            if (!check_slot(task.string(row->vspace_key),
                            projections.vspace, OBJECT_KIND_VSPACE)
                || !check_slot(task.string(row->cspace_key),
                               projections.cspace, OBJECT_KIND_CSPACE)) {
                return false;
            }
            for (uint32_t mapping = 0; mapping < row->mapping_count;
                 ++mapping) {
                const auto mapping_row = task.mapping(mapping);
                if (!mapping_row
                    || !check_slot(task.string(mapping_row->produced),
                                   projections.mappings[mapping],
                                   OBJECT_KIND_MEMORY)) {
                    return false;
                }
            }
            for (uint32_t object = 0; object < row->object_count; ++object) {
                const auto object_row = task.object(object);
                if (!object_row
                    || !check_slot(task.string(object_row->output),
                                   projections.objects[object],
                                   object_row->kind)
                    || !check_slot(task.string(object_row->output_b),
                                   projections.object_b[object],
                                   object_row->kind)) {
                    return false;
                }
            }
            for (uint32_t execution = 0;
                 execution < row->execution_count; ++execution) {
                const auto execution_row = task.execution(execution);
                if (!execution_row
                    || !check_slot(task.string(execution_row->key),
                                   projections.executions[execution],
                                   OBJECT_KIND_THREAD)
                    || !check_slot(task.string(execution_row->sc),
                                   projections.scheduling_contexts[execution],
                                   OBJECT_KIND_SCHED_CONTEXT)) {
                    return false;
                }
            }
            if (!found || !result
                || declared_kind != export_row->ceiling.kind) {
                return false;
            }
            record.exports_[index] = CapSrc{*result, export_row->ceiling};
        }
        return true;
    }

    [[nodiscard]] static auto imports_admissible(
        const TaskSpec& task) noexcept -> bool {
        const auto row = task.row();
        if (!row) {
            return false;
        }
        for (uint32_t index = 0; index < row->import_count; ++index) {
            const auto import = task.import(index);
            if (!import
                || import->mode >= DEPLOY_IMPORT_MOVE) {
                return false;
            }
        }
        return true;
    }

};

} // namespace deploy
