#pragma once

/*
 * Cut A task transaction primitives.  These types are userspace state only:
 * the kernel knows neither TaskId nor any of the table/CompletionSet tags.
 * TaskSpace remains the sole owner of kernel selectors; this file only moves
 * that owner between Builder, TaskTable and ClosingRecord.
 */

#include <stddef.h>
#include <stdint.h>

#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <libk/variant.hpp>
#include <uapi/deploy.h>
#include <uapi/bootstrap.h>
#include <uapi/endpoint.h>
#include <uapi/thread.h>
#include <uapi/status.h>
#include <uapi/vproc.h>

#include <user/lib/deployment.hpp>
#include <user/lib/deployment_plan.hpp>
#include <user/lib/image_materializer.hpp>
#include <user/lib/task_authority.hpp>

namespace myos::deploy {

template<typename Table, typename CompletionSetT>
class TaskBuilder;

struct TaskId final {
    uint32_t slot{};
    uint32_t generation{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return generation != 0;
    }

    constexpr auto operator==(const TaskId&) const noexcept -> bool = default;
};

enum class TaskState : uint8_t {
    Constructing,
    Prepared,
    Starting,
    Running,
    Terminating,
    Failed,
    Closing,
    Reclaimed,
};

/* The sequence is the kernel terminal generation; status is the public
 * terminal status sampled from that generation.  A zero sequence means that
 * the target is still live (or that no observation has been admitted yet). */
struct TerminalObservation final {
    uint64_t sequence{};
    myos_status_t status{};

    [[nodiscard]] constexpr auto terminal() const noexcept -> bool {
        return sequence != 0;
    }
};

enum class TaskSlotTag : uint8_t {
    Vacant,
    Reserved,
    Record,
    Closing,
    Retired,
};

enum class CloseReason : uint16_t {
    ConstructionFailure,
    Terminal,
    Explicit,
    SourceRevoked,
    Internal,
};

enum class ProjectionKind : uint8_t {
    Empty,
    Local,
    Remote,
};

/* A checked view; it never owns or closes the represented selector. */
struct SlotProjection final {
    ProjectionKind projection{ProjectionKind::Empty};
    myos::deploy::LocalSlot local{};
    size_t remote_index{};
    myos_cap_t manager{};
    myos_object_kind_t kind{MYOS_OBJECT_KIND_INVALID};
    AuthorityId authority{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        if (projection == ProjectionKind::Local) {
            return local.valid();
        }
        return projection == ProjectionKind::Remote
            && remote_index != static_cast<size_t>(-1)
            && manager != 0
            && kind > MYOS_OBJECT_KIND_INVALID
            && kind < MYOS_OBJECT_KIND_COUNT;
    }
};

enum class SourceProjectionKind : uint8_t {
    Empty,
    Pool,
    Local,
};

/* A non-owning binding used only between Prepared construction and the later
 * publication transition.  The TaskSpace remains the sole selector owner;
 * this view carries either its pool marker or a checked local slot, never a
 * selector, authority identity, generation or registration token. */
struct SourceProjection final {
    SourceProjectionKind projection{SourceProjectionKind::Empty};
    LocalSlot local{};
    myos_object_kind_t kind{MYOS_OBJECT_KIND_INVALID};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        if (projection == SourceProjectionKind::Pool) {
            return local.pool == 0 && local.kind == MYOS_OBJECT_KIND_INVALID
                && kind == MYOS_OBJECT_KIND_RESOURCE_POOL;
        }
        return projection == SourceProjectionKind::Local
            && local.valid() && local.kind == kind;
    }
};

/* Construction receives already-resolved authority identities.  The arrays
 * are caller-owned bindings indexed by the decoded TaskPlan rows; they are
 * not a second policy table, descriptor byte store or selector owner. */
struct TaskAuthorityBindings final {
    AuthorityId domains[MYOS_DEPLOY_TASK_EXECUTION_MAX]{};
    AuthorityId pagers[MYOS_DEPLOY_TASK_MAPPING_MAX]{};
    AuthorityId imports[MYOS_DEPLOY_TASK_IMPORT_MAX]{};
};

/*
 * Bounded, caller-owned scratch for one finite construction.  This is not a
 * second policy or ownership record: every selector is adopted by TaskSpace
 * immediately, while these arrays only hold leases and materialization
 * metadata until the call returns.  Keeping it outside TaskBuilder's target
 * frame makes the bounded workspace/lifetime contract explicit to the real
 * construction caller.
 */
template<typename Authorities>
struct TaskConstructionWorkspace final {
    using lease_type = typename Authorities::Lease;
    using image_type = MaterializedImage<32, 1>;

    TaskConstructionWorkspace() noexcept = default;
    TaskConstructionWorkspace(const TaskConstructionWorkspace&) = delete;
    auto operator=(const TaskConstructionWorkspace&)
        -> TaskConstructionWorkspace& = delete;
    TaskConstructionWorkspace(TaskConstructionWorkspace&&) = delete;
    auto operator=(TaskConstructionWorkspace&&)
        -> TaskConstructionWorkspace& = delete;

    ~TaskConstructionWorkspace() noexcept {
        libk_assert(empty());
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        for (const auto& lease : domain_leases) {
            if (lease.has_value()) {
                return false;
            }
        }
        for (const auto& lease : pager_leases) {
            if (lease.has_value()) {
                return false;
            }
        }
        if (!image.segments.empty() || !image.stacks.empty()
            || image.entry != 0) {
            return false;
        }
        for (size_t index = 0;
             index < MYOS_DEPLOY_TASK_IMAGE_MAX; ++index) {
            if (image_entries[index] != 0) {
                return false;
            }
        }
        for (size_t index = 0;
             index < MYOS_DEPLOY_TASK_MAPPING_MAX; ++index) {
            if (mapping_regions[index].valid()
                || mapping_addresses[index] != 0
                || mapping_sizes[index] != 0
                || mapping_access[index] != 0
                || mapping_done[index]) {
                return false;
            }
        }
        for (size_t index = 0; index < kImportBatchMax; ++index) {
            const ImportProjection& output = imports[index];
            if (output.authority.valid() || output.task_key
                || output.remote_index != static_cast<size_t>(-1)
                || output.manager != 0
                || output.kind != MYOS_OBJECT_KIND_INVALID) {
                return false;
            }
        }
        if (bootstrap_memory.valid()) {
            return false;
        }
        for (size_t index = 0; index < kImportBatchMax; ++index) {
            if (import_bindings[index].authority.valid()
                || import_bindings[index].descriptor.valid()
                || import_bindings[index].descriptor_offset != 0
                || import_bindings[index].source) {
                return false;
            }
        }
        if (import_descriptor.valid()) {
            return false;
        }
        for (const auto& bytes : import_descriptor_bytes) {
            for (const uint8_t byte : bytes) {
                if (byte != 0) {
                    return false;
                }
            }
        }
        return true;
    }

    void clear() noexcept {
        for (auto& lease : domain_leases) {
            lease.reset();
        }
        for (auto& lease : pager_leases) {
            lease.reset();
        }
        image.clear();
        for (auto& entry : image_entries) {
            entry = 0;
        }
        for (auto& address : mapping_addresses) {
            address = 0;
        }
        for (auto& region : mapping_regions) {
            region = {};
        }
        for (auto& size : mapping_sizes) {
            size = 0;
        }
        for (auto& access : mapping_access) {
            access = 0;
        }
        for (auto& done : mapping_done) {
            done = false;
        }
        for (auto& output : imports) {
            output = {};
        }
        for (auto& binding : import_bindings) {
            binding = {};
        }
        import_descriptor = {};
        bootstrap_memory = {};
        for (auto& bytes : import_descriptor_bytes) {
            for (auto& byte : bytes) {
                byte = 0;
            }
        }
    }

    libk::optional<lease_type> domain_leases[
        MYOS_DEPLOY_TASK_EXECUTION_MAX]{};
    libk::optional<lease_type> pager_leases[
        MYOS_DEPLOY_TASK_MAPPING_MAX]{};
    image_type image{};
    uintptr_t image_entries[MYOS_DEPLOY_TASK_IMAGE_MAX]{};
    LocalSlot mapping_regions[MYOS_DEPLOY_TASK_MAPPING_MAX]{};
    myos_word_t mapping_addresses[MYOS_DEPLOY_TASK_MAPPING_MAX]{};
    myos_word_t mapping_sizes[MYOS_DEPLOY_TASK_MAPPING_MAX]{};
    myos_word_t mapping_access[MYOS_DEPLOY_TASK_MAPPING_MAX]{};
    bool mapping_done[MYOS_DEPLOY_TASK_MAPPING_MAX]{};
    ImportProjection imports[kImportBatchMax]{};
    ImportBinding import_bindings[kImportBatchMax]{};
    LocalSlot import_descriptor{};
    /* Writable carrier retained only until the generated bootstrap envelope
     * is sealed.  The mapped bootstrap region remains the published child
     * projection after this selector is closed. */
    LocalSlot bootstrap_memory{};
    uint8_t import_descriptor_bytes[kImportBatchMax][
        MYOS_CAP_ATTENUATION_SIZE]{};
};

template<typename B, typename Authorities>
struct TaskConstructionInput final {
    using workspace_type = TaskConstructionWorkspace<Authorities>;

    cap::CapRef parent_pool{};
    MappedBundle<B>* bundle{};
    ScratchWindow<B>* scratch{};
    const void* bootstrap{};
    size_t bootstrap_size{};
    /* Runtime context is explicit construction input, not manifest policy.
     * A generated bootstrap envelope requires a checked non-zero CPU count. */
    uint32_t runtime_cpu_count{};
    const TaskAuthorityBindings* bindings{};
    workspace_type& workspace;
};

template<typename B>
concept ConstructionBackend = Backend<B>
    && requires(
        cap::CapRef pool,
        cap::CapRef vspace,
        cap::CapRef cspace,
        cap::CapRef descriptor,
        cap::CapRef domain,
        cap::CapRef target,
        cap::CapRef notification,
        myos_word_t words) {
    { B::memory_create_pager(pool, words, words, descriptor) }
        -> libk::SameAs<SysResult>;
    { B::sc_create(pool, domain, words, words, words, words) }
        -> libk::SameAs<SysResult>;
    { B::sc_bind(domain, target) } -> libk::SameAs<myos_status_t>;
    { B::thread_create(pool, vspace, cspace, descriptor, words) }
        -> libk::SameAs<SysResult>;
    { B::vproc_create(pool, vspace, cspace, descriptor, words) }
        -> libk::SameAs<SysResult>;
    { B::notification_create(pool, words) } -> libk::SameAs<SysResult>;
    { B::channel_create(pool, words, words, words, words) }
        -> libk::SameAs<SysResult>;
    { B::pager_create(pool, words, words) } -> libk::SameAs<SysResult>;
    { B::endpoint_create(pool, vspace, cspace, descriptor, words) }
        -> libk::SameAs<SysResult>;
    { B::terminal_observe_bind(target, notification, words) }
        -> libk::SameAs<myos_status_t>;
};

struct TaskProjections final {
    SlotProjection vspace{};
    SlotProjection cspace{};
    SlotProjection bootstrap{};
    SlotProjection mappings[MYOS_DEPLOY_TASK_MAPPING_MAX]{};
    SlotProjection objects[MYOS_DEPLOY_TASK_OBJECT_MAX]{};
    /* ChannelMint/Channel construction has two destination selectors for one
     * manifest object.  Keep the second projection parallel to the decoded
     * object row instead of consuming an unrelated adjacent row. */
    SlotProjection object_b[MYOS_DEPLOY_TASK_OBJECT_MAX]{};
    SlotProjection executions[MYOS_DEPLOY_TASK_EXECUTION_MAX]{};
    SlotProjection scheduling_contexts[MYOS_DEPLOY_TASK_EXECUTION_MAX]{};
    SlotProjection imports[MYOS_DEPLOY_TASK_IMPORT_MAX]{};
    SlotProjection relations[MYOS_DEPLOY_TASK_DEPENDENCY_MAX]{};
    SourceProjection exports[MYOS_DEPLOY_TASK_EXPORT_MAX]{};
};

struct ResidentAccounting final {
    uint64_t total_bytes{};
    uint64_t by_class[MYOS_DEPLOY_CRITICAL_IPC_HEADER + 1U]{};
};

enum class CompletionCellState : uint8_t {
    Vacant,
    Reserved,
    Ready,
    Detached,
    Retired,
};

struct CompletionId final {
    uint32_t index{};
    uint32_t generation{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return generation != 0;
    }

    constexpr auto operator==(const CompletionId&) const noexcept -> bool = default;
};

struct CompletionResult final {
    TaskId task{};
    CloseReason reason{CloseReason::Internal};
    myos_status_t status{MYOS_STATUS_INTERNAL};
};

template<size_t Capacity = MYOS_DEPLOY_TASK_MAX,
         uint32_t GenerationLimit = UINT32_MAX>
class CompletionSet final {
    static_assert(Capacity != 0);
    static_assert(GenerationLimit != 0);

    struct Cell final {
        CompletionCellState state{CompletionCellState::Vacant};
        uint32_t generation{1};
        CompletionResult result{};
    };

public:
    class Receiver;
    class Sender;

    class Pair final {
    public:
        Pair() noexcept = default;
        Pair(const Pair&) = delete;
        auto operator=(const Pair&) -> Pair& = delete;

        Pair(Pair&& other) noexcept
            : sender_(libk::move(other.sender_)),
              receiver_(libk::move(other.receiver_)) {}

        auto operator=(Pair&& other) noexcept -> Pair& {
            if (this == &other) {
                return *this;
            }
            cancel();
            sender_ = libk::move(other.sender_);
            receiver_ = libk::move(other.receiver_);
            return *this;
        }

        ~Pair() noexcept { cancel(); }

        [[nodiscard]] auto take_sender() noexcept -> Sender {
            return libk::move(sender_);
        }
        [[nodiscard]] auto take_receiver() noexcept -> Receiver {
            return libk::move(receiver_);
        }

        void cancel() noexcept {
            static_cast<void>(receiver_.detach());
            static_cast<void>(sender_.cancel());
        }

    private:
        friend class CompletionSet;
        Pair(CompletionSet* set, CompletionId id) noexcept
            : sender_(set, id), receiver_(set, id) {}

        Sender sender_{};
        Receiver receiver_{};
    };

    class Sender final {
    public:
        Sender() noexcept = default;
        Sender(const Sender&) = delete;
        auto operator=(const Sender&) -> Sender& = delete;

        Sender(Sender&& other) noexcept
            : set_(other.set_), id_(other.id_), active_(other.active_),
              cancelable_(other.cancelable_) {
            other.set_ = nullptr;
            other.id_ = {};
            other.active_ = false;
            other.cancelable_ = false;
        }

        auto operator=(Sender&& other) noexcept -> Sender& {
            if (this == &other) {
                return *this;
            }
            abandon();
            set_ = other.set_;
            id_ = other.id_;
            active_ = other.active_;
            cancelable_ = other.cancelable_;
            other.set_ = nullptr;
            other.id_ = {};
            other.active_ = false;
            other.cancelable_ = false;
            return *this;
        }

        ~Sender() noexcept { abandon(); }

        [[nodiscard]] constexpr auto valid() const noexcept -> bool {
            return set_ != nullptr && active_ && id_.valid();
        }

        /* Reservation owners cancel only after the receiver detaches. */
        void seal() noexcept { cancelable_ = false; }

        [[nodiscard]] auto complete(CompletionResult result) noexcept -> bool {
            if (!valid()) {
                return false;
            }
            const bool delivered = set_->publish(id_, result);
            active_ = false;
            set_ = nullptr;
            id_ = {};
            cancelable_ = false;
            return delivered;
        }

        [[nodiscard]] auto cancel() noexcept -> bool {
            if (!valid() || !cancelable_) {
                return false;
            }
            const bool canceled = set_->cancel(id_);
            if (!canceled) {
                return false;
            }
            active_ = false;
            set_ = nullptr;
            id_ = {};
            cancelable_ = false;
            return canceled;
        }

        [[nodiscard]] constexpr auto id() const noexcept -> CompletionId {
            return id_;
        }

    private:
        friend class CompletionSet;
        Sender(CompletionSet* set, CompletionId id) noexcept
            : set_(set), id_(id), active_(true), cancelable_(true) {}

        void abandon() noexcept {
            if (!active_) {
                return;
            }
            if (!cancelable_) {
                libk_assert(false);
            }
            libk_assert(cancel());
        }

        CompletionSet* set_{};
        CompletionId id_{};
        bool active_{};
        bool cancelable_{};
    };

    class Receiver final {
    public:
        Receiver() noexcept = default;
        Receiver(const Receiver&) = delete;
        auto operator=(const Receiver&) -> Receiver& = delete;

        Receiver(Receiver&& other) noexcept
            : set_(other.set_), id_(other.id_), active_(other.active_) {
            other.set_ = nullptr;
            other.id_ = {};
            other.active_ = false;
        }

        auto operator=(Receiver&& other) noexcept -> Receiver& {
            if (this == &other) {
                return *this;
            }
            detach();
            set_ = other.set_;
            id_ = other.id_;
            active_ = other.active_;
            other.set_ = nullptr;
            other.id_ = {};
            other.active_ = false;
            return *this;
        }

        ~Receiver() noexcept { static_cast<void>(detach()); }

        [[nodiscard]] constexpr auto valid() const noexcept -> bool {
            return set_ != nullptr && active_ && id_.valid();
        }

        [[nodiscard]] auto take() noexcept -> libk::optional<CompletionResult> {
            if (!valid() || set_->cell_state(id_) != CompletionCellState::Ready) {
                return libk::nullopt;
            }
            CompletionResult result = set_->cell(id_).result;
            set_->recycle(id_);
            active_ = false;
            set_ = nullptr;
            id_ = {};
            return result;
        }

        [[nodiscard]] auto detach() noexcept -> bool {
            if (!valid()) {
                return false;
            }
            const bool detached = set_->detach(id_);
            active_ = false;
            set_ = nullptr;
            id_ = {};
            return detached;
        }

        [[nodiscard]] constexpr auto id() const noexcept -> CompletionId {
            return id_;
        }

    private:
        friend class CompletionSet;
        Receiver(CompletionSet* set, CompletionId id) noexcept
            : set_(set), id_(id), active_(true) {}

        CompletionSet* set_{};
        CompletionId id_{};
        bool active_{};
    };

    CompletionSet() noexcept = default;
    CompletionSet(const CompletionSet&) = delete;
    auto operator=(const CompletionSet&) -> CompletionSet& = delete;

    ~CompletionSet() noexcept {
        for (const Cell& cell : cells_) {
            libk_assert(cell.state == CompletionCellState::Vacant
                || cell.state == CompletionCellState::Retired);
        }
    }

    [[nodiscard]] auto reserve() noexcept -> libk::optional<Pair> {
        for (size_t index = 0; index < Capacity; ++index) {
            Cell& cell = cells_[index];
            if (cell.state != CompletionCellState::Vacant) {
                continue;
            }
            const CompletionId id{
                static_cast<uint32_t>(index), cell.generation};
            cell.state = CompletionCellState::Reserved;
            cell.result = {};
            return Pair{this, id};
        }
        return libk::nullopt;
    }

    [[nodiscard]] auto cell_state(CompletionId id) const noexcept
        -> CompletionCellState {
        const Cell* cell = checked_cell(id);
        return cell == nullptr ? CompletionCellState::Retired : cell->state;
    }

    [[nodiscard]] constexpr auto available() const noexcept -> size_t {
        size_t count = 0;
        for (const Cell& cell : cells_) {
            count += cell.state == CompletionCellState::Vacant ? 1 : 0;
        }
        return count;
    }

    [[nodiscard]] static constexpr auto capacity() noexcept -> size_t {
        return Capacity;
    }

    [[nodiscard]] constexpr auto retired() const noexcept -> size_t {
        size_t count = 0;
        for (const Cell& cell : cells_) {
            count += cell.state == CompletionCellState::Retired ? 1 : 0;
        }
        return count;
    }

private:
    [[nodiscard]] auto checked_cell(CompletionId id) noexcept -> Cell* {
        if (!id.valid() || id.index >= Capacity) {
            return nullptr;
        }
        Cell& cell = cells_[id.index];
        return cell.generation == id.generation ? &cell : nullptr;
    }

    [[nodiscard]] auto checked_cell(CompletionId id) const noexcept
        -> const Cell* {
        if (!id.valid() || id.index >= Capacity) {
            return nullptr;
        }
        const Cell& cell = cells_[id.index];
        return cell.generation == id.generation ? &cell : nullptr;
    }

    [[nodiscard]] auto cell(CompletionId id) noexcept -> Cell& {
        Cell* result = checked_cell(id);
        libk_assert(result != nullptr);
        return *result;
    }

    void recycle(CompletionId id) noexcept {
        Cell* cell_ptr = checked_cell(id);
        if (cell_ptr == nullptr) {
            return;
        }
        if (cell_ptr->generation >= GenerationLimit) {
            cell_ptr->state = CompletionCellState::Retired;
            return;
        }
        ++cell_ptr->generation;
        cell_ptr->state = CompletionCellState::Vacant;
        cell_ptr->result = {};
    }

    [[nodiscard]] auto publish(
        CompletionId id,
        CompletionResult result) noexcept -> bool {
        Cell* cell_ptr = checked_cell(id);
        if (cell_ptr == nullptr) {
            return false;
        }
        if (cell_ptr->state == CompletionCellState::Reserved) {
            cell_ptr->result = result;
            cell_ptr->state = CompletionCellState::Ready;
            return true;
        }
        if (cell_ptr->state == CompletionCellState::Detached) {
            recycle(id);
        }
        return false;
    }

    [[nodiscard]] auto cancel(CompletionId id) noexcept -> bool {
        Cell* cell_ptr = checked_cell(id);
        if (cell_ptr == nullptr) {
            return false;
        }
        if (cell_ptr->state == CompletionCellState::Detached) {
            recycle(id);
            return true;
        }
        return false;
    }

    [[nodiscard]] auto detach(CompletionId id) noexcept -> bool {
        Cell* cell_ptr = checked_cell(id);
        if (cell_ptr == nullptr) {
            return false;
        }
        if (cell_ptr->state == CompletionCellState::Reserved) {
            cell_ptr->state = CompletionCellState::Detached;
            return true;
        }
        if (cell_ptr->state == CompletionCellState::Ready) {
            recycle(id);
            return true;
        }
        return false;
    }

    Cell cells_[Capacity]{};
};

template<typename CompletionSetT>
class CompletionPublication final {
public:
    using Sender = typename CompletionSetT::Sender;

    CompletionPublication() noexcept = default;
    CompletionPublication(Sender&& sender, CompletionResult result) noexcept
        : sender_(libk::move(sender)), result_(result), active_(true) {}

    CompletionPublication(const CompletionPublication&) = delete;
    auto operator=(const CompletionPublication&) -> CompletionPublication& = delete;

    CompletionPublication(CompletionPublication&& other) noexcept
        : sender_(libk::move(other.sender_)), result_(other.result_),
          active_(other.active_) {
        other.active_ = false;
    }

    auto operator=(CompletionPublication&& other) noexcept
        -> CompletionPublication& {
        if (this == &other) {
            return *this;
        }
        publish();
        sender_ = libk::move(other.sender_);
        result_ = other.result_;
        active_ = other.active_;
        other.active_ = false;
        return *this;
    }

    ~CompletionPublication() noexcept { publish(); }

    void publish() noexcept {
        if (!active_) {
            return;
        }
        static_cast<void>(sender_.complete(result_));
        active_ = false;
    }

private:
    Sender sender_{};
    CompletionResult result_{};
    bool active_{};
};

template<typename Space>
class TaskRecord final {
public:
    using space_type = Space;
    using backend_type = typename Space::backend_type;

    TaskRecord(TaskId id, PlanLease&& plan, uint32_t plan_task) noexcept
        : id_(id), plan_(libk::move(plan)), plan_task_(plan_task) {
        libk_assert(id_.valid() && plan_.valid() && plan_.task(plan_task_).valid());
    }

    TaskRecord(const TaskRecord&) = delete;
    auto operator=(const TaskRecord&) -> TaskRecord& = delete;

    TaskRecord(TaskRecord&& other) noexcept
        : id_(other.id_), state_(other.state_), plan_(libk::move(other.plan_)),
          plan_task_(other.plan_task_), space_(libk::move(other.space_)),
          registrations_(libk::move(other.registrations_)),
          projections_(other.projections_), readiness_(other.readiness_),
          accounting_(other.accounting_),
          readiness_ready_(other.readiness_ready_),
          terminal_sequence_(other.terminal_sequence_),
          terminal_status_(other.terminal_status_) {
        other.id_ = {};
        other.state_ = TaskState::Reclaimed;
        other.plan_task_ = 0;
        other.projections_ = TaskProjections{};
        other.readiness_ = {};
        other.readiness_ready_ = false;
        other.accounting_ = ResidentAccounting{};
        other.terminal_sequence_ = 0;
        other.terminal_status_ = MYOS_STATUS_OK;
    }

    auto operator=(TaskRecord&& other) noexcept -> TaskRecord& {
        if (this == &other) {
            return *this;
        }
        if (state_ != TaskState::Reclaimed
            && (space_.phase() != Phase::Closed
                || registrations_.has_live_registrations())) {
            backend_type::ownership_fault(MYOS_STATUS_BUSY);
        }
        id_ = other.id_;
        state_ = other.state_;
        plan_ = libk::move(other.plan_);
        plan_task_ = other.plan_task_;
        space_ = libk::move(other.space_);
        registrations_ = libk::move(other.registrations_);
        projections_ = other.projections_;
        readiness_ = other.readiness_;
        accounting_ = other.accounting_;
        readiness_ready_ = other.readiness_ready_;
        terminal_sequence_ = other.terminal_sequence_;
        terminal_status_ = other.terminal_status_;
        other.id_ = {};
        other.state_ = TaskState::Reclaimed;
        other.plan_task_ = 0;
        other.projections_ = TaskProjections{};
        other.readiness_ = {};
        other.readiness_ready_ = false;
        other.accounting_ = ResidentAccounting{};
        other.terminal_sequence_ = 0;
        other.terminal_status_ = MYOS_STATUS_OK;
        return *this;
    }

    ~TaskRecord() noexcept = default;

    [[nodiscard]] constexpr auto id() const noexcept -> TaskId { return id_; }
    [[nodiscard]] constexpr auto state() const noexcept -> TaskState {
        return state_;
    }
    [[nodiscard]] auto readiness() const noexcept
        -> myos_deploy_readiness_policy_t {
        const PlanTask* row = plan().row();
        return row == nullptr
            ? MYOS_DEPLOY_READINESS_EXPLICIT
            : static_cast<myos_deploy_readiness_policy_t>(row->readiness);
    }
    [[nodiscard]] auto ready() const noexcept -> bool {
        if (state_ != TaskState::Running) {
            return false;
        }
        return readiness() == MYOS_DEPLOY_READINESS_EXPLICIT
            ? readiness_ready_ : true;
    }
    [[nodiscard]] constexpr auto terminal_sequence() const noexcept
        -> uint64_t {
        return terminal_sequence_;
    }
    [[nodiscard]] constexpr auto terminal_status() const noexcept
        -> myos_status_t {
        return terminal_status_;
    }
    [[nodiscard]] constexpr auto plan_task_id() const noexcept -> PlanTaskId {
        return PlanTaskId{plan_.id(), plan_task_};
    }
    [[nodiscard]] auto plan() const noexcept -> TaskPlanView {
        return plan_.task(plan_task_);
    }
    [[nodiscard]] auto plan_lease() const noexcept -> const PlanLease& {
        return plan_;
    }
    [[nodiscard]] constexpr auto projections() const noexcept
        -> const TaskProjections& {
        return projections_;
    }
    [[nodiscard]] constexpr auto accounting() const noexcept
        -> const ResidentAccounting& {
        return accounting_;
    }
    [[nodiscard]] constexpr auto has_resources() const noexcept -> bool {
        return space_.phase() != Phase::Closed
            || registrations_.has_live_registrations();
    }

    static void ownership_fault(myos_status_t status) noexcept {
        backend_type::ownership_fault(status);
    }

    /* Resolve every execution before the first syscall.  Once Starting is
     * published, already-started targets are external effects and are never
     * rolled back by a later failure. */
    template<typename B = backend_type>
    requires requires(cap::CapRef target) {
        { B::execution_start(target) } -> libk::SameAs<SysResult>;
    }
    [[nodiscard]] auto start() noexcept -> myos_status_t {
        if (state_ != TaskState::Prepared) {
            return MYOS_STATUS_BUSY;
        }
        const TaskPlanView task = plan();
        const PlanTask* row = task.row();
        if (row == nullptr || row->executions.count == 0
            || row->executions.count > MYOS_DEPLOY_TASK_EXECUTION_MAX) {
            if (!transition(TaskState::Failed)) {
                return MYOS_STATUS_INTERNAL;
            }
            return MYOS_STATUS_BAD_ARGS;
        }
        cap::CapRef targets[MYOS_DEPLOY_TASK_EXECUTION_MAX]{};
        myos_object_kind_t kinds[MYOS_DEPLOY_TASK_EXECUTION_MAX]{};
        for (uint32_t index = 0; index < row->executions.count; ++index) {
            const PlanExecution* execution = task.execution(index);
            if (execution == nullptr) {
                static_cast<void>(transition(TaskState::Failed));
                return MYOS_STATUS_BAD_ARGS;
            }
            kinds[index] = execution->model == MYOS_DEPLOY_EXECUTION_THREAD
                ? MYOS_OBJECT_KIND_THREAD : MYOS_OBJECT_KIND_VPROC;
            const SlotProjection& projection = projections_.executions[index];
            const auto target = resolve(projection, kinds[index]);
            if (!target) {
                static_cast<void>(transition(TaskState::Failed));
                return MYOS_STATUS_INVALID_CAP;
            }
            targets[index] = target.value();
        }
        if (!transition(TaskState::Starting)) {
            return MYOS_STATUS_INTERNAL;
        }
        for (uint32_t index = 0; index < row->executions.count; ++index) {
            const SysResult result = B::execution_start(targets[index]);
            if (result.status != MYOS_STATUS_OK) {
                static_cast<void>(transition(TaskState::Failed));
                return result.status;
            }
        }
        if (!transition(TaskState::Running)) {
            static_cast<void>(transition(TaskState::Failed));
            return MYOS_STATUS_INTERNAL;
        }
        return MYOS_STATUS_OK;
    }

private:
    /* Selector-bearing resolution and mutable construction state stay behind
     * the TaskBuilder/TaskTable friendship boundary.  Public observers only
     * receive immutable projections and accounting snapshots. */
    [[nodiscard]] auto resolve(
        const SlotProjection& projection,
        myos_object_kind_t expected_kind) const noexcept
        -> libk::optional<myos::cap::CapRef> {
        /* The readiness relation is a TaskTable-owned operation.  Even if a
         * caller reconstructs the same local slot from a public object view,
         * the generic resolver must not disclose that selector. */
        if (projection.projection == ProjectionKind::Local
            && readiness_.projection == ProjectionKind::Local
            && projection.local.pool == readiness_.local.pool
            && projection.local.index == readiness_.local.index
            && projection.local.kind == readiness_.local.kind) {
            return libk::nullopt;
        }
        return resolve_internal(projection, expected_kind);
    }

    [[nodiscard]] auto space() const noexcept -> const Space& { return space_; }
    [[nodiscard]] auto space() noexcept -> Space& { return space_; }
    [[nodiscard]] auto mutable_projections() noexcept -> TaskProjections& {
        return projections_;
    }
    [[nodiscard]] auto mutable_accounting() noexcept -> ResidentAccounting& {
        return accounting_;
    }

    template<typename B = backend_type>
    requires requires(cap::CapRef notification) {
        { B::notification_take(notification) } -> libk::SameAs<SysResult>;
    }
    [[nodiscard]] auto consume_readiness() noexcept -> myos_status_t {
        if (readiness() != MYOS_DEPLOY_READINESS_EXPLICIT
            || state_ != TaskState::Running) {
            return MYOS_STATUS_BUSY;
        }
        if (readiness_ready_) {
            return MYOS_STATUS_RETRY;
        }
        const auto notification = resolve_readiness();
        if (!notification) {
            return MYOS_STATUS_INVALID_CAP;
        }
        const SysResult result = B::notification_take(notification.value());
        if (result.status != MYOS_STATUS_OK) {
            return result.status;
        }
        if (result.value == 0) {
            return MYOS_STATUS_RETRY;
        }
        readiness_ready_ = true;
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto terminal_notification() const noexcept
        -> libk::optional<myos::cap::CapRef> {
        if (state_ != TaskState::Starting && state_ != TaskState::Running
            && state_ != TaskState::Terminating) {
            return libk::nullopt;
        }
        const TaskPlanView task = plan();
        const PlanTask* const row = task.row();
        if (row == nullptr || row->executions.count != 1) {
            return libk::nullopt;
        }
        const SlotProjection& relation = projections_.relations[0];
        if (!relation.valid()
            || relation.kind != MYOS_OBJECT_KIND_NOTIFICATION) {
            return libk::nullopt;
        }
        return resolve_internal(relation, MYOS_OBJECT_KIND_NOTIFICATION);
    }

public:
    template<typename B = backend_type>
    requires requires(cap::CapRef target) {
        { B::terminal_query(target) } -> libk::SameAs<SysResult>;
    }
    [[nodiscard]] auto observe_terminal() const noexcept -> SysResult {
        if (state_ != TaskState::Starting && state_ != TaskState::Running
            && state_ != TaskState::Terminating) {
            return SysResult{.status = MYOS_STATUS_BUSY};
        }
        const TaskPlanView task = plan();
        const PlanTask* row = task.row();
        if (row == nullptr || row->executions.count != 1) {
            /* The current production envelope is single-execution.  A
             * multi-execution aggregation policy needs its own explicit
             * terminal contract and is not guessed here. */
            return SysResult{.status = MYOS_STATUS_BAD_ARGS};
        }
        const PlanExecution* execution = task.execution(0);
        if (execution == nullptr) {
            return SysResult{.status = MYOS_STATUS_BAD_ARGS};
        }
        const myos_object_kind_t kind =
            execution->model == MYOS_DEPLOY_EXECUTION_THREAD
            ? MYOS_OBJECT_KIND_THREAD : MYOS_OBJECT_KIND_VPROC;
        const auto target = resolve(projections_.executions[0], kind);
        if (!target) {
            return SysResult{.status = MYOS_STATUS_INVALID_CAP};
        }
        return B::terminal_query(target.value());
    }

    /* Admit one fresh kernel terminal generation.  Repeated or stale
     * observations are ignored; only the first accepted sequence chooses the
     * Task result and lifecycle edge. */
    [[nodiscard]] auto consume_terminal(
        const SysResult& observation) noexcept -> myos_status_t {
        if (observation.status != MYOS_STATUS_OK) {
            return observation.status;
        }
        if (observation.value == 0) {
            return MYOS_STATUS_RETRY;
        }
        if (observation.value <= terminal_sequence_) {
            return MYOS_STATUS_RETRY;
        }
        if (state_ != TaskState::Starting && state_ != TaskState::Running) {
            return MYOS_STATUS_BUSY;
        }
        terminal_sequence_ = observation.value;
        terminal_status_ = static_cast<myos_status_t>(
            static_cast<int64_t>(observation.value2));
        if (terminal_status_ == MYOS_STATUS_OK) {
            return transition(TaskState::Terminating)
                ? MYOS_STATUS_OK : MYOS_STATUS_INTERNAL;
        }
        return transition(TaskState::Failed)
            ? MYOS_STATUS_OK : MYOS_STATUS_INTERNAL;
    }

private:
    /* Admit a PreparedKey source only after construction has committed the
     * TaskRecord.  RegistrationOwner retains the reciprocal source token; the
     * returned AuthorityId is merely the checked handle handed to the caller.
     */
    template<typename Authorities>
    [[nodiscard]] auto register_prepared_export(
        Authorities& authorities,
        uint32_t export_index,
        uint64_t identity) noexcept -> libk::optional<AuthorityId> {
        if (state_ != TaskState::Prepared) {
            return libk::nullopt;
        }
        const TaskPlanView task = plan();
        const PlanTask* const row = task.row();
        if (row == nullptr || export_index >= row->exports.count
            || export_index >= MYOS_DEPLOY_TASK_EXPORT_MAX) {
            return libk::nullopt;
        }
        const PlanExport* const export_row =
            task.export_record(export_index);
        if (export_row == nullptr
            || export_row->source_class != MYOS_DEPLOY_EXPORT_PREPARED_KEY
            || !valid_authority_ceiling(export_row->ceiling)) {
            return libk::nullopt;
        }
        const SourceProjection& projection = projections_.exports[export_index];
        if (!projection.valid()
            || projection.kind != export_row->ceiling.kind) {
            return libk::nullopt;
        }
        const auto source = resolve_source(projection, projection.kind);
        if (!source || source->cspace != 0) {
            return libk::nullopt;
        }
        return registrations_.register_source(
            authorities, source.value(), identity, export_row->ceiling);
    }

private:
    template<typename, typename, size_t, uint32_t>
    friend class TaskTable;
    template<typename, typename>
    friend class TaskBuilder;

    [[nodiscard]] auto resolve_readiness() const noexcept
        -> libk::optional<myos::cap::CapRef> {
        if (readiness_.projection != ProjectionKind::Local
            || !readiness_.valid()
            || readiness_.kind != MYOS_OBJECT_KIND_NOTIFICATION) {
            return libk::nullopt;
        }
        return space_.lookup(readiness_.local, MYOS_OBJECT_KIND_NOTIFICATION);
    }

    [[nodiscard]] auto resolve_internal(
        const SlotProjection& projection,
        myos_object_kind_t expected_kind) const noexcept
        -> libk::optional<myos::cap::CapRef> {
        if (!projection.valid()) {
            return libk::nullopt;
        }
        if (projection.projection == ProjectionKind::Local) {
            return space_.lookup(projection.local, expected_kind);
        }
        if (projection.kind != expected_kind) {
            return libk::nullopt;
        }
        return space_.lookup_remote(
            projection.remote_index, projection.manager);
    }

    [[nodiscard]] auto resolve_source(
        const SourceProjection& projection,
        myos_object_kind_t expected_kind) const noexcept
        -> libk::optional<myos::cap::CapRef> {
        if (!projection.valid() || projection.kind != expected_kind) {
            return libk::nullopt;
        }
        if (projection.projection == SourceProjectionKind::Pool) {
            return expected_kind == MYOS_OBJECT_KIND_RESOURCE_POOL
                ? space_.pool() : libk::nullopt;
        }
        return space_.lookup(projection.local, expected_kind);
    }

    [[nodiscard]] auto close_space() noexcept -> myos_status_t {
        return registrations_.close(space_);
    }

    [[nodiscard]] auto install_import_projection(
        size_t index,
        const ImportProjection& projection) noexcept -> bool {
        if (index >= MYOS_DEPLOY_TASK_IMPORT_MAX || !projection.valid()) {
            return false;
        }
        projections_.imports[index] = SlotProjection{
            .projection = ProjectionKind::Remote,
            .remote_index = projection.remote_index,
            .manager = projection.manager,
            .kind = projection.kind,
            .authority = projection.authority};
        return true;
    }

    [[nodiscard]] static constexpr auto legal(
        TaskState from,
        TaskState to) noexcept -> bool {
        switch (from) {
        case TaskState::Constructing:
            return to == TaskState::Prepared || to == TaskState::Failed;
        case TaskState::Prepared:
            return to == TaskState::Starting || to == TaskState::Failed;
        case TaskState::Starting:
            return to == TaskState::Running || to == TaskState::Terminating
                || to == TaskState::Failed;
        case TaskState::Running:
            return to == TaskState::Terminating || to == TaskState::Failed;
        case TaskState::Terminating:
            return to == TaskState::Closing;
        case TaskState::Failed:
            return to == TaskState::Closing;
        case TaskState::Closing:
            return to == TaskState::Reclaimed;
        case TaskState::Reclaimed:
            return false;
        }
        return false;
    }

    [[nodiscard]] auto transition(TaskState to) noexcept -> bool {
        if (!legal(state_, to)) {
            return false;
        }
        state_ = to;
        return true;
    }

    TaskId id_{};
    TaskState state_{TaskState::Constructing};
    PlanLease plan_{};
    uint32_t plan_task_{};
    Space space_{};
    RegistrationOwner<MYOS_DEPLOY_TASK_EXPORT_MAX> registrations_{};
    TaskProjections projections_{};
    /* This selector is deliberately not part of the public projection view;
     * only TaskBuilder may publish it and TaskTable may consume it through
     * consume_readiness(TaskId). */
    SlotProjection readiness_{};
    ResidentAccounting accounting_{};
    bool readiness_ready_{};
    uint64_t terminal_sequence_{};
    myos_status_t terminal_status_{};
};

template<typename Record,
         typename CompletionSetT,
         size_t Capacity = MYOS_DEPLOY_TASK_MAX,
         uint32_t GenerationLimit = UINT32_MAX>
class TaskTable final {
    static_assert(Capacity != 0);
    static_assert(GenerationLimit != 0);

public:
    using record_type = Record;
    using completion_set_type = CompletionSetT;
    using sender_type = typename CompletionSetT::Sender;
    using receiver_type = typename CompletionSetT::Receiver;
    using publication_type = CompletionPublication<CompletionSetT>;

    class ClosingRecord final {
    public:
        ClosingRecord(
            TaskId id,
            PlanLease&& plan,
            uint32_t plan_task,
            sender_type&& sender
            ) noexcept
            : record_(id, libk::move(plan), plan_task),
              sender_(libk::move(sender)) {}

        ClosingRecord(const ClosingRecord&) = delete;
        auto operator=(const ClosingRecord&) -> ClosingRecord& = delete;
        ClosingRecord(ClosingRecord&&) = delete;
        auto operator=(ClosingRecord&&) -> ClosingRecord& = delete;
        ~ClosingRecord() noexcept = default;

        [[nodiscard]] auto begin_close(
            CloseReason reason,
            myos_status_t status) noexcept -> bool {
            /* A normal terminal winner already owns the Terminating edge;
             * preserve that state while entering Closing.  Construction,
             * startup and fatal outcomes are the only paths that first pass
             * through Failed. */
            if (record_.state() == TaskState::Terminating) {
                if (!record_.transition(TaskState::Closing)) {
                    return false;
                }
            } else {
                if (record_.state() != TaskState::Failed
                    && !record_.transition(TaskState::Failed)) {
                    return false;
                }
                if (!record_.transition(TaskState::Closing)) {
                    return false;
                }
            }
            result_ = CompletionResult{record_.id(), reason, status};
            sender_.seal();
            return true;
        }

        void seal_sender() noexcept { sender_.seal(); }

        [[nodiscard]] auto continue_close() noexcept -> myos_status_t {
            const myos_status_t status = record_.close_space();
            if (status != MYOS_STATUS_OK) {
                return status;
            }
            return record_.space().phase() == Phase::Closed
                ? MYOS_STATUS_OK : MYOS_STATUS_BUSY;
        }

        [[nodiscard]] auto record() noexcept -> Record& { return record_; }
        [[nodiscard]] auto record() const noexcept -> const Record& {
            return record_;
        }
        [[nodiscard]] constexpr auto result() const noexcept
            -> CompletionResult {
            return result_;
        }
    private:
        friend class TaskTable;
        [[nodiscard]] auto cancel_sender() noexcept -> bool {
            return sender_.cancel();
        }
        [[nodiscard]] auto take_sender() noexcept -> sender_type {
            return libk::move(sender_);
        }

        Record record_;
        sender_type sender_{};
        CompletionResult result_{};
    };

private:
    template<typename, typename>
    friend class TaskBuilder;

    /*
     * The variant index is the only inactive/active storage discriminator.
     * ActiveSlot keeps the one Record and Sender in stable table storage from
     * Constructing through Closing; TaskState is the record's sole lifecycle
     * truth and TaskSlotTag is derived from it.  Reservation is only a small
     * checked token granting the Builder logical ownership of Constructing.
     */
    struct VacantSlot final {};
    struct ActiveSlot final {
        ActiveSlot(
            TaskId id,
            PlanLease&& plan,
            uint32_t plan_task,
            sender_type&& sender) noexcept
            : closing(id, libk::move(plan), plan_task, libk::move(sender)) {}

        ClosingRecord closing;
    };

    struct RetiredSlot final {};

    using SlotPayload = libk::variant<VacantSlot, ActiveSlot, RetiredSlot>;

    struct Slot final {
        uint32_t generation{1};
        SlotPayload payload{};
    };

public:

    class Reservation final {
    public:
        Reservation() noexcept = default;
        Reservation(const Reservation&) = delete;
        auto operator=(const Reservation&) -> Reservation& = delete;

        Reservation(Reservation&& other) noexcept
            : table_(other.table_), id_(other.id_), active_(other.active_) {
            other.table_ = nullptr;
            other.id_ = {};
            other.active_ = false;
        }

        auto operator=(Reservation&& other) noexcept -> Reservation& {
            if (this == &other) {
                return *this;
            }
            abandon();
            table_ = other.table_;
            id_ = other.id_;
            active_ = other.active_;
            other.table_ = nullptr;
            other.id_ = {};
            other.active_ = false;
            return *this;
        }

        ~Reservation() noexcept { abandon(); }

        [[nodiscard]] constexpr auto valid() const noexcept -> bool {
            return table_ != nullptr && active_ && id_.valid();
        }
        [[nodiscard]] constexpr auto id() const noexcept -> TaskId { return id_; }
        [[nodiscard]] auto record() noexcept -> Record& {
            Record* record = table_ == nullptr
                ? nullptr : table_->reservation_record(id_);
            libk_assert(record != nullptr);
            return *record;
        }
        [[nodiscard]] auto record() const noexcept -> const Record& {
            const Record* record = table_ == nullptr
                ? nullptr : table_->reservation_record(id_);
            libk_assert(record != nullptr);
            return *record;
        }

        [[nodiscard]] auto cancel() noexcept -> bool {
            if (!valid() || record().has_resources()) {
                return false;
            }
            if (!table_->cancel_reservation(id_)) {
                return false;
            }
            active_ = false;
            table_ = nullptr;
            id_ = {};
            return true;
        }

        [[nodiscard]] auto commit_prepared() noexcept -> bool {
            return valid() && table_->commit(*this);
        }

        [[nodiscard]] auto fail(
            CloseReason reason,
            myos_status_t status) noexcept -> bool {
            return valid() && table_->move_to_closing(*this, reason, status);
        }

        private:
        friend class TaskTable;
        Reservation(
            TaskTable* table,
            TaskId id) noexcept
            : table_(table), id_(id), active_(true) {}

        void abandon() noexcept {
            if (!active_) {
                return;
            }
            if (record().has_resources()) {
                Record::ownership_fault(MYOS_STATUS_BUSY);
                return;
            }
            if (!table_->cancel_reservation(id_)) {
                Record::ownership_fault(MYOS_STATUS_BUSY);
                return;
            }
            active_ = false;
            table_ = nullptr;
            id_ = {};
        }

        TaskTable* table_{};
        TaskId id_{};
        bool active_{};
    };

    TaskTable() noexcept = default;
    TaskTable(const TaskTable&) = delete;
    auto operator=(const TaskTable&) -> TaskTable& = delete;

    ~TaskTable() noexcept {
        for (const Slot& slot : slots_) {
            if (slot_tag(slot) != TaskSlotTag::Vacant
                && slot_tag(slot) != TaskSlotTag::Retired) {
                Record::ownership_fault(MYOS_STATUS_BUSY);
            }
        }
    }

private:
    [[nodiscard]] auto reserve(
        sender_type&& sender,
        PlanLease&& plan,
        uint32_t plan_task) noexcept -> libk::optional<Reservation> {
        if (!sender.valid() || !plan.valid()) {
            return libk::nullopt;
        }
        if (!plan.task(plan_task).valid()) {
            return libk::nullopt;
        }
        for (size_t index = 0; index < Capacity; ++index) {
            Slot& slot = slots_[index];
            if (slot_tag(slot) != TaskSlotTag::Vacant) {
                continue;
            }
            const TaskId id{static_cast<uint32_t>(index), slot.generation};
            slot.payload.template emplace<ActiveSlot>(
                id, libk::move(plan), plan_task, libk::move(sender));
            return Reservation{this, id};
        }
        return libk::nullopt;
    }

public:
    [[nodiscard]] auto tag(TaskId id) const noexcept -> TaskSlotTag {
        const Slot* slot = checked_slot(id);
        return slot == nullptr ? TaskSlotTag::Retired : slot_tag(*slot);
    }

    [[nodiscard]] auto record(TaskId id) noexcept -> Record* {
        Slot* slot = checked_slot(id);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Record) {
            return nullptr;
        }
        auto* payload = libk::get_if<ActiveSlot>(&slot->payload);
        return payload == nullptr ? nullptr : &payload->closing.record();
    }

    [[nodiscard]] auto record(TaskId id) const noexcept -> const Record* {
        const Slot* slot = checked_slot(id);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Record) {
            return nullptr;
        }
        const auto* payload = libk::get_if<ActiveSlot>(&slot->payload);
        return payload == nullptr ? nullptr : &payload->closing.record();
    }

    [[nodiscard]] auto closing(TaskId id) noexcept -> ClosingRecord* {
        Slot* slot = checked_slot(id);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Closing) {
            return nullptr;
        }
        auto* payload = libk::get_if<ActiveSlot>(&slot->payload);
        return payload == nullptr ? nullptr : &payload->closing;
    }

    [[nodiscard]] auto transition(TaskId id, TaskState next) noexcept -> bool {
        Slot* slot = checked_slot(id);
        Record* record_ptr = record(id);
        if (slot == nullptr || record_ptr == nullptr) {
            return false;
        }
        return record_ptr->transition(next);
    }

    template<typename B = typename record_type::backend_type>
    requires requires(cap::CapRef target) {
        { B::execution_start(target) } -> libk::SameAs<SysResult>;
    }
    [[nodiscard]] auto start(TaskId id) noexcept -> myos_status_t {
        Record* const record_ptr = record(id);
        return record_ptr == nullptr
            ? MYOS_STATUS_INVALID_CAP : record_ptr->template start<B>();
    }

    template<typename B = typename record_type::backend_type>
    requires requires(cap::CapRef target) {
        { B::terminal_query(target) } -> libk::SameAs<SysResult>;
    }
    [[nodiscard]] auto observe_terminal(TaskId id) const noexcept -> SysResult {
        const Record* const record_ptr = record(id);
        return record_ptr == nullptr
            ? SysResult{.status = MYOS_STATUS_INVALID_CAP}
            : record_ptr->template observe_terminal<B>();
    }

    /* Return only the checked terminal wake relation used by supervision.
     * Generic local-slot resolution remains private to TaskRecord so a
     * caller cannot recover readiness or Prepared-export selectors from its
     * immutable projections. */
    [[nodiscard]] auto terminal_notification(TaskId id) const noexcept
        -> libk::optional<myos::cap::CapRef> {
        const Record* const record_ptr = record(id);
        return record_ptr == nullptr
            ? libk::nullopt : record_ptr->terminal_notification();
    }

    [[nodiscard]] auto consume_terminal(
        TaskId id,
        const SysResult& observation) noexcept -> myos_status_t {
        Record* const record_ptr = record(id);
        return record_ptr == nullptr
            ? MYOS_STATUS_INVALID_CAP
            : record_ptr->consume_terminal(observation);
    }

    template<typename B = typename record_type::backend_type>
    requires requires(cap::CapRef notification) {
        { B::notification_take(notification) } -> libk::SameAs<SysResult>;
    }
    [[nodiscard]] auto consume_readiness(TaskId id) noexcept -> myos_status_t {
        Record* const record_ptr = record(id);
        return record_ptr == nullptr
            ? MYOS_STATUS_INVALID_CAP
            : record_ptr->template consume_readiness<B>();
    }

    /* Register one PreparedKey export through the source TaskRecord.  The
     * caller supplies no identity: it is an injective, checked encoding of
     * the live TaskId and export row, so stale task generations cannot alias a
     * previous source entry. */
    template<typename Authorities>
    [[nodiscard]] auto register_prepared_export(
        TaskId id,
        uint32_t export_index,
        Authorities& authorities) noexcept
        -> libk::optional<AuthorityId> {
        Record* const record_ptr = record(id);
        if (record_ptr == nullptr) {
            return libk::nullopt;
        }
        const auto identity = export_identity(id, export_index);
        if (!identity) {
            return libk::nullopt;
        }
        return record_ptr->register_prepared_export(
            authorities, export_index, identity.value());
    }

    /* A policy owner calls this only for a normal/external termination
     * decision.  Faults use begin_close directly so Failed remains distinct
     * from the normal Terminating path. */
    [[nodiscard]] auto terminate(
        TaskId id,
        CloseReason reason,
        myos_status_t status) noexcept -> bool {
        Slot* const slot = checked_slot(id);
        Record* const record_ptr = record(id);
        if (slot == nullptr || record_ptr == nullptr
            || (record_ptr->state() != TaskState::Running
                && record_ptr->state() != TaskState::Starting)
            || !record_ptr->transition(TaskState::Terminating)) {
            return false;
        }
        auto* payload = libk::get_if<ActiveSlot>(&slot->payload);
        return payload != nullptr
            && payload->closing.begin_close(reason, status);
    }

    [[nodiscard]] auto begin_close(
        TaskId id,
        CloseReason reason,
        myos_status_t status) noexcept -> bool {
        Slot* slot = checked_slot(id);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Record) {
            return false;
        }
        auto* payload = libk::get_if<ActiveSlot>(&slot->payload);
        if (payload == nullptr
            || !payload->closing.begin_close(reason, status)) {
            return false;
        }
        return true;
    }

    [[nodiscard]] auto continue_close(TaskId id) noexcept -> myos_status_t {
        Slot* slot = checked_slot(id);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Closing) {
            return MYOS_STATUS_INVALID_CAP;
        }
        auto* payload = libk::get_if<ActiveSlot>(&slot->payload);
        if (payload == nullptr) {
            return MYOS_STATUS_INVALID_CAP;
        }
        const myos_status_t status = payload->closing.continue_close();
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        if (!payload->closing.record().transition(TaskState::Reclaimed)) {
            return MYOS_STATUS_INTERNAL;
        }
        const CompletionResult result = payload->closing.result();
        publication_type publication{
            payload->closing.take_sender(), result};
        const bool retire = slot->generation >= GenerationLimit;
        slot->payload.template emplace<VacantSlot>();
        if (retire) {
            slot->payload.template emplace<RetiredSlot>();
        } else {
            ++slot->generation;
        }
        publication.publish();
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] constexpr auto capacity() const noexcept -> size_t {
        return Capacity;
    }

private:
    [[nodiscard]] static auto export_identity(
        TaskId id,
        uint32_t export_index) noexcept -> libk::optional<uint64_t> {
        if (!id.valid() || id.slot >= Capacity
            || export_index >= MYOS_DEPLOY_TASK_EXPORT_MAX) {
            return libk::nullopt;
        }
        const auto generation = libk::checked_multiply<uint64_t>(
            static_cast<uint64_t>(id.generation),
            static_cast<uint64_t>(Capacity));
        if (!generation) {
            return libk::nullopt;
        }
        const auto slot = libk::checked_add<uint64_t>(
            generation.value(), static_cast<uint64_t>(id.slot));
        if (!slot) {
            return libk::nullopt;
        }
        const auto row = libk::checked_multiply<uint64_t>(
            slot.value(), static_cast<uint64_t>(MYOS_DEPLOY_TASK_EXPORT_MAX));
        if (!row) {
            return libk::nullopt;
        }
        return libk::checked_add<uint64_t>(
            row.value(), static_cast<uint64_t>(export_index));
    }

    [[nodiscard]] static auto slot_tag(const Slot& slot) noexcept
        -> TaskSlotTag {
        if (libk::holds_alternative<VacantSlot>(slot.payload)) {
            return TaskSlotTag::Vacant;
        }
        if (const auto* active = libk::get_if<ActiveSlot>(&slot.payload)) {
            switch (active->closing.record().state()) {
            case TaskState::Constructing:
                return TaskSlotTag::Reserved;
            case TaskState::Closing:
                return TaskSlotTag::Closing;
            case TaskState::Reclaimed:
                libk_assert(false);
                return TaskSlotTag::Retired;
            case TaskState::Prepared:
            case TaskState::Starting:
            case TaskState::Running:
            case TaskState::Terminating:
            case TaskState::Failed:
                return TaskSlotTag::Record;
            }
        }
        return TaskSlotTag::Retired;
    }

    [[nodiscard]] auto checked_slot(TaskId id) noexcept -> Slot* {
        if (!id.valid() || id.slot >= Capacity) {
            return nullptr;
        }
        Slot& slot = slots_[id.slot];
        return slot.generation == id.generation
            && slot_tag(slot) != TaskSlotTag::Retired ? &slot : nullptr;
    }

    [[nodiscard]] auto checked_slot(TaskId id) const noexcept -> const Slot* {
        if (!id.valid() || id.slot >= Capacity) {
            return nullptr;
        }
        const Slot& slot = slots_[id.slot];
        return slot.generation == id.generation
            && slot_tag(slot) != TaskSlotTag::Retired ? &slot : nullptr;
    }

    [[nodiscard]] auto reservation_record(TaskId id) noexcept -> Record* {
        Slot* slot = checked_slot(id);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Reserved) {
            return nullptr;
        }
        ActiveSlot* active = libk::get_if<ActiveSlot>(&slot->payload);
        return active == nullptr ? nullptr : &active->closing.record();
    }

    [[nodiscard]] auto reservation_record(TaskId id) const noexcept
        -> const Record* {
        const Slot* slot = checked_slot(id);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Reserved) {
            return nullptr;
        }
        const ActiveSlot* active = libk::get_if<ActiveSlot>(&slot->payload);
        return active == nullptr ? nullptr : &active->closing.record();
    }

    [[nodiscard]] auto cancel_reservation(TaskId id) noexcept -> bool {
        Slot* slot = checked_slot(id);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Reserved) {
            return false;
        }
        ActiveSlot* active = libk::get_if<ActiveSlot>(&slot->payload);
        if (active == nullptr || !active->closing.cancel_sender()) {
            return false;
        }
        slot->payload.template emplace<VacantSlot>();
        return true;
    }

    [[nodiscard]] auto commit(Reservation& reservation) noexcept -> bool {
        Slot* slot = checked_slot(reservation.id_);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Reserved
            || !reservation.record().transition(TaskState::Prepared)) {
            return false;
        }
        ActiveSlot* active = libk::get_if<ActiveSlot>(&slot->payload);
        if (active == nullptr) {
            return false;
        }
        active->closing.seal_sender();
        reservation.active_ = false;
        reservation.table_ = nullptr;
        reservation.id_ = {};
        return true;
    }

    [[nodiscard]] auto move_to_closing(
        Reservation& reservation,
        CloseReason reason,
        myos_status_t status) noexcept -> bool {
        Slot* slot = checked_slot(reservation.id_);
        if (slot == nullptr || slot_tag(*slot) != TaskSlotTag::Reserved) {
            return false;
        }
        ActiveSlot* active = libk::get_if<ActiveSlot>(&slot->payload);
        if (active == nullptr || !active->closing.begin_close(reason, status)) {
            return false;
        }
        reservation.active_ = false;
        reservation.table_ = nullptr;
        reservation.id_ = {};
        return true;
    }

    Slot slots_[Capacity]{};
};

template<typename Table, typename CompletionSetT>
class TaskBuilder final {
public:
    using table_type = Table;
    using completion_set_type = CompletionSetT;
    using reservation_type = typename Table::Reservation;
    using receiver_type = typename CompletionSetT::Receiver;
    using record_type = typename Table::record_type;
    using space_type = typename record_type::space_type;
    using backend_type = typename space_type::backend_type;
    using owner_type = typename space_type::owner_type;

    TaskBuilder() noexcept = default;
    TaskBuilder(const TaskBuilder&) = delete;
    auto operator=(const TaskBuilder&) -> TaskBuilder& = delete;

    TaskBuilder(TaskBuilder&& other) noexcept
        : table_(other.table_), reservation_(libk::move(other.reservation_)),
          receiver_(libk::move(other.receiver_)) {
        other.table_ = nullptr;
    }

    auto operator=(TaskBuilder&& other) noexcept -> TaskBuilder& {
        if (this == &other) {
            return *this;
        }
        abandon();
        table_ = other.table_;
        reservation_ = libk::move(other.reservation_);
        receiver_ = libk::move(other.receiver_);
        other.table_ = nullptr;
        return *this;
    }

    ~TaskBuilder() noexcept { abandon(); }

    [[nodiscard]] static auto begin(
        completion_set_type& completions,
        table_type& table,
        PlanLease&& plan,
        uint32_t plan_task) noexcept -> libk::optional<TaskBuilder> {
        const TaskPlanView task = plan.task(plan_task);
        if (!task.valid() || !imports_admissible(task)) {
            // Move is a reserved wire value, not an immediate syscall mode.
            // Reject before CompletionSet or TaskTable reservation so this
            // policy has no resource, lease or publication side effect.
            return libk::nullopt;
        }
        auto pair = completions.reserve();
        if (!pair) {
            return libk::nullopt;
        }
        auto sender = pair->take_sender();
        auto receiver = pair->take_receiver();
        auto reservation = table.reserve(
            libk::move(sender), libk::move(plan), plan_task);
        if (!reservation) {
            static_cast<void>(receiver.detach());
            static_cast<void>(sender.cancel());
            return libk::nullopt;
        }
        return TaskBuilder{
            &table, libk::move(*reservation), libk::move(receiver)};
    }

    [[nodiscard]] auto valid() const noexcept -> bool {
        return table_ != nullptr && reservation_.has_value();
    }
    [[nodiscard]] auto record() const noexcept
        -> const typename Table::record_type* {
        return valid() ? &reservation_->record() : nullptr;
    }

    /* Execute the finite unpublished construction sequence.  The decoded
     * plan remains the policy source; bindings and bootstrap bytes are
     * explicit caller inputs, while every selector produced below enters the
     * TaskSpace owner before the next fallible operation. */
    template<typename Authorities>
    requires ConstructionBackend<backend_type>
    [[nodiscard]] auto construct(
        const TaskConstructionInput<backend_type, Authorities>& input,
        Authorities& authorities) noexcept -> myos_status_t {
        if (!valid() || input.bindings == nullptr
            || input.scratch == nullptr || !input.parent_pool
            || !input.workspace.empty()) {
            return MYOS_STATUS_BAD_ARGS;
        }
        using Workspace = TaskConstructionWorkspace<Authorities>;
        Workspace& workspace = input.workspace;
        struct WorkspaceCleanup final {
            Workspace* workspace{};
            ~WorkspaceCleanup() noexcept { workspace->clear(); }
        } cleanup{&workspace};

        auto& record = reservation_->record();
        const TaskPlanView task = record.plan();
        const PlanTask* const row = task.row();
        if (row == nullptr) {
            return MYOS_STATUS_BAD_ARGS;
        }
        /* Bootstrap rows own the envelope contents.  A caller-provided byte
         * snapshot is the legacy path and cannot coexist with that policy. */
        if (row->bootstraps.count != 0
            && (input.bootstrap != nullptr || input.bootstrap_size != 0)) {
            return MYOS_STATUS_BAD_ARGS;
        }

        /* Capacity is checked from the same cumulative terms that bound the
         * public maxima.  This runs before the first authority lease or
         * TaskSpace acquisition, so an undersized specialization has no
         * observable transaction side effect. */
        size_t local_demand{};
        size_t remote_demand{};
        size_t lease_demand{};
        bool typed_imports{};
        if (!capacity_demand(task, *row, local_demand, remote_demand,
                             lease_demand, typed_imports)
            || local_demand > space_type::local_capacity()
            || remote_demand > space_type::remote_capacity()
            || lease_demand > Authorities::lease_capacity()) {
            return MYOS_STATUS_NO_MEMORY;
        }

        /* Plan references are global decoded-table indices while the typed
         * TaskPlanView accessors intentionally take a task-local index.  Keep
         * the conversion at this construction boundary so every later
         * projection lookup addresses the same row that policy validated. */
        const auto mapping_local = [row](uint32_t reference) noexcept
            -> uint32_t {
            if (reference == MYOS_DEPLOY_NO_INDEX
                || reference < row->mappings.first
                || reference - row->mappings.first >= row->mappings.count) {
                return MYOS_DEPLOY_NO_INDEX;
            }
            return reference - row->mappings.first;
        };
        const uint32_t bootstrap_mapping =
            mapping_local(row->bootstrap_mapping);

        /* Domain and Pager bindings are admitted before opening the child;
         * their move-only leases remain live through all constructor uses. */
        for (uint32_t index = 0; index < row->executions.count; ++index) {
            const AuthorityId id = input.bindings->domains[index];
            if (!id.valid()) {
                return MYOS_STATUS_BAD_ARGS;
            }
            auto lease = authorities.lease(id);
            if (!lease || !lease->valid()
                || lease->source().cspace != 0
                || lease->ceiling().kind != MYOS_OBJECT_KIND_SCHED_DOMAIN) {
                return MYOS_STATUS_DENIED;
            }
            workspace.domain_leases[index] = libk::move(*lease);
        }
        for (uint32_t index = 0; index < row->mappings.count; ++index) {
            const PlanMapping* const mapping = task.mapping(index);
            if (mapping == nullptr) {
                return MYOS_STATUS_BAD_ARGS;
            }
            if (mapping->source != MYOS_DEPLOY_MAPPING_SOURCE_PAGER) {
                continue;
            }
            const AuthorityId id = input.bindings->pagers[index];
            if (!id.valid()) {
                return MYOS_STATUS_BAD_ARGS;
            }
            auto lease = authorities.lease(id);
            if (!lease || !lease->valid()
                || lease->source().cspace != 0
                || lease->ceiling().kind != MYOS_OBJECT_KIND_PAGER) {
                return MYOS_STATUS_DENIED;
            }
            workspace.pager_leases[index] = libk::move(*lease);
        }

        if (!input.bundle || input.bundle->phase() != LeasePhase::Mapped) {
            return MYOS_STATUS_BAD_ARGS;
        }
        const myos_status_t opened_status = record.space().open(
                input.parent_pool,
                static_cast<myos_word_t>(row->pool_memory),
                static_cast<myos_word_t>(row->pool_caps),
                static_cast<myos_word_t>(row->kind_mask),
                static_cast<myos_word_t>(row->cspace_slots),
                static_cast<myos_word_t>(row->cspace_pages));
        if (opened_status != MYOS_STATUS_OK) {
            /* TaskSpace::open may retain a strong-closeable partial
             * aggregate when a later child operation fails.  Transfer that
             * exact owner through the same table ClosingRecord path used by
             * every post-acquisition construction failure; only a truly
             * resource-free rejection may return directly. */
            if (record.has_resources()
                && !fail(CloseReason::ConstructionFailure, opened_status)) {
                record_type::ownership_fault(opened_status);
            }
            return opened_status;
        }
        bool opened = true;
        const auto failure = [&](myos_status_t status) noexcept
            -> myos_status_t {
            /* construction diagnostics are intentionally fail-stop-only in
             * production; host callers observe the returned status. */
            if (!opened) {
                return status;
            }
            if (!fail(CloseReason::ConstructionFailure, status)) {
                record_type::ownership_fault(status);
            }
            opened = false;
            return status;
        };

        auto& projections = record.mutable_projections();
        projections.vspace = SlotProjection{
            .projection = ProjectionKind::Local,
            .local = record.space().vspace_slot(),
            .kind = MYOS_OBJECT_KIND_VSPACE};
        projections.cspace = SlotProjection{
            .projection = ProjectionKind::Local,
            .local = record.space().manager_slot(),
            .kind = MYOS_OBJECT_KIND_CSPACE};

        const auto pool = record.space().pool();
        const auto vspace = record.resolve_internal(
            projections.vspace, MYOS_OBJECT_KIND_VSPACE);
        const auto cspace = record.resolve_internal(
            projections.cspace, MYOS_OBJECT_KIND_CSPACE);
        if (!pool || !vspace || !cspace) {
            return failure(MYOS_STATUS_INVALID_CAP);
        }

        using Materializer = ImageMaterializer<
            space_type::local_capacity(),
            space_type::remote_capacity(),
            backend_type,
            32,
            1>;
        using Image = typename Materializer::Image;
        Materializer materializer{
            record.space(), *input.bundle, *input.scratch};
        Image& image = workspace.image;
        uintptr_t (&image_entries)[MYOS_DEPLOY_TASK_IMAGE_MAX] =
            workspace.image_entries;
        LocalSlot (&mapping_regions)[MYOS_DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_regions;
        myos_word_t (&mapping_addresses)[MYOS_DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_addresses;
        myos_word_t (&mapping_sizes)[MYOS_DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_sizes;
        myos_word_t (&mapping_access)[MYOS_DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_access;
        bool (&mapping_done)[MYOS_DEPLOY_TASK_MAPPING_MAX] =
            workspace.mapping_done;

        if (typed_imports) {
            const myos_status_t status = materializer.materialize_descriptor(
                &workspace.import_descriptor_bytes[0][0],
                sizeof(workspace.import_descriptor_bytes),
                workspace.import_descriptor);
            if (status != MYOS_STATUS_OK) {
                return failure(status);
            }
        }

        const auto local_projection = [](LocalSlot slot)
            noexcept -> SlotProjection {
            return SlotProjection{
                .projection = ProjectionKind::Local,
                .local = slot,
                .kind = slot.kind};
        };
        const auto close_owner = [&](owner_type& owner) noexcept
            -> myos_status_t {
            if (!owner) {
                return MYOS_STATUS_OK;
            }
            const myos_status_t status = owner.close();
            if (status != MYOS_STATUS_OK) {
                backend_type::ownership_fault(status);
            }
            return status;
        };
        const auto adopt_result = [&](SysResult result,
                                      myos_object_kind_t kind,
                                      LocalSlot& output) noexcept
            -> myos_status_t {
            output = {};
            if (result.value == 0) {
                return result.status == MYOS_STATUS_OK
                    ? MYOS_STATUS_INVALID_CAP : result.status;
            }
            owner_type owner{cap::CapRef{result.value, 0}};
            if (result.status != MYOS_STATUS_OK) {
                static_cast<void>(close_owner(owner));
                return result.status;
            }
            const auto slot = record.space().adopt_local(
                libk::move(owner), kind);
            if (!slot) {
                static_cast<void>(close_owner(owner));
                return MYOS_STATUS_NO_MEMORY;
            }
            output = *slot;
            return MYOS_STATUS_OK;
        };

        /* Pre-mapping local objects have no references to mappings. */
        LocalSlot relation_notification{};
        myos_word_t relation_badge{};
        const auto role_source = [&](uint32_t role,
                                     ByteView& source) noexcept -> bool {
            source = {};
            size_t role_count = 0;
            for (uint32_t index = 0; index < row->bootstraps.count; ++index) {
                const PlanBootstrap* bootstrap_row = task.bootstrap(index);
                if (bootstrap_row == nullptr || bootstrap_row->kind != role) {
                    continue;
                }
                ++role_count;
                const ByteView destination =
                    task.symbol(bootstrap_row->destination);
                size_t matches = 0;
                for (uint32_t import_index = 0;
                     import_index < row->imports.count; ++import_index) {
                    const PlanImport* import = task.import(import_index);
                    if (import == nullptr
                        || import->source_class
                            != MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY
                        || !task.symbol(import->destination).equals(destination)) {
                        continue;
                    }
                    source = task.symbol(import->source);
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
        if (!role_source(MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION,
                         service_key)
            || !role_source(MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION,
                            readiness_key)) {
            return failure(MYOS_STATUS_BAD_ARGS);
        }
        for (uint32_t index = 0; index < row->objects.count; ++index) {
            const PlanObject* const object = task.object(index);
            if (object == nullptr) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            if (object->kind == MYOS_OBJECT_KIND_ENDPOINT
                || (object->flags & MYOS_DEPLOY_OBJECT_POST_MAPPING) != 0) {
                continue;
            }
            LocalSlot slot{};
            myos_status_t status = MYOS_STATUS_BAD_ARGS;
            switch (object->kind) {
            case MYOS_OBJECT_KIND_NOTIFICATION: {
                status = adopt_result(
                    backend_type::notification_create(
                        pool.value(), object->args[0]),
                    object->kind, slot);
                break;
            }
            case MYOS_OBJECT_KIND_CHANNEL: {
                const SysResult created = backend_type::channel_create(
                    pool.value(), object->args[0], object->args[1],
                    object->args[2], object->args[3]);
                owner_type first{};
                owner_type second{};
                if (created.value != 0) {
                    first = owner_type{cap::CapRef{created.value, 0}};
                }
                if (created.value2 != 0) {
                    second = owner_type{cap::CapRef{created.value2, 0}};
                }
                if (created.status != MYOS_STATUS_OK
                    || !first || !second) {
                    static_cast<void>(close_owner(first));
                    static_cast<void>(close_owner(second));
                    status = created.status == MYOS_STATUS_OK
                        ? MYOS_STATUS_INVALID_CAP : created.status;
                    break;
                }
                const auto first_slot = record.space().adopt_local(
                    libk::move(first), object->kind);
                if (!first_slot) {
                    static_cast<void>(close_owner(first));
                    static_cast<void>(close_owner(second));
                    status = MYOS_STATUS_NO_MEMORY;
                    break;
                }
                const auto second_slot = record.space().adopt_local(
                    libk::move(second), object->kind);
                if (!second_slot) {
                    static_cast<void>(close_owner(second));
                    status = MYOS_STATUS_NO_MEMORY;
                    break;
                }
                projections.objects[index] = local_projection(*first_slot);
                projections.object_b[index] = local_projection(*second_slot);
                status = MYOS_STATUS_OK;
                break;
            }
            case MYOS_OBJECT_KIND_PAGER:
                status = adopt_result(
                    backend_type::pager_create(
                        pool.value(), object->args[0], object->args[1]),
                    object->kind, slot);
                break;
            default:
                status = MYOS_STATUS_BAD_ARGS;
                break;
            }
            if (status != MYOS_STATUS_OK) {
                return failure(status);
            }
            if (object->kind != MYOS_OBJECT_KIND_CHANNEL) {
                projections.objects[index] = local_projection(slot);
            }
        }

        /* The execution terminal relation is the one Notification not named
         * by a service/readiness bootstrap role.  This derives the relation
         * from the manifest graph rather than from object-row order. */
        for (uint32_t index = 0; index < row->objects.count; ++index) {
            const PlanObject* object = task.object(index);
            if (object == nullptr || object->kind != MYOS_OBJECT_KIND_NOTIFICATION
                || !projections.objects[index].valid()) {
                continue;
            }
            const ByteView output = task.symbol(object->output);
            if ((service_key.size() != 0 && output.equals(service_key))
                || (readiness_key.size() != 0 && output.equals(readiness_key))) {
                continue;
            }
            if (relation_notification.valid()) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            relation_notification = projections.objects[index].local;
            relation_badge = object->args[0];
        }
        if (row->executions.count != 0 && !relation_notification.valid()) {
            return failure(MYOS_STATUS_BAD_ARGS);
        }

        /* Resolve and install every mapping exactly once. */
        for (uint32_t image_index = 0; image_index < row->images.count;
             ++image_index) {
            const PlanImage* const image_row = task.image(image_index);
            if (image_row == nullptr) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            const ByteView name = task.symbol(image_row->source);
            const myos_status_t materialized_status =
                materializer.materialize(name, image);
            if (materialized_status != MYOS_STATUS_OK) {
                return failure(materialized_status);
            }
            image_entries[image_index] = image.entry;
            for (size_t segment = 0; segment < image.segments.size();
                 ++segment) {
                size_t matches = 0;
                for (uint32_t mapping_index = 0;
                     mapping_index < row->mappings.count; ++mapping_index) {
                    const PlanMapping* const mapping =
                        task.mapping(mapping_index);
                    if (mapping == nullptr
                        || mapping->source
                            != MYOS_DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
                        || mapping->image
                            != row->images.first + image_index
                        || mapping->segment != segment) {
                        continue;
                    }
                    ++matches;
                    if (matches != 1 || mapping_done[mapping_index]) {
                        return failure(MYOS_STATUS_BAD_ARGS);
                    }
                    projections.mappings[mapping_index] = local_projection(
                        image.segments[segment].memory);
                    mapping_regions[mapping_index] =
                        image.segments[segment].region;
                    mapping_addresses[mapping_index] =
                        static_cast<myos_word_t>(
                            image.segments[segment].address);
                    mapping_sizes[mapping_index] = image.segments[segment].size;
                    mapping_access[mapping_index] =
                        image.segments[segment].access;
                    mapping_done[mapping_index] = true;
                }
                if (matches != 1) {
                    return failure(MYOS_STATUS_BAD_ARGS);
                }
            }
            image.clear();
        }

        for (uint32_t mapping_index = 0;
             mapping_index < row->mappings.count; ++mapping_index) {
            if (mapping_done[mapping_index]) {
                continue;
            }
            const PlanMapping* const mapping = task.mapping(mapping_index);
            if (mapping == nullptr) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            typename Image::Mapping materialized{};
            myos_status_t status = MYOS_STATUS_BAD_ARGS;
            if (mapping_index == bootstrap_mapping) {
                if ((input.bootstrap == nullptr && input.bootstrap_size != 0)
                    || input.bootstrap_size > mapping->size) {
                    return failure(MYOS_STATUS_BAD_ARGS);
                }
                status = materializer.materialize_zero(
                    static_cast<myos_word_t>(mapping->address),
                    static_cast<myos_word_t>(mapping->size),
                    MYOS_VM_READ,
                    materialized);
                if (status == MYOS_STATUS_OK && input.bootstrap != nullptr
                    && input.bootstrap_size != 0) {
                    status = materializer.write(
                        materialized.memory,
                        static_cast<myos_word_t>(mapping->size),
                        0,
                        input.bootstrap,
                        input.bootstrap_size);
                }
                /* A generated production envelope is populated after Imports,
                 * so retain this writable MemoryObject until that point.  A
                 * caller-supplied legacy snapshot remains closed here. */
                if (status == MYOS_STATUS_OK && row->bootstraps.count == 0) {
                    status = record.space().close_slot(materialized.memory);
                    materialized.memory = {};
                } else if (status == MYOS_STATUS_OK) {
                    workspace.bootstrap_memory = materialized.memory;
                }
            } else if (mapping->source == MYOS_DEPLOY_MAPPING_SOURCE_ZERO) {
                status = materializer.materialize_zero(
                    static_cast<myos_word_t>(mapping->address),
                    static_cast<myos_word_t>(mapping->size),
                    static_cast<myos_word_t>(mapping->access),
                    materialized);
            } else if (mapping->source == MYOS_DEPLOY_MAPPING_SOURCE_PAGER) {
                if (!workspace.pager_leases[mapping_index]
                    || !workspace.pager_leases[mapping_index]->valid()) {
                    return failure(MYOS_STATUS_INVALID_CAP);
                }
                status = materializer.materialize_paged(
                    workspace.pager_leases[mapping_index]->source(),
                    static_cast<myos_word_t>(mapping->address),
                    static_cast<myos_word_t>(mapping->size),
                    static_cast<myos_word_t>(mapping->access),
                    materialized);
            }
            if (status != MYOS_STATUS_OK || !materialized.region.valid()) {
                return failure(status == MYOS_STATUS_OK
                    ? MYOS_STATUS_INVALID_CAP : status);
            }
            projections.mappings[mapping_index] = materialized.memory.valid()
                ? local_projection(materialized.memory)
                : local_projection(materialized.region);
            mapping_regions[mapping_index] = materialized.region;
            mapping_addresses[mapping_index] = materialized.address;
            mapping_sizes[mapping_index] = materialized.size;
            mapping_access[mapping_index] = materialized.access;
            mapping_done[mapping_index] = true;
            if (mapping_index == bootstrap_mapping) {
                projections.bootstrap = projections.mappings[mapping_index];
            }
        }
        for (uint32_t mapping_index = 0;
             mapping_index < row->mappings.count; ++mapping_index) {
            if (!mapping_done[mapping_index]) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
        }

        ResidentAccounting accounting{};
        for (uint32_t mapping_index = 0;
             mapping_index < row->mappings.count; ++mapping_index) {
            const PlanMapping* const mapping = task.mapping(mapping_index);
            if (mapping == nullptr || mapping->critical
                    > MYOS_DEPLOY_CRITICAL_IPC_HEADER) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            if (mapping->critical == MYOS_DEPLOY_CRITICAL_NONE) {
                continue;
            }
            const auto total = libk::checked_add(
                accounting.total_bytes,
                static_cast<uint64_t>(mapping_sizes[mapping_index]));
            const auto class_total = libk::checked_add(
                accounting.by_class[mapping->critical],
                static_cast<uint64_t>(mapping_sizes[mapping_index]));
            if (!total || !class_total) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            accounting.total_bytes = *total;
            accounting.by_class[mapping->critical] = *class_total;
            const SlotProjection& projection =
                projections.mappings[mapping_index];
            if (projection.projection == ProjectionKind::Local
                && projection.local.kind == MYOS_OBJECT_KIND_MEMORY) {
                for (uint32_t prior = 0; prior < mapping_index; ++prior) {
                    const SlotProjection& previous =
                        projections.mappings[prior];
                    if (previous.projection == ProjectionKind::Local
                        && previous.local.kind == MYOS_OBJECT_KIND_MEMORY
                        && previous.local.pool == projection.local.pool
                        && previous.local.index == projection.local.index) {
                        return failure(MYOS_STATUS_BAD_ARGS);
                    }
                }
            }
        }
        if (accounting.total_bytes > row->critical_bytes) {
            return failure(MYOS_STATUS_NO_MEMORY);
        }
        record.mutable_accounting() = accounting;

        /* Post-mapping Endpoint descriptors are snapshots into a zero mapping
         * and are constructed only after all image/stack mappings exist. */
        for (uint32_t object_index = 0; object_index < row->objects.count;
             ++object_index) {
            const PlanObject* const object = task.object(object_index);
            if (object == nullptr || object->kind != MYOS_OBJECT_KIND_ENDPOINT) {
                continue;
            }
            const uint32_t descriptor_mapping = mapping_local(object->refs[0]);
            if (descriptor_mapping == MYOS_DEPLOY_NO_INDEX) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            const PlanMapping* const descriptor_row =
                task.mapping(descriptor_mapping);
            const auto descriptor_memory = record.resolve_internal(
                projections.mappings[descriptor_mapping],
                MYOS_OBJECT_KIND_MEMORY);
            if (descriptor_row == nullptr || !descriptor_memory
                || object->args[0] > descriptor_row->size
                || sizeof(myos_endpoint_desc)
                    > descriptor_row->size - object->args[0]
                || row->executions.count != 1) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            const PlanExecution* const execution = task.execution(0);
            if (execution == nullptr) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            if (execution->image < row->images.first
                || execution->image >= row->images.first + row->images.count) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            const size_t execution_image_index =
                execution->image - row->images.first;
            const uintptr_t execution_entry = execution->entry != 0
                ? static_cast<uintptr_t>(execution->entry)
                : image_entries[execution_image_index];
            const auto code_mapping = [&]() noexcept
                -> uint32_t {
                for (uint32_t index = 0; index < row->mappings.count;
                     ++index) {
                    const PlanMapping* candidate = task.mapping(index);
                    if (candidate != nullptr
                        && candidate->source
                            == MYOS_DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT
                        && candidate->image == execution->image
                        && (mapping_access[index] & MYOS_VM_EXECUTE) != 0
                        && execution_entry >= mapping_addresses[index]
                        && execution_entry - mapping_addresses[index]
                            < mapping_sizes[index]) {
                        return index;
                    }
                }
                return MYOS_DEPLOY_NO_INDEX;
            }();
            const uint32_t stack_mapping_index =
                mapping_local(execution->stack);
            if (stack_mapping_index == MYOS_DEPLOY_NO_INDEX) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            const auto stack_ref = record.resolve_internal(
                projections.mappings[stack_mapping_index],
                MYOS_OBJECT_KIND_MEMORY);
            if (code_mapping == MYOS_DEPLOY_NO_INDEX || !stack_ref) {
                return failure(MYOS_STATUS_INVALID_CAP);
            }
            const auto code_ref = record.resolve_internal(
                projections.mappings[code_mapping],
                MYOS_OBJECT_KIND_MEMORY);
            const PlanMapping* const stack_row =
                task.mapping(stack_mapping_index);
            if (!code_ref || stack_row == nullptr
                || mapping_sizes[code_mapping] == 0) {
                return failure(MYOS_STATUS_INVALID_CAP);
            }
            myos_endpoint_desc descriptor{};
            descriptor.version = MYOS_ENDPOINT_VERSION;
            descriptor.flags = MYOS_ENDPOINT_FLAGS_NONE;
            descriptor.entry = execution_entry;
            descriptor.code_memory = code_ref->selector;
            descriptor.code_page = 0;
            descriptor.code_address = mapping_addresses[code_mapping];
            descriptor.code_pages = mapping_sizes[code_mapping]
                / MYOS_DEPLOY_PAGE_SIZE;
            descriptor.stack_memory = stack_ref->selector;
            descriptor.stack_page = 0;
            descriptor.stack_address = mapping_addresses[stack_mapping_index];
            descriptor.stack_pages = mapping_sizes[stack_mapping_index]
                / MYOS_DEPLOY_PAGE_SIZE;
            descriptor.stack_stride = mapping_sizes[stack_mapping_index];
            descriptor.activation_count = 1;
            descriptor.queue_capacity = 1;
            descriptor.max_depth = 1;
            descriptor.budget_floor_ns = 1;
            descriptor.urgency_ceiling = execution->urgency;
            if (execution->ipc != MYOS_DEPLOY_NO_INDEX) {
                const uint32_t ipc_mapping_index =
                    mapping_local(execution->ipc);
                if (ipc_mapping_index == MYOS_DEPLOY_NO_INDEX) {
                    return failure(MYOS_STATUS_BAD_ARGS);
                }
                const PlanMapping* const ipc_row =
                    task.mapping(ipc_mapping_index);
                const auto ipc_ref = record.resolve_internal(
                    projections.mappings[ipc_mapping_index],
                    MYOS_OBJECT_KIND_MEMORY);
                if (ipc_row == nullptr || !ipc_ref) {
                    return failure(MYOS_STATUS_INVALID_CAP);
                }
                descriptor.ipc.memory = ipc_ref->selector;
                descriptor.ipc.address = mapping_addresses[ipc_mapping_index];
                descriptor.ipc.pages = mapping_sizes[ipc_mapping_index]
                    / MYOS_DEPLOY_PAGE_SIZE;
                descriptor.ipc_stride = mapping_sizes[ipc_mapping_index];
            }
            myos_status_t status = materializer.write(
                projections.mappings[descriptor_mapping].local,
                static_cast<myos_word_t>(descriptor_row->size),
                static_cast<myos_word_t>(object->args[0]),
                &descriptor,
                sizeof(descriptor));
            if (status != MYOS_STATUS_OK) {
                return failure(status);
            }
            LocalSlot endpoint{};
            status = adopt_result(
                backend_type::endpoint_create(
                    pool.value(), vspace.value(), cspace.value(),
                    descriptor_memory.value(),
                    static_cast<myos_word_t>(object->args[0])),
                MYOS_OBJECT_KIND_ENDPOINT,
                endpoint);
            if (status != MYOS_STATUS_OK) {
                return failure(status);
            }
            projections.objects[object_index] = local_projection(endpoint);
        }

        /* Imports are bounded transactions; earlier successful batches remain
         * in the same unpublished TaskSpace and are reclaimed by failure.  A
         * typed row consumes its canonical descriptor bytes from the one
         * TaskSpace-owned carrier at its batch-local offset.  The phase is
         * declared here but invoked after every constructible TaskKey source
         * (including executions and scheduling contexts) exists. */
        const auto import_sources = [&]() noexcept -> myos_status_t {
            uint32_t imported = 0;
            while (imported < row->imports.count) {
            const uint32_t count = row->imports.count - imported
                > kImportBatchMax
                ? static_cast<uint32_t>(kImportBatchMax)
                : row->imports.count - imported;
            ImportProjection* const outputs = workspace.imports;
            for (size_t index = 0; index < kImportBatchMax; ++index) {
                outputs[index] = {};
                workspace.import_bindings[index] = {};
                for (uint8_t& byte : workspace.import_descriptor_bytes[index]) {
                    byte = 0;
                }
            }
                for (uint32_t index = 0; index < count; ++index) {
                    const PlanImport* const import = task.import(imported + index);
                    if (import == nullptr) {
                        return failure(MYOS_STATUS_BAD_ARGS);
                    }
                    attenuation::encode_wire(
                        import->attenuation,
                        workspace.import_descriptor_bytes[index]);
                    ImportBinding& binding = workspace.import_bindings[index];
                    binding.authority = input.bindings->imports[imported + index];
                    if (import->source_class == MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY) {
                        const ByteView source_key = task.symbol(import->source);
                        libk::optional<cap::CapRef> source{};
                        const auto matches = [source_key](ByteView candidate)
                            noexcept -> bool {
                            return source_key.size() != 0
                                && candidate.equals(source_key);
                        };
                        if (matches(task.symbol(row->pool_key))) {
                            source = record.space().pool();
                        }
                        const auto consider = [&source, &record, matches](
                            ByteView key,
                            const SlotProjection& projection,
                            myos_object_kind_t kind) noexcept {
                            if (source.has_value() || !matches(key)
                                || projection.kind != kind) {
                                return;
                            }
                            source = record.resolve_internal(projection, kind);
                        };
                        consider(task.symbol(row->vspace_key),
                                 projections.vspace,
                                 MYOS_OBJECT_KIND_VSPACE);
                        consider(task.symbol(row->cspace_key),
                                 projections.cspace,
                                 MYOS_OBJECT_KIND_CSPACE);
                        for (uint32_t mapping = 0;
                             mapping < row->mappings.count && !source; ++mapping) {
                            const PlanMapping* mapping_row = task.mapping(mapping);
                            if (mapping_row == nullptr) {
                                return failure(MYOS_STATUS_BAD_ARGS);
                            }
                            consider(task.symbol(mapping_row->produced),
                                     projections.mappings[mapping],
                                     MYOS_OBJECT_KIND_MEMORY);
                            consider(task.symbol(mapping_row->region),
                                     local_projection(mapping_regions[mapping]),
                                     MYOS_OBJECT_KIND_VSPACE);
                        }
                        for (uint32_t object = 0;
                             object < row->objects.count && !source; ++object) {
                            const PlanObject* object_row = task.object(object);
                            if (object_row == nullptr) {
                                return failure(MYOS_STATUS_BAD_ARGS);
                            }
                            consider(task.symbol(object_row->output),
                                     projections.objects[object],
                                     object_row->kind);
                            consider(task.symbol(object_row->output_b),
                                     projections.object_b[object],
                                     object_row->kind);
                        }
                        for (uint32_t execution = 0;
                             execution < row->executions.count && !source;
                             ++execution) {
                            const PlanExecution* execution_row =
                                task.execution(execution);
                            if (execution_row == nullptr) {
                                return failure(MYOS_STATUS_BAD_ARGS);
                            }
                            consider(task.symbol(execution_row->key),
                                     projections.executions[execution],
                                     execution_row->model
                                         == MYOS_DEPLOY_EXECUTION_THREAD
                                     ? MYOS_OBJECT_KIND_THREAD
                                     : MYOS_OBJECT_KIND_VPROC);
                            consider(task.symbol(execution_row->sc),
                                     projections.scheduling_contexts[execution],
                                     MYOS_OBJECT_KIND_SCHED_CONTEXT);
                        }
                        if (!source || source->cspace != 0) {
                            return failure(MYOS_STATUS_BAD_ARGS);
                        }
                        binding.source = source.value();
                    }
                    if (import->mode == MYOS_DEPLOY_IMPORT_TYPED_DELEGATE) {
                        binding.descriptor = workspace.import_descriptor;
                        binding.descriptor_offset =
                            index * MYOS_CAP_ATTENUATION_SIZE;
                    }
                }
                if (typed_imports) {
                    const myos_status_t written = materializer.write(
                        workspace.import_descriptor,
                        MYOS_DEPLOY_PAGE_SIZE,
                        0,
                        &workspace.import_descriptor_bytes[0][0],
                        sizeof(workspace.import_descriptor_bytes));
                    if (written != MYOS_STATUS_OK) {
                        return failure(written);
                    }
                }
                const myos_status_t status = ImportTransaction<
                    space_type, Authorities, kImportBatchMax>::run(
                    record.space(), task, imported, count,
                        workspace.import_bindings, authorities, outputs);
                if (status != MYOS_STATUS_OK) {
                    return failure(status);
                }
                for (uint32_t index = 0; index < count; ++index) {
                    if (!outputs[index].valid()
                        || !record.install_import_projection(
                            imported + index, outputs[index])) {
                        return failure(MYOS_STATUS_INVALID_CAP);
                    }
                }
                imported += count;
            }

            if (workspace.import_descriptor.valid()) {
                const myos_status_t closed = record.space().close_slot(
                    workspace.import_descriptor);
                if (closed != MYOS_STATUS_OK) {
                    return failure(closed);
                }
                workspace.import_descriptor = {};
            }
            return MYOS_STATUS_OK;
        };

        /* Bootstrap rows are materialized only after every Import has been
         * adopted.  The envelope therefore contains selectors from the
         * admitted child projections, never source authorities or guessed
         * fixed slots.  The current production path deliberately supports one execution here;
         * extending this to multi-execution requires an explicit ABI for
         * per-execution bootstrap state.  Defer the phase until after target
         * construction so TaskKey sources are complete. */
        const auto generate_bootstrap = [&]() noexcept -> myos_status_t {
        uint32_t readiness_roles = 0;
        SlotProjection readiness_source{};
        SlotProjection service_source{};
        if (row->bootstraps.count != 0) {
            if (bootstrap_mapping == MYOS_DEPLOY_NO_INDEX
                || row->bootstraps.count > MYOS_BOOTSTRAP_MAX_CAPS
                || row->executions.count != 1
                || input.runtime_cpu_count == 0
                || !workspace.bootstrap_memory.valid()) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            const PlanExecution* const execution = task.execution(0);
            const uint32_t stack_mapping = execution == nullptr
                ? MYOS_DEPLOY_NO_INDEX : mapping_local(execution->stack);
            const uint32_t execution_bootstrap = execution == nullptr
                ? MYOS_DEPLOY_NO_INDEX : mapping_local(execution->bootstrap);
            if (execution == nullptr
                || stack_mapping == MYOS_DEPLOY_NO_INDEX
                || execution_bootstrap != bootstrap_mapping
                || mapping_sizes[bootstrap_mapping] < sizeof(myos_bootstrap_info)
                || !mapping_regions[bootstrap_mapping].valid()
                || mapping_addresses[stack_mapping] == 0
                || mapping_sizes[stack_mapping] == 0
                || execution->stack_top == 0
                || input.bundle->size() == 0) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }

            myos_bootstrap_info info{};
            info.magic = MYOS_BOOTSTRAP_MAGIC;
            info.major = MYOS_BOOTSTRAP_MAJOR;
            info.minor = MYOS_BOOTSTRAP_MINOR;
            info.size = sizeof(info);
            info.cap_count = row->bootstraps.count;
            info.cpu_count = input.runtime_cpu_count;
            info.stack_base = mapping_addresses[stack_mapping];
            info.stack_size = mapping_sizes[stack_mapping];
            info.boot_bundle_size = input.bundle->size();

            for (uint32_t bootstrap = 0;
                 bootstrap < row->bootstraps.count; ++bootstrap) {
                const PlanBootstrap* const bootstrap_row =
                    task.bootstrap(bootstrap);
                if (bootstrap_row == nullptr
                    || bootstrap_row->kind < MYOS_BOOTSTRAP_CAP_VSPACE
                    || bootstrap_row->kind > MYOS_BOOTSTRAP_CAP_STAGING_REGION) {
                    return failure(MYOS_STATUS_BAD_ARGS);
                }
                const myos_object_kind_t expected_kind =
                    myos_bootstrap_object_kind(bootstrap_row->kind);
                if (expected_kind == MYOS_OBJECT_KIND_INVALID) {
                    return failure(MYOS_STATUS_BAD_ARGS);
                }
                const ByteView destination =
                    task.symbol(bootstrap_row->destination);
                size_t import_index = 0;
                size_t matches = 0;
                for (uint32_t import = 0; import < row->imports.count;
                     ++import) {
                    const PlanImport* const import_row = task.import(import);
                    if (import_row != nullptr
                        && task.symbol(import_row->destination)
                               .equals(destination)) {
                        import_index = import;
                        ++matches;
                    }
                }
                if (destination.size() == 0 || matches != 1) {
                    return failure(MYOS_STATUS_BAD_ARGS);
                }
                const PlanImport* const import = task.import(import_index);
                const SlotProjection& projection =
                    projections.imports[import_index];
                if (import == nullptr || import->attenuation.kind != expected_kind
                    || projection.kind != expected_kind) {
                    return failure(MYOS_STATUS_BAD_ARGS);
                }
                if (bootstrap_row->kind
                        == MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION) {
                    if (row->readiness != MYOS_DEPLOY_READINESS_EXPLICIT
                        || ++readiness_roles != 1
                        || import->source_class
                            != MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY
                        || import->mode != MYOS_DEPLOY_IMPORT_DUPLICATE
                        || import->attenuation.rights
                            != MYOS_RIGHT_SIGNAL) {
                        return failure(MYOS_STATUS_BAD_ARGS);
                    }
                    const ByteView source_key = task.symbol(import->source);
                    if (source_key.size() == 0) {
                        return failure(MYOS_STATUS_BAD_ARGS);
                    }
                    size_t source_matches = 0;
                    for (uint32_t object_index = 0;
                         object_index < row->objects.count; ++object_index) {
                        const PlanObject* const object =
                            task.object(object_index);
                        if (object == nullptr
                            || object->kind != MYOS_OBJECT_KIND_NOTIFICATION) {
                            continue;
                        }
                        const auto consider = [&](ByteView key,
                                                  const SlotProjection& slot)
                            noexcept {
                            if (key.size() == 0 || !key.equals(source_key)) {
                                return;
                            }
                            ++source_matches;
                            readiness_source = slot;
                        };
                        consider(task.symbol(object->output),
                                 projections.objects[object_index]);
                        consider(task.symbol(object->output_b),
                                 projections.object_b[object_index]);
                    }
                    if (source_matches != 1 || !readiness_source.valid()
                        || readiness_source.kind
                            != MYOS_OBJECT_KIND_NOTIFICATION) {
                        return failure(MYOS_STATUS_INVALID_CAP);
                    }
                    const LocalSlot relation = relation_notification;
                    if (relation.valid()
                        && readiness_source.projection == ProjectionKind::Local
                        && readiness_source.local.pool == relation.pool
                        && readiness_source.local.index == relation.index
                        && readiness_source.local.kind == relation.kind) {
                        return failure(MYOS_STATUS_BAD_ARGS);
                    }
                    record.readiness_ = readiness_source;
                } else if (bootstrap_row->kind
                               == MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION) {
                    if (import->source_class
                            != MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY
                        || import->mode != MYOS_DEPLOY_IMPORT_DUPLICATE) {
                        return failure(MYOS_STATUS_BAD_ARGS);
                    }
                    const ByteView source_key = task.symbol(import->source);
                    if (source_key.size() == 0) {
                        return failure(MYOS_STATUS_BAD_ARGS);
                    }
                    size_t source_matches = 0;
                    for (uint32_t object_index = 0;
                         object_index < row->objects.count; ++object_index) {
                        const PlanObject* const object =
                            task.object(object_index);
                        if (object == nullptr
                            || object->kind != MYOS_OBJECT_KIND_NOTIFICATION) {
                            continue;
                        }
                        const auto consider = [&](ByteView key,
                                                  const SlotProjection& slot)
                            noexcept {
                            if (key.size() == 0 || !key.equals(source_key)) {
                                return;
                            }
                            ++source_matches;
                            service_source = slot;
                        };
                        consider(task.symbol(object->output),
                                 projections.objects[object_index]);
                        consider(task.symbol(object->output_b),
                                 projections.object_b[object_index]);
                    }
                    if (source_matches != 1 || !service_source.valid()
                        || service_source.kind
                            != MYOS_OBJECT_KIND_NOTIFICATION) {
                        return failure(MYOS_STATUS_INVALID_CAP);
                    }
                }
                const auto reference = record.resolve_internal(
                    projection, expected_kind);
                if (!projection.valid() || !reference
                    || reference->cspace == 0) {
                    return failure(MYOS_STATUS_INVALID_CAP);
                }
                info.caps[bootstrap] = myos_bootstrap_cap{
                    .kind = bootstrap_row->kind,
                    .flags = 0,
                    .handle = reference->selector};
            }

            myos_status_t status = materializer.write(
                workspace.bootstrap_memory,
                mapping_sizes[bootstrap_mapping],
                0,
                &info,
                sizeof(info));
            if (status == MYOS_STATUS_OK) {
                const auto memory = record.space().lookup(
                    workspace.bootstrap_memory, MYOS_OBJECT_KIND_MEMORY);
                status = memory
                    ? backend_type::memory_seal(memory.value())
                    : MYOS_STATUS_INVALID_CAP;
            }
            if (status == MYOS_STATUS_OK) {
                status = record.space().close_slot(workspace.bootstrap_memory);
            }
            if (status != MYOS_STATUS_OK) {
                return failure(status);
            }
            workspace.bootstrap_memory = {};
            projections.mappings[bootstrap_mapping] = local_projection(
                mapping_regions[bootstrap_mapping]);
            projections.bootstrap = projections.mappings[bootstrap_mapping];
        }
        if ((row->readiness == MYOS_DEPLOY_READINESS_EXPLICIT)
                != (readiness_roles == 1)
            || (row->readiness != MYOS_DEPLOY_READINESS_EXPLICIT
                && record.readiness_.valid())) {
            return failure(MYOS_STATUS_BAD_ARGS);
        }
        const auto same_local = [](const SlotProjection& left,
                                   const LocalSlot& right) noexcept {
            return left.projection == ProjectionKind::Local
                && left.local.valid() && right.valid()
                && left.local.pool == right.pool
                && left.local.index == right.index
                && left.local.kind == right.kind;
        };
        if (service_source.valid()
            && (same_local(service_source, relation_notification)
                || (readiness_source.valid()
                    && readiness_source.projection == ProjectionKind::Local
                    && readiness_source.local.pool == service_source.local.pool
                    && readiness_source.local.index == service_source.local.index
                    && readiness_source.local.kind == service_source.local.kind))) {
            return failure(MYOS_STATUS_BAD_ARGS);
        }
        return MYOS_STATUS_OK;
        };

        /* Executions and their SCs are created before imports.  A descriptor
         * MemoryObject is retained by TaskSpace until this unpublished task
         * either commits in Cut D or follows the strong-close path. */
        for (uint32_t index = 0; index < row->executions.count; ++index) {
            const PlanExecution* const execution = task.execution(index);
            if (execution == nullptr || !workspace.domain_leases[index]
                || !workspace.domain_leases[index]->valid()) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            const uint32_t stack_mapping_index = mapping_local(execution->stack);
            const uint32_t bootstrap_mapping_index =
                mapping_local(execution->bootstrap);
            if (execution->image < row->images.first
                || execution->image >= row->images.first + row->images.count
                || stack_mapping_index == MYOS_DEPLOY_NO_INDEX
                || bootstrap_mapping_index == MYOS_DEPLOY_NO_INDEX) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            const size_t image_index =
                execution->image - row->images.first;
            const PlanMapping* const stack_mapping =
                task.mapping(stack_mapping_index);
            const PlanMapping* const bootstrap_mapping =
                task.mapping(bootstrap_mapping_index);
            const auto stack = record.resolve_internal(
                projections.mappings[stack_mapping_index],
                MYOS_OBJECT_KIND_MEMORY);
            if (stack_mapping == nullptr || bootstrap_mapping == nullptr
                || !stack) {
                return failure(MYOS_STATUS_INVALID_CAP);
            }
            const auto entry = execution->entry != 0
                ? execution->entry : image_entries[image_index];
            if (entry == 0 || execution->stack_top == 0) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            LocalSlot descriptor_slot{};
            myos_status_t status = MYOS_STATUS_BAD_ARGS;
            if (execution->model == MYOS_DEPLOY_EXECUTION_THREAD) {
                myos_thread_start descriptor{};
                descriptor.version = MYOS_THREAD_START_VERSION;
                descriptor.flags = 0;
                descriptor.entry = entry;
                descriptor.stack = execution->stack_top;
                descriptor.arguments[0] = mapping_addresses[
                    bootstrap_mapping_index];
                descriptor.arguments[1] = mapping_sizes[
                    bootstrap_mapping_index];
                if (execution->ipc != MYOS_DEPLOY_NO_INDEX) {
                    const uint32_t ipc_mapping_index =
                        mapping_local(execution->ipc);
                    if (ipc_mapping_index == MYOS_DEPLOY_NO_INDEX) {
                        return failure(MYOS_STATUS_BAD_ARGS);
                    }
                    const PlanMapping* const ipc_mapping =
                        task.mapping(ipc_mapping_index);
                    const auto ipc = ipc_mapping == nullptr
                        ? libk::optional<cap::CapRef>{}
                        : record.resolve_internal(
                            projections.mappings[ipc_mapping_index],
                            MYOS_OBJECT_KIND_MEMORY);
                    if (ipc_mapping == nullptr || !ipc) {
                        return failure(MYOS_STATUS_INVALID_CAP);
                    }
                    descriptor.ipc.memory = ipc->selector;
                    descriptor.ipc.address = mapping_addresses[
                        ipc_mapping_index];
                    descriptor.ipc.pages = mapping_sizes[ipc_mapping_index]
                        / MYOS_DEPLOY_PAGE_SIZE;
                }
                status = materializer.materialize_descriptor(
                    &descriptor, sizeof(descriptor), descriptor_slot);
                if (status == MYOS_STATUS_OK) {
                    const auto descriptor_ref = record.space().lookup(
                        descriptor_slot, MYOS_OBJECT_KIND_MEMORY);
                    status = descriptor_ref
                        ? adopt_result(
                              backend_type::thread_create(
                                  pool.value(), vspace.value(), cspace.value(),
                                  descriptor_ref.value(), 0),
                              MYOS_OBJECT_KIND_THREAD,
                              projections.executions[index].local)
                        : MYOS_STATUS_INVALID_CAP;
                }
            } else if (execution->model == MYOS_DEPLOY_EXECUTION_VPROC) {
                if (execution->control == MYOS_DEPLOY_NO_INDEX
                    || execution->event == MYOS_DEPLOY_NO_INDEX) {
                    return failure(MYOS_STATUS_BAD_ARGS);
                }
                const uint32_t control_mapping_index =
                    mapping_local(execution->control);
                const uint32_t event_mapping_index =
                    mapping_local(execution->event);
                if (control_mapping_index == MYOS_DEPLOY_NO_INDEX
                    || event_mapping_index == MYOS_DEPLOY_NO_INDEX) {
                    return failure(MYOS_STATUS_BAD_ARGS);
                }
                const PlanMapping* const control_mapping =
                    task.mapping(control_mapping_index);
                const PlanMapping* const event_mapping =
                    task.mapping(event_mapping_index);
                const auto control = control_mapping == nullptr
                    ? libk::optional<cap::CapRef>{}
                    : record.resolve_internal(
                        projections.mappings[control_mapping_index],
                        MYOS_OBJECT_KIND_MEMORY);
                const auto event = event_mapping == nullptr
                    ? libk::optional<cap::CapRef>{}
                    : record.resolve_internal(
                        projections.mappings[event_mapping_index],
                        MYOS_OBJECT_KIND_MEMORY);
                if (!control || !event || control_mapping == nullptr
                    || event_mapping == nullptr) {
                    return failure(MYOS_STATUS_INVALID_CAP);
                }
                myos_vproc_start descriptor{};
                descriptor.version = MYOS_VPROC_START_VERSION;
                descriptor.flags = 0;
                descriptor.entry = entry;
                descriptor.stack = execution->stack_top;
                descriptor.arguments[0] = mapping_addresses[
                    bootstrap_mapping_index];
                descriptor.arguments[1] = mapping_sizes[
                    bootstrap_mapping_index];
                descriptor.control_memory = control->selector;
                descriptor.control_page = 0;
                descriptor.control_address = mapping_addresses[
                    control_mapping_index];
                descriptor.event_memory = event->selector;
                descriptor.event_page = 0;
                descriptor.event_address = mapping_addresses[
                    event_mapping_index];
                status = materializer.materialize_descriptor(
                    &descriptor, sizeof(descriptor), descriptor_slot);
                if (status == MYOS_STATUS_OK) {
                    const auto descriptor_ref = record.space().lookup(
                        descriptor_slot, MYOS_OBJECT_KIND_MEMORY);
                    status = descriptor_ref
                        ? adopt_result(
                              backend_type::vproc_create(
                                  pool.value(), vspace.value(), cspace.value(),
                                  descriptor_ref.value(), 0),
                              MYOS_OBJECT_KIND_VPROC,
                              projections.executions[index].local)
                        : MYOS_STATUS_INVALID_CAP;
                }
            } else {
                status = MYOS_STATUS_BAD_ARGS;
            }
            if (descriptor_slot.valid()) {
                const myos_status_t closed =
                    record.space().close_slot(descriptor_slot);
                if (status == MYOS_STATUS_OK && closed != MYOS_STATUS_OK) {
                    status = closed;
                }
            }
            if (status != MYOS_STATUS_OK
                || !projections.executions[index].local.valid()) {
                return failure(status == MYOS_STATUS_OK
                    ? MYOS_STATUS_INVALID_CAP : status);
            }
            projections.executions[index] = local_projection(
                projections.executions[index].local);
            const auto execution_ref = record.resolve_internal(
                projections.executions[index],
                static_cast<myos_object_kind_t>(
                    execution->model == MYOS_DEPLOY_EXECUTION_THREAD
                        ? MYOS_OBJECT_KIND_THREAD : MYOS_OBJECT_KIND_VPROC));
            const auto sc = adopt_result(
                backend_type::sc_create(
                    pool.value(), workspace.domain_leases[index]->source(),
                    static_cast<myos_word_t>(execution->sc_budget),
                    static_cast<myos_word_t>(execution->sc_period),
                    static_cast<myos_word_t>(execution->urgency),
                    execution->home_cpu == MYOS_DEPLOY_HOME_CPU_ANY
                        ? 0 : execution->home_cpu),
                MYOS_OBJECT_KIND_SCHED_CONTEXT,
                projections.scheduling_contexts[index].local);
            if (status != MYOS_STATUS_OK || !execution_ref
                || sc != MYOS_STATUS_OK) {
                return failure(status != MYOS_STATUS_OK ? status : sc);
            }
            projections.scheduling_contexts[index] = local_projection(
                projections.scheduling_contexts[index].local);
            const auto sc_ref = record.resolve_internal(
                projections.scheduling_contexts[index],
                MYOS_OBJECT_KIND_SCHED_CONTEXT);
            if (!sc_ref) {
                return failure(MYOS_STATUS_INVALID_CAP);
            }
            const myos_status_t sc_bind_status = backend_type::sc_bind(
                sc_ref.value(), execution_ref.value());
            if (sc_bind_status != MYOS_STATUS_OK) {
                return failure(sc_bind_status);
            }
            if (relation_notification.valid()) {
                const auto notification = record.resolve_internal(
                    local_projection(relation_notification),
                    MYOS_OBJECT_KIND_NOTIFICATION);
                if (!notification) {
                    return failure(MYOS_STATUS_INVALID_CAP);
                }
                const myos_status_t terminal_status =
                    backend_type::terminal_observe_bind(
                        execution_ref.value(), notification.value(),
                        relation_badge);
                if (terminal_status != MYOS_STATUS_OK) {
                    return failure(terminal_status);
                }
                if (index < MYOS_DEPLOY_TASK_DEPENDENCY_MAX) {
                    projections.relations[index] = local_projection(
                        relation_notification);
                }
            }
            for (const auto& lease : workspace.domain_leases) {
                if (lease && !lease->valid()) {
                    return failure(MYOS_STATUS_INVALID_CAP);
                }
            }
            for (const auto& lease : workspace.pager_leases) {
                if (lease && !lease->valid()) {
                    return failure(MYOS_STATUS_INVALID_CAP);
                }
            }
        }

        /* Every local source named by a TaskKey now exists in the caller's
         * current CSpace.  Imports adopt their destinations immediately, then
         * the generated bootstrap envelope records only those admitted child
         * selectors. */
        const myos_status_t import_status = import_sources();
        if (import_status != MYOS_STATUS_OK) {
            return import_status;
        }
        const myos_status_t bootstrap_status = generate_bootstrap();
        if (bootstrap_status != MYOS_STATUS_OK) {
            return bootstrap_status;
        }

        /* Endpoint descriptor mappings are snapshot sources.  Retire their
         * writable MemoryObject selectors only after all execution consumers
         * have taken their snapshots; a shared mapping therefore remains
         * usable through the final constructor use.  The mapped VSpace region
         * is the retained projection and object lifetime. */
        for (uint32_t object_index = 0; object_index < row->objects.count;
             ++object_index) {
            const PlanObject* const object = task.object(object_index);
            if (object == nullptr || object->kind != MYOS_OBJECT_KIND_ENDPOINT) {
                continue;
            }
            const uint32_t descriptor_mapping = mapping_local(object->refs[0]);
            if (descriptor_mapping == MYOS_DEPLOY_NO_INDEX) {
                return failure(MYOS_STATUS_BAD_ARGS);
            }
            SlotProjection& mapping_projection =
                projections.mappings[descriptor_mapping];
            if (mapping_projection.projection != ProjectionKind::Local
                || mapping_projection.local.kind
                    != MYOS_OBJECT_KIND_MEMORY) {
                continue;
            }
            const LocalSlot region = mapping_regions[descriptor_mapping];
            if (!region.valid() || region.kind != MYOS_OBJECT_KIND_VSPACE) {
                return failure(MYOS_STATUS_INVALID_CAP);
            }
            const myos_status_t status = record.space().close_slot(
                mapping_projection.local);
            if (status != MYOS_STATUS_OK) {
                return failure(status);
            }
            mapping_projection = local_projection(region);
        }

        if (!workspace.domain_leases[0] && row->executions.count != 0) {
            return failure(MYOS_STATUS_INVALID_CAP);
        }
        for (const auto& lease : workspace.domain_leases) {
            if (lease && !lease->valid()) {
                return failure(MYOS_STATUS_INVALID_CAP);
            }
        }
        for (const auto& lease : workspace.pager_leases) {
            if (lease && !lease->valid()) {
                return failure(MYOS_STATUS_INVALID_CAP);
            }
        }
        if (row->bootstrap_mapping != MYOS_DEPLOY_NO_INDEX
            && !projections.bootstrap.valid()) {
            return failure(MYOS_STATUS_INVALID_CAP);
        }
        if (!bind_exports(record, task)
            || !validate_prepared(record, task)) {
            return failure(MYOS_STATUS_INVALID_CAP);
        }
        return MYOS_STATUS_OK;
    }
    [[nodiscard]] auto take_receiver() noexcept
        -> libk::optional<receiver_type> {
        if (!receiver_) {
            return libk::nullopt;
        }
        auto result = libk::move(*receiver_);
        receiver_.reset();
        return result;
    }

    [[nodiscard]] auto commit_prepared() noexcept -> bool {
        if (!valid()) {
            return false;
        }
        record_type& record = reservation_->record();
        const TaskPlanView task = record.plan();
        if (!bind_exports(record, task) || !validate_prepared(record, task)) {
            const myos_status_t status = MYOS_STATUS_INVALID_CAP;
            if (!reservation_->fail(
                    CloseReason::ConstructionFailure, status)) {
                record_type::ownership_fault(status);
                return false;
            }
            reservation_.reset();
            table_ = nullptr;
            return false;
        }
        if (!reservation_->commit_prepared()) {
            const myos_status_t status = MYOS_STATUS_INTERNAL;
            if (!reservation_->fail(
                    CloseReason::ConstructionFailure, status)) {
                record_type::ownership_fault(status);
                return false;
            }
            reservation_.reset();
            table_ = nullptr;
            return false;
        }
        reservation_.reset();
        table_ = nullptr;
        return true;
    }

    [[nodiscard]] auto fail(
        CloseReason reason,
        myos_status_t status) noexcept -> bool {
        if (!valid() || !reservation_->fail(reason, status)) {
            return false;
        }
        reservation_.reset();
        table_ = nullptr;
        return true;
    }

    /* Cancel an unpublished, resource-free transaction after its receiver
     * has been detached.  A rejected exact cancel retains the Reservation so
     * the caller can detach/retry instead of losing the table owner. */
    [[nodiscard]] auto cancel() noexcept -> bool {
        if (!valid() || receiver_ || !reservation_->cancel()) {
            return false;
        }
        reservation_.reset();
        table_ = nullptr;
        return true;
    }

private:
    [[nodiscard]] static auto capacity_demand(
        const TaskPlanView& task,
        const PlanTask& row,
        size_t& local,
        size_t& remote,
        size_t& leases,
        bool& typed_imports) noexcept -> bool {
        if (row.mappings.count > MYOS_DEPLOY_TASK_MAPPING_MAX
            || row.objects.count > MYOS_DEPLOY_TASK_OBJECT_MAX
            || row.executions.count > MYOS_DEPLOY_TASK_EXECUTION_MAX
            || row.imports.count > MYOS_DEPLOY_TASK_IMPORT_MAX) {
            return false;
        }

        local = 1;
        const auto add_term = [](size_t& total, size_t count,
                                 size_t factor) noexcept -> bool {
            const auto term = libk::checked_multiply(count, factor);
            if (!term) {
                return false;
            }
            const auto sum = libk::checked_add(total, *term);
            if (!sum) {
                return false;
            }
            total = *sum;
            return true;
        };
        if (!add_term(local, row.mappings.count, 2)
            || !add_term(local, row.executions.count, 3)) {
            return false;
        }
        for (uint32_t index = 0; index < row.objects.count; ++index) {
            const PlanObject* object = task.object(index);
            if (object == nullptr) {
                return false;
            }
            const size_t outputs = object->kind == MYOS_OBJECT_KIND_CHANNEL
                ? 2U : 1U;
            const auto next = libk::checked_add(local, outputs);
            if (!next) {
                return false;
            }
            local = *next;
        }

        typed_imports = false;
        for (uint32_t index = 0; index < row.imports.count; ++index) {
            const PlanImport* const import = task.import(index);
            if (import == nullptr) {
                return false;
            }
            typed_imports = typed_imports
                || import->mode == MYOS_DEPLOY_IMPORT_TYPED_DELEGATE;
        }
        if (typed_imports) {
            const auto next = libk::checked_add(local, size_t{1});
            if (!next) {
                return false;
            }
            local = *next;
        }

        remote = row.imports.count;
        size_t pager_count = 0;
        for (uint32_t index = 0; index < row.mappings.count; ++index) {
            const PlanMapping* mapping = task.mapping(index);
            if (mapping == nullptr) {
                return false;
            }
            if (mapping->source == MYOS_DEPLOY_MAPPING_SOURCE_PAGER) {
                const auto next = libk::checked_add(pager_count, size_t{1});
                if (!next) {
                    return false;
                }
                pager_count = *next;
            }
        }
        leases = row.executions.count;
        const auto with_pagers = libk::checked_add(leases, pager_count);
        if (!with_pagers) {
            return false;
        }
        leases = *with_pagers;
        const size_t batch = row.imports.count < kImportBatchMax
            ? row.imports.count : kImportBatchMax;
        const auto with_batch = libk::checked_add(leases, batch);
        if (!with_batch) {
            return false;
        }
        leases = *with_batch;
        return true;
    }

    [[nodiscard]] static auto source_from_slot(
        const SlotProjection& projection) noexcept -> SourceProjection {
        if (projection.projection != ProjectionKind::Local
            || !projection.local.valid()
            || projection.kind != projection.local.kind) {
            return SourceProjection{};
        }
        return SourceProjection{
            .projection = SourceProjectionKind::Local,
            .local = projection.local,
            .kind = projection.local.kind};
    }

    [[nodiscard]] static auto bind_exports(
        record_type& record,
        const TaskPlanView& task) noexcept -> bool {
        const PlanTask* row = task.row();
        if (row == nullptr || row->exports.count > MYOS_DEPLOY_TASK_EXPORT_MAX) {
            return false;
        }
        TaskProjections& projections = record.mutable_projections();
        for (auto& projection : projections.exports) {
            projection = {};
        }

        const auto consider = [](ByteView source, ByteView key,
                                const SlotProjection& slot,
                                myos_object_kind_t declared_kind,
                                bool& found,
                                myos_object_kind_t& declared,
                                SourceProjection& result) noexcept -> bool {
            if (!key.equals(source)) {
                return true;
            }
            if (found) {
                return false;
            }
            found = true;
            declared = declared_kind;
            result = source_from_slot(slot);
            return true;
        };

        for (uint32_t index = 0; index < row->exports.count; ++index) {
            const PlanExport* export_row = task.export_record(index);
            if (export_row == nullptr
                || !valid_authority_ceiling(export_row->ceiling)) {
                return false;
            }
            if (export_row->source_class == MYOS_DEPLOY_EXPORT_RUNTIME_READY) {
                /* RuntimeReady is intentionally withheld from construction;
                 * the current production path binds it during the real publication transition. */
                continue;
            }
            if (export_row->source_class != MYOS_DEPLOY_EXPORT_PREPARED_KEY) {
                return false;
            }
            const ByteView source = task.symbol(export_row->source);
            if (source.size() == 0) {
                return false;
            }
            SourceProjection result{};
            bool found = false;
            myos_object_kind_t declared_kind = MYOS_OBJECT_KIND_INVALID;
            const ByteView pool_key = task.symbol(row->pool_key);
            if (pool_key.equals(source)) {
                found = true;
                declared_kind = MYOS_OBJECT_KIND_RESOURCE_POOL;
                result = SourceProjection{
                    .projection = SourceProjectionKind::Pool,
                    .kind = MYOS_OBJECT_KIND_RESOURCE_POOL};
            }
            const auto check_slot = [&](ByteView key,
                                        const SlotProjection& slot,
                                        myos_object_kind_t kind) noexcept {
                return consider(source, key, slot, kind, found,
                                declared_kind, result);
            };
            if (!check_slot(task.symbol(row->vspace_key),
                            projections.vspace, MYOS_OBJECT_KIND_VSPACE)
                || !check_slot(task.symbol(row->cspace_key),
                               projections.cspace, MYOS_OBJECT_KIND_CSPACE)) {
                return false;
            }
            for (uint32_t mapping = 0; mapping < row->mappings.count;
                 ++mapping) {
                const PlanMapping* mapping_row = task.mapping(mapping);
                if (mapping_row == nullptr
                    || !check_slot(task.symbol(mapping_row->produced),
                                   projections.mappings[mapping],
                                   MYOS_OBJECT_KIND_MEMORY)) {
                    return false;
                }
            }
            for (uint32_t object = 0; object < row->objects.count; ++object) {
                const PlanObject* object_row = task.object(object);
                if (object_row == nullptr
                    || !check_slot(task.symbol(object_row->output),
                                   projections.objects[object],
                                   object_row->kind)
                    || !check_slot(task.symbol(object_row->output_b),
                                   projections.object_b[object],
                                   object_row->kind)) {
                    return false;
                }
            }
            for (uint32_t execution = 0;
                 execution < row->executions.count; ++execution) {
                const PlanExecution* execution_row = task.execution(execution);
                if (execution_row == nullptr
                    || !check_slot(task.symbol(execution_row->key),
                                   projections.executions[execution],
                                   execution_row->model
                                           == MYOS_DEPLOY_EXECUTION_THREAD
                                       ? MYOS_OBJECT_KIND_THREAD
                                       : MYOS_OBJECT_KIND_VPROC)
                    || !check_slot(task.symbol(execution_row->sc),
                                   projections.scheduling_contexts[execution],
                                   MYOS_OBJECT_KIND_SCHED_CONTEXT)) {
                    return false;
                }
            }
            if (!found || !result.valid()
                || declared_kind != export_row->ceiling.kind
                || result.kind != export_row->ceiling.kind) {
                return false;
            }
            projections.exports[index] = result;
        }
        return true;
    }

    [[nodiscard]] static auto validate_prepared(
        record_type& record,
        const TaskPlanView& task) noexcept -> bool {
        const PlanTask* row = task.row();
        if (row == nullptr || record.state() != TaskState::Constructing
            || !record.id().valid() || !record.plan_lease().valid()
            || !task.valid() || record.space().phase() != Phase::Open) {
            return false;
        }
        size_t local_demand{};
        size_t remote_demand{};
        size_t lease_demand{};
        bool typed_imports{};
        if (!capacity_demand(task, *row, local_demand, remote_demand,
                             lease_demand, typed_imports)
            || record.space().local_cumulative() > local_demand
            || record.space().local_cumulative()
                > space_type::local_capacity()
            || record.space().remote_size() > remote_demand
            || record.space().remote_size() > space_type::remote_capacity()) {
            return false;
        }
        static_cast<void>(typed_imports);
        const TaskProjections& projections = record.projections();
        if (!projections.vspace.valid() || !projections.cspace.valid()
            || !record.resolve_internal(projections.vspace, MYOS_OBJECT_KIND_VSPACE)
            || !record.resolve_internal(projections.cspace, MYOS_OBJECT_KIND_CSPACE)) {
            return false;
        }
        if (row->readiness == MYOS_DEPLOY_READINESS_EXPLICIT) {
            if (!record.readiness_.valid()
                || record.readiness_.projection != ProjectionKind::Local
                || record.readiness_.kind
                    != MYOS_OBJECT_KIND_NOTIFICATION
                || !record.resolve_readiness()) {
                return false;
            }
        } else if (record.readiness_.valid()) {
            return false;
        }
        if (row->bootstrap_mapping != MYOS_DEPLOY_NO_INDEX) {
            if (row->bootstrap_mapping < row->mappings.first
                || row->bootstrap_mapping - row->mappings.first
                    >= row->mappings.count
                || !projections.bootstrap.valid()) {
                return false;
            }
            const size_t mapping = row->bootstrap_mapping
                - row->mappings.first;
            const SlotProjection& bootstrap = projections.mappings[mapping];
            if (bootstrap.projection != projections.bootstrap.projection
                || bootstrap.local.pool != projections.bootstrap.local.pool
                || bootstrap.local.index != projections.bootstrap.local.index
                || bootstrap.local.kind != projections.bootstrap.local.kind) {
                return false;
            }
        }
        for (uint32_t index = 0; index < row->mappings.count; ++index) {
            const SlotProjection& projection = projections.mappings[index];
            if (!projection.valid() || projection.projection != ProjectionKind::Local
                || !record.resolve_internal(projection, projection.local.kind)) {
                return false;
            }
        }
        for (uint32_t index = 0; index < row->objects.count; ++index) {
            const PlanObject* object = task.object(index);
            if (object == nullptr || !projections.objects[index].valid()
                || !record.resolve_internal(projections.objects[index], object->kind)) {
                return false;
            }
            if (object->kind == MYOS_OBJECT_KIND_CHANNEL
                && (!projections.object_b[index].valid()
                    || !record.resolve_internal(projections.object_b[index], object->kind))) {
                return false;
            }
        }
        for (uint32_t index = 0; index < row->imports.count; ++index) {
            const SlotProjection& projection = projections.imports[index];
            if (!projection.valid()
                || !record.resolve_internal(projection, projection.kind)) {
                return false;
            }
        }
        for (uint32_t index = 0; index < row->executions.count; ++index) {
            const PlanExecution* execution = task.execution(index);
            const myos_object_kind_t execution_kind = execution != nullptr
                && execution->model == MYOS_DEPLOY_EXECUTION_THREAD
                ? MYOS_OBJECT_KIND_THREAD : MYOS_OBJECT_KIND_VPROC;
            if (execution == nullptr || !projections.executions[index].valid()
                || !record.resolve_internal(projections.executions[index], execution_kind)
                || !projections.scheduling_contexts[index].valid()
                || !record.resolve_internal(projections.scheduling_contexts[index],
                                   MYOS_OBJECT_KIND_SCHED_CONTEXT)
                || !projections.relations[index].valid()
                || !record.resolve_internal(projections.relations[index],
                                   MYOS_OBJECT_KIND_NOTIFICATION)) {
                return false;
            }
        }
        if (record.accounting().total_bytes > row->critical_bytes) {
            return false;
        }
        for (uint32_t index = 0; index < row->exports.count; ++index) {
            const PlanExport* export_row = task.export_record(index);
            if (export_row == nullptr) {
                return false;
            }
            if (export_row->source_class == MYOS_DEPLOY_EXPORT_RUNTIME_READY) {
                if (projections.exports[index].valid()) {
                    return false;
                }
                continue;
            }
            if (export_row->source_class != MYOS_DEPLOY_EXPORT_PREPARED_KEY
                || !projections.exports[index].valid()
                || !record.resolve_source(projections.exports[index],
                                           export_row->ceiling.kind)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static auto imports_admissible(
        const TaskPlanView& task) noexcept -> bool {
        const PlanTask* row = task.row();
        if (row == nullptr) {
            return false;
        }
        for (uint32_t index = 0; index < row->imports.count; ++index) {
            const PlanImport* import = task.import(index);
            if (import == nullptr
                || import->mode >= MYOS_DEPLOY_IMPORT_MOVE) {
                return false;
            }
        }
        return true;
    }

    TaskBuilder(
        table_type* table,
        reservation_type&& reservation,
        receiver_type&& receiver) noexcept
        : table_(table), reservation_(libk::move(reservation)),
          receiver_(libk::move(receiver)) {}

    void abandon() noexcept {
        if (!reservation_ || !reservation_->valid()) {
            if (receiver_ && receiver_->valid()) {
                static_cast<void>(receiver_->detach());
            }
            table_ = nullptr;
            return;
        }
        if (reservation_->valid()
            && reservation_->record().has_resources()) {
            Table::record_type::ownership_fault(MYOS_STATUS_BUSY);
            return;
        }
        if (receiver_) {
            static_cast<void>(receiver_->detach());
            receiver_.reset();
        }
        if (!reservation_->cancel()) {
            Table::record_type::ownership_fault(MYOS_STATUS_BUSY);
            return;
        }
        reservation_.reset();
        table_ = nullptr;
    }

    table_type* table_{};
    libk::optional<reservation_type> reservation_{};
    libk::optional<receiver_type> receiver_{};
};

} // namespace myos::deploy
