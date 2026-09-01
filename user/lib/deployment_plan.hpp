#pragma once

/*
 * The manifest parser owns the wire contract.  This header is the next
 * lifetime boundary: it copies the bounded string table and decodes every
 * row into caller-provided storage before any TaskRecord can retain it.
 * DeploymentPlan therefore contains no wire view and performs no policy
 * validation of its own.  A plan is immutable after decode; PlanLease is the
 * checked lifetime token retained by prepared and closing task records.
 */

#include <stddef.h>
#include <stdint.h>

#include <libk/assert.hpp>
#include <libk/expected.hpp>
#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <uapi/capability.h>
#include <uapi/deploy.h>

#include <user/lib/deploy_manifest.hpp>

namespace myos::deploy {

struct PlanId final {
    uint32_t slot{};
    uint32_t generation{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return generation != 0;
    }

    constexpr auto operator==(const PlanId&) const noexcept -> bool = default;
};

struct SymbolId final {
    uint32_t offset{};
    uint32_t length{};

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return length == 0;
    }

    constexpr auto operator==(const SymbolId&) const noexcept -> bool = default;
};

struct PlanTaskId final {
    PlanId plan{};
    uint32_t index{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return plan.valid();
    }

    constexpr auto operator==(const PlanTaskId&) const noexcept -> bool = default;
};

struct PlanRange final {
    uint32_t first{};
    uint32_t count{};
};

struct PlanTask final {
    SymbolId name{};
    SymbolId pool_key{};
    SymbolId vspace_key{};
    SymbolId cspace_key{};
    PlanRange images{};
    PlanRange mappings{};
    PlanRange objects{};
    PlanRange executions{};
    PlanRange imports{};
    PlanRange dependencies{};
    PlanRange exports{};
    uint64_t pool_memory{};
    uint64_t pool_caps{};
    uint64_t kind_mask{};
    uint64_t critical_bytes{};
    uint32_t cspace_slots{};
    uint32_t cspace_pages{};
    uint32_t bootstrap_mapping{MYOS_DEPLOY_NO_INDEX};
    uint32_t flags{};
    uint16_t readiness{};
    uint16_t terminal{};
    uint16_t restart{};
    uint64_t readiness_value{};
    PlanRange bootstraps{};
};

struct PlanImage final {
    SymbolId source{};
    uint16_t source_kind{};
    uint16_t flags{};
};

struct PlanMapping final {
    SymbolId produced{};
    SymbolId pager{};
    SymbolId region{};
    uint32_t image{MYOS_DEPLOY_NO_INDEX};
    uint32_t segment{MYOS_DEPLOY_NO_INDEX};
    uint16_t source{};
    uint16_t residency{};
    uint16_t critical{};
    uint16_t flags{};
    uint32_t access{};
    uint32_t pager_policy{};
    uint64_t address{};
    uint64_t size{};
};

struct PlanObject final {
    SymbolId output{};
    SymbolId output_b{};
    uint16_t kind{};
    uint16_t flags{};
    uint32_t refs[4]{};
    uint64_t args[6]{};
};

struct PlanExecution final {
    SymbolId key{};
    SymbolId sc{};
    SymbolId domain{};
    uint32_t image{MYOS_DEPLOY_NO_INDEX};
    uint32_t stack{MYOS_DEPLOY_NO_INDEX};
    uint32_t bootstrap{MYOS_DEPLOY_NO_INDEX};
    uint32_t ipc{MYOS_DEPLOY_NO_INDEX};
    uint32_t control{MYOS_DEPLOY_NO_INDEX};
    uint32_t event{MYOS_DEPLOY_NO_INDEX};
    uint16_t model{};
    uint16_t flags{};
    uint16_t fault{};
    uint16_t terminal{};
    uint64_t entry{};
    uint64_t stack_top{};
    uint64_t sc_budget{};
    uint64_t sc_period{};
    uint32_t urgency{};
    uint32_t home_cpu{};
};

struct PlanImport final {
    SymbolId source{};
    SymbolId destination{};
    uint16_t mode{};
    uint16_t selector{};
    uint32_t flags{};
    myos_cap_attenuation attenuation{};
    uint16_t source_class{};
};

struct PlanBootstrap final {
    uint32_t kind{};
    SymbolId destination{};
};

struct PlanDependency final {
    uint32_t target{MYOS_DEPLOY_NO_INDEX};
    uint16_t kind{};
    uint16_t flags{};
    SymbolId relation{};
};

struct PlanExport final {
    SymbolId source{};
    SymbolId key{};
    uint16_t source_class{};
    uint16_t flags{};
    myos_cap_attenuation ceiling{};
};

class DeploymentPlan;
class PlanLease;
template<size_t Capacity = 1, uint32_t GenerationLimit = UINT32_MAX>
class PlanSet;

/*
 * This is deliberately an explicit decoded-storage object.  PlanSet owns
 * stable control blocks containing it; callers place the PlanSet in a
 * process-server arena or static storage, never on a bootstrap stack.
 */
class PlanStorage final {
public:
    PlanStorage() noexcept = default;
    PlanStorage(const PlanStorage&) = delete;
    auto operator=(const PlanStorage&) -> PlanStorage& = delete;

    [[nodiscard]] constexpr auto task_count() const noexcept -> uint32_t {
        return task_count_;
    }
    [[nodiscard]] constexpr auto image_count() const noexcept -> uint32_t {
        return image_count_;
    }
    [[nodiscard]] constexpr auto mapping_count() const noexcept -> uint32_t {
        return mapping_count_;
    }
    [[nodiscard]] constexpr auto object_count() const noexcept -> uint32_t {
        return object_count_;
    }
    [[nodiscard]] constexpr auto execution_count() const noexcept -> uint32_t {
        return execution_count_;
    }
    [[nodiscard]] constexpr auto import_count() const noexcept -> uint32_t {
        return import_count_;
    }
    [[nodiscard]] constexpr auto dependency_count() const noexcept -> uint32_t {
        return dependency_count_;
    }
    [[nodiscard]] constexpr auto export_count() const noexcept -> uint32_t {
        return export_count_;
    }
    [[nodiscard]] constexpr auto bootstrap_count() const noexcept -> uint32_t {
        return bootstrap_count_;
    }

    [[nodiscard]] auto task(uint32_t index) const noexcept -> const PlanTask* {
        return index < task_count_ ? &tasks_[index] : nullptr;
    }
    [[nodiscard]] auto image(uint32_t index) const noexcept -> const PlanImage* {
        return index < image_count_ ? &images_[index] : nullptr;
    }
    [[nodiscard]] auto mapping(uint32_t index) const noexcept
        -> const PlanMapping* {
        return index < mapping_count_ ? &mappings_[index] : nullptr;
    }
    [[nodiscard]] auto object(uint32_t index) const noexcept
        -> const PlanObject* {
        return index < object_count_ ? &objects_[index] : nullptr;
    }
    [[nodiscard]] auto execution(uint32_t index) const noexcept
        -> const PlanExecution* {
        return index < execution_count_ ? &executions_[index] : nullptr;
    }
    [[nodiscard]] auto import(uint32_t index) const noexcept
        -> const PlanImport* {
        return index < import_count_ ? &imports_[index] : nullptr;
    }
    [[nodiscard]] auto dependency(uint32_t index) const noexcept
        -> const PlanDependency* {
        return index < dependency_count_ ? &dependencies_[index] : nullptr;
    }
    [[nodiscard]] auto export_record(uint32_t index) const noexcept
        -> const PlanExport* {
        return index < export_count_ ? &exports_[index] : nullptr;
    }
    [[nodiscard]] auto bootstrap(uint32_t index) const noexcept
        -> const PlanBootstrap* {
        return index < bootstrap_count_ ? &bootstraps_[index] : nullptr;
    }

    [[nodiscard]] auto symbol(SymbolId symbol_id) const noexcept -> ByteView {
        if (symbol_id.offset > string_size_
            || symbol_id.length > string_size_ - symbol_id.offset) {
            return {};
        }
        return ByteView{strings_ + symbol_id.offset, symbol_id.length};
    }

private:
    friend class DeploymentPlan;
    template<size_t Capacity, uint32_t GenerationLimit>
    friend class PlanSet;

    void clear() noexcept {
        for (size_t index = 0; index < task_count_; ++index) {
            tasks_[index] = {};
        }
        for (size_t index = 0; index < image_count_; ++index) {
            images_[index] = {};
        }
        for (size_t index = 0; index < mapping_count_; ++index) {
            mappings_[index] = {};
        }
        for (size_t index = 0; index < object_count_; ++index) {
            objects_[index] = {};
        }
        for (size_t index = 0; index < execution_count_; ++index) {
            executions_[index] = {};
        }
        for (size_t index = 0; index < import_count_; ++index) {
            imports_[index] = {};
        }
        for (size_t index = 0; index < dependency_count_; ++index) {
            dependencies_[index] = {};
        }
        for (size_t index = 0; index < export_count_; ++index) {
            exports_[index] = {};
        }
        for (size_t index = 0; index < bootstrap_count_; ++index) {
            bootstraps_[index] = {};
        }
        for (size_t index = 0; index < string_size_; ++index) {
            strings_[index] = 0;
        }
        task_count_ = 0;
        image_count_ = 0;
        mapping_count_ = 0;
        object_count_ = 0;
        execution_count_ = 0;
        import_count_ = 0;
        dependency_count_ = 0;
        export_count_ = 0;
        bootstrap_count_ = 0;
        string_size_ = 0;
    }

    PlanTask tasks_[MYOS_DEPLOY_TASK_MAX]{};
    PlanImage images_[MYOS_DEPLOY_IMAGE_MAX]{};
    PlanMapping mappings_[MYOS_DEPLOY_MAPPING_MAX]{};
    PlanObject objects_[MYOS_DEPLOY_OBJECT_MAX]{};
    PlanExecution executions_[MYOS_DEPLOY_EXECUTION_MAX]{};
    PlanImport imports_[MYOS_DEPLOY_IMPORT_MAX]{};
    PlanDependency dependencies_[MYOS_DEPLOY_DEPENDENCY_MAX]{};
    PlanExport exports_[MYOS_DEPLOY_EXPORT_MAX]{};
    PlanBootstrap bootstraps_[MYOS_DEPLOY_BOOTSTRAP_MAX]{};
    uint8_t strings_[MYOS_DEPLOY_STRING_MAX]{};
    uint32_t task_count_{};
    uint32_t image_count_{};
    uint32_t mapping_count_{};
    uint32_t object_count_{};
    uint32_t execution_count_{};
    uint32_t import_count_{};
    uint32_t dependency_count_{};
    uint32_t export_count_{};
    uint32_t bootstrap_count_{};
    uint32_t string_size_{};
};

namespace plan_detail {

enum class SlotState : uint8_t {
    Vacant,
    Owned,
    Retiring,
    Retired,
};

struct PlanControl final {
    using Reclaim = void (*)(PlanControl*) noexcept;

    PlanStorage storage{};
    PlanId id{};
    SlotState state{SlotState::Vacant};
    uint32_t lease_count{};
    void* owner{};
    Reclaim reclaim{};
};

} // namespace plan_detail

struct TaskPlanView final {
    plan_detail::PlanControl* control{};
    PlanTaskId id{};

    [[nodiscard]] auto valid() const noexcept -> bool;
    [[nodiscard]] auto row() const noexcept -> const PlanTask*;
    [[nodiscard]] auto image(uint32_t index) const noexcept
        -> const PlanImage*;
    [[nodiscard]] auto mapping(uint32_t index) const noexcept
        -> const PlanMapping*;
    [[nodiscard]] auto object(uint32_t index) const noexcept
        -> const PlanObject*;
    [[nodiscard]] auto execution(uint32_t index) const noexcept
        -> const PlanExecution*;
    [[nodiscard]] auto import(uint32_t index) const noexcept
        -> const PlanImport*;
    [[nodiscard]] auto dependency(uint32_t index) const noexcept
        -> const PlanDependency*;
    [[nodiscard]] auto export_record(uint32_t index) const noexcept
        -> const PlanExport*;
    [[nodiscard]] auto bootstrap(uint32_t index) const noexcept
        -> const PlanBootstrap*;
    [[nodiscard]] auto symbol(SymbolId symbol) const noexcept -> ByteView;
};

enum class PlanError : uint8_t {
    InvalidInput,
    Capacity,
};

class DeploymentPlan final {
public:
    DeploymentPlan() noexcept = default;
    DeploymentPlan(const DeploymentPlan&) = delete;
    auto operator=(const DeploymentPlan&) -> DeploymentPlan& = delete;

    DeploymentPlan(DeploymentPlan&& other) noexcept
        : control_(other.control_) {
        /* PlanLease points at the stable registry control block. */
        other.control_ = nullptr;
    }

    auto operator=(DeploymentPlan&& other) noexcept -> DeploymentPlan& {
        if (this == &other) {
            return *this;
        }
        release_owner();
        control_ = other.control_;
        other.control_ = nullptr;
        return *this;
    }

    ~DeploymentPlan() noexcept { release_owner(); }

    template<size_t Capacity, uint32_t GenerationLimit>
    [[nodiscard]] static auto decode(
        const ManifestView& view,
        PlanSet<Capacity, GenerationLimit>& plans) noexcept
        -> libk::Expected<DeploymentPlan, PlanError>;

    [[nodiscard]] auto id() const noexcept -> PlanId {
        return control_ == nullptr ? PlanId{} : control_->id;
    }

    [[nodiscard]] constexpr auto task_count() const noexcept -> uint32_t {
        return control_ == nullptr ? 0 : control_->storage.task_count();
    }
    [[nodiscard]] constexpr auto image_count() const noexcept -> uint32_t {
        return control_ == nullptr ? 0 : control_->storage.image_count();
    }
    [[nodiscard]] constexpr auto mapping_count() const noexcept -> uint32_t {
        return control_ == nullptr ? 0 : control_->storage.mapping_count();
    }
    [[nodiscard]] constexpr auto object_count() const noexcept -> uint32_t {
        return control_ == nullptr ? 0 : control_->storage.object_count();
    }
    [[nodiscard]] constexpr auto execution_count() const noexcept -> uint32_t {
        return control_ == nullptr ? 0 : control_->storage.execution_count();
    }
    [[nodiscard]] constexpr auto import_count() const noexcept -> uint32_t {
        return control_ == nullptr ? 0 : control_->storage.import_count();
    }
    [[nodiscard]] constexpr auto dependency_count() const noexcept -> uint32_t {
        return control_ == nullptr ? 0 : control_->storage.dependency_count();
    }
    [[nodiscard]] constexpr auto export_count() const noexcept -> uint32_t {
        return control_ == nullptr ? 0 : control_->storage.export_count();
    }
    [[nodiscard]] constexpr auto bootstrap_count() const noexcept -> uint32_t {
        return control_ == nullptr ? 0
            : control_->storage.bootstrap_count();
    }

    [[nodiscard]] auto task(uint32_t index) const noexcept -> const PlanTask* {
        return control_ == nullptr ? nullptr : control_->storage.task(index);
    }
    [[nodiscard]] auto image(uint32_t index) const noexcept -> const PlanImage* {
        return control_ == nullptr ? nullptr : control_->storage.image(index);
    }
    [[nodiscard]] auto mapping(uint32_t index) const noexcept
        -> const PlanMapping* {
        return control_ == nullptr ? nullptr : control_->storage.mapping(index);
    }
    [[nodiscard]] auto object(uint32_t index) const noexcept
        -> const PlanObject* {
        return control_ == nullptr ? nullptr : control_->storage.object(index);
    }
    [[nodiscard]] auto execution(uint32_t index) const noexcept
        -> const PlanExecution* {
        return control_ == nullptr ? nullptr
            : control_->storage.execution(index);
    }
    [[nodiscard]] auto import(uint32_t index) const noexcept
        -> const PlanImport* {
        return control_ == nullptr ? nullptr : control_->storage.import(index);
    }
    [[nodiscard]] auto dependency(uint32_t index) const noexcept
        -> const PlanDependency* {
        return control_ == nullptr
            ? nullptr : control_->storage.dependency(index);
    }
    [[nodiscard]] auto export_record(uint32_t index) const noexcept
        -> const PlanExport* {
        return control_ == nullptr
            ? nullptr : control_->storage.export_record(index);
    }
    [[nodiscard]] auto bootstrap(uint32_t index) const noexcept
        -> const PlanBootstrap* {
        return control_ == nullptr ? nullptr
            : control_->storage.bootstrap(index);
    }

    [[nodiscard]] auto symbol(SymbolId symbol_id) const noexcept -> ByteView {
        return control_ == nullptr ? ByteView{}
            : control_->storage.symbol(symbol_id);
    }

    /* Plan identity is the durable lookup key for deployment callers.  The
     * query returns an index into this immutable plan; it does not create a
     * second task registry or retain a borrowed row. */
    [[nodiscard]] auto find_task(ByteView name) const noexcept
        -> libk::optional<uint32_t>;

    template<size_t N>
    [[nodiscard]] auto find_task(const char (&name)[N]) const noexcept
        -> libk::optional<uint32_t> {
        static_assert(N != 0);
        return find_task(ByteView{
            reinterpret_cast<const uint8_t*>(name), N - 1});
    }

    [[nodiscard]] auto lease() const noexcept -> libk::optional<PlanLease>;

private:
    friend class PlanLease;
    friend struct TaskPlanView;
    template<size_t Capacity, uint32_t GenerationLimit>
    friend class PlanSet;

    explicit DeploymentPlan(plan_detail::PlanControl* control) noexcept
        : control_(control) {}

    [[nodiscard]] auto decode_rows(const ManifestView& view) noexcept -> bool;
    void release_owner() noexcept;

    plan_detail::PlanControl* control_{};
};

class PlanLease final {
public:
    PlanLease() noexcept = default;
    PlanLease(const PlanLease&) = delete;
    auto operator=(const PlanLease&) -> PlanLease& = delete;

    PlanLease(PlanLease&& other) noexcept
        : control_(other.control_), id_(other.id_) {
        other.control_ = nullptr;
        other.id_ = {};
    }

    auto operator=(PlanLease&& other) noexcept -> PlanLease& {
        if (this == &other) {
            return *this;
        }
        release();
        control_ = other.control_;
        id_ = other.id_;
        other.control_ = nullptr;
        other.id_ = {};
        return *this;
    }

    ~PlanLease() noexcept { release(); }

    [[nodiscard]] auto valid() const noexcept -> bool {
        return control_ != nullptr && id_.valid()
            && control_->id == id_
            && (control_->state == plan_detail::SlotState::Owned
                || control_->state == plan_detail::SlotState::Retiring);
    }
    [[nodiscard]] constexpr auto id() const noexcept -> PlanId { return id_; }

    [[nodiscard]] auto task(uint32_t index) const noexcept -> TaskPlanView {
        return valid() && control_->storage.task(index) != nullptr
            ? TaskPlanView{control_, PlanTaskId{id_, index}}
            : TaskPlanView{};
    }

private:
    friend class DeploymentPlan;

    explicit PlanLease(plan_detail::PlanControl* control, PlanId id) noexcept
        : control_(control), id_(id) {}

    void release() noexcept {
        if (control_ == nullptr) {
            return;
        }
        libk_assert(control_->lease_count != 0);
        --control_->lease_count;
        if (control_->lease_count == 0
            && control_->state == plan_detail::SlotState::Retiring
            && control_->reclaim != nullptr) {
            control_->reclaim(control_);
        }
        control_ = nullptr;
        id_ = {};
    }

    plan_detail::PlanControl* control_{};
    PlanId id_{};
};

inline auto DeploymentPlan::lease() const noexcept -> libk::optional<PlanLease> {
    if (control_ == nullptr || control_->state != plan_detail::SlotState::Owned
        || !control_->id.valid() || control_->lease_count == UINT32_MAX) {
        return libk::nullopt;
    }
    ++control_->lease_count;
    return PlanLease{control_, control_->id};
}

inline auto TaskPlanView::valid() const noexcept -> bool {
    return control != nullptr && id.valid() && control->id == id.plan
        && (control->state == plan_detail::SlotState::Owned
            || control->state == plan_detail::SlotState::Retiring)
        && control->storage.task(id.index) != nullptr;
}

inline auto TaskPlanView::row() const noexcept -> const PlanTask* {
    return valid() ? control->storage.task(id.index) : nullptr;
}

inline auto TaskPlanView::image(uint32_t index) const noexcept
    -> const PlanImage* {
    const PlanTask* task = row();
    if (task == nullptr || index >= task->images.count) {
        return nullptr;
    }
    return control->storage.image(task->images.first + index);
}

inline auto TaskPlanView::mapping(uint32_t index) const noexcept
    -> const PlanMapping* {
    const PlanTask* task = row();
    if (task == nullptr || index >= task->mappings.count) {
        return nullptr;
    }
    return control->storage.mapping(task->mappings.first + index);
}

inline auto TaskPlanView::object(uint32_t index) const noexcept
    -> const PlanObject* {
    const PlanTask* task = row();
    if (task == nullptr || index >= task->objects.count) {
        return nullptr;
    }
    return control->storage.object(task->objects.first + index);
}

inline auto TaskPlanView::execution(uint32_t index) const noexcept
    -> const PlanExecution* {
    const PlanTask* task = row();
    if (task == nullptr || index >= task->executions.count) {
        return nullptr;
    }
    return control->storage.execution(task->executions.first + index);
}

inline auto TaskPlanView::import(uint32_t index) const noexcept
    -> const PlanImport* {
    const PlanTask* task = row();
    if (task == nullptr || index >= task->imports.count) {
        return nullptr;
    }
    return control->storage.import(task->imports.first + index);
}

inline auto TaskPlanView::dependency(uint32_t index) const noexcept
    -> const PlanDependency* {
    const PlanTask* task = row();
    if (task == nullptr || index >= task->dependencies.count) {
        return nullptr;
    }
    return control->storage.dependency(task->dependencies.first + index);
}

inline auto TaskPlanView::export_record(uint32_t index) const noexcept
    -> const PlanExport* {
    const PlanTask* task = row();
    if (task == nullptr || index >= task->exports.count) {
        return nullptr;
    }
    return control->storage.export_record(task->exports.first + index);
}

inline auto TaskPlanView::bootstrap(uint32_t index) const noexcept
    -> const PlanBootstrap* {
    const PlanTask* task = row();
    if (task == nullptr || index >= task->bootstraps.count) {
        return nullptr;
    }
    return control->storage.bootstrap(task->bootstraps.first + index);
}

inline auto TaskPlanView::symbol(SymbolId symbol_id) const noexcept -> ByteView {
    return valid() ? control->storage.symbol(symbol_id) : ByteView{};
}

inline auto DeploymentPlan::find_task(ByteView name) const noexcept
    -> libk::optional<uint32_t> {
    if (!name) {
        return libk::nullopt;
    }
    for (uint32_t index = 0; index < task_count(); ++index) {
        const PlanTask* const row = task(index);
        if (row != nullptr && symbol(row->name).equals(name)) {
            return index;
        }
    }
    return libk::nullopt;
}

template<size_t Capacity, uint32_t GenerationLimit>
class PlanSet final {
    static_assert(Capacity != 0);
    static_assert(GenerationLimit != 0);

public:
    /* PlanSet is the explicit caller-owned plan registry/storage arena. */
    PlanSet() noexcept {
        for (size_t index = 0; index < Capacity; ++index) {
            controls_[index].id = PlanId{
                static_cast<uint32_t>(index), 1};
        }
    }

    PlanSet(const PlanSet&) = delete;
    auto operator=(const PlanSet&) -> PlanSet& = delete;

    ~PlanSet() noexcept {
        for (const auto& control : controls_) {
            libk_assert(control.state == plan_detail::SlotState::Vacant
                || control.state == plan_detail::SlotState::Retired);
            libk_assert(control.lease_count == 0);
        }
    }

    [[nodiscard]] auto decode(const ManifestView& view) noexcept
        -> libk::Expected<DeploymentPlan, PlanError> {
        plan_detail::PlanControl* control = nullptr;
        for (auto& candidate : controls_) {
            if (candidate.state == plan_detail::SlotState::Vacant) {
                control = &candidate;
                break;
            }
        }
        if (control == nullptr) {
            return libk::unexpected(PlanError::Capacity);
        }

        control->state = plan_detail::SlotState::Owned;
        control->owner = this;
        control->reclaim = &reclaim_control;
        DeploymentPlan result{control};
        if (!result.decode_rows(view)) {
            result.release_owner();
            return libk::unexpected(PlanError::InvalidInput);
        }
        return libk::expected(libk::move(result));
    }

    [[nodiscard]] constexpr auto capacity() const noexcept -> size_t {
        return Capacity;
    }

private:
    static void reclaim_control(plan_detail::PlanControl* control) noexcept {
        libk_assert(control != nullptr && control->owner != nullptr);
        auto* owner = static_cast<PlanSet*>(control->owner);
        owner->reclaim(*control);
    }

    void reclaim(plan_detail::PlanControl& control) noexcept {
        libk_assert(control.state == plan_detail::SlotState::Retiring);
        libk_assert(control.lease_count == 0);
        control.storage.clear();
        if (control.id.generation >= GenerationLimit) {
            control.state = plan_detail::SlotState::Retired;
            control.owner = nullptr;
            control.reclaim = nullptr;
            return;
        }
        ++control.id.generation;
        control.state = plan_detail::SlotState::Vacant;
        control.owner = nullptr;
        control.reclaim = nullptr;
    }

    plan_detail::PlanControl controls_[Capacity]{};
};

template<size_t Capacity, uint32_t GenerationLimit>
inline auto DeploymentPlan::decode(
    const ManifestView& view,
    PlanSet<Capacity, GenerationLimit>& plans) noexcept
    -> libk::Expected<DeploymentPlan, PlanError> {
    return plans.decode(view);
}

inline void DeploymentPlan::release_owner() noexcept {
    if (control_ == nullptr) {
        return;
    }
    plan_detail::PlanControl* control = control_;
    control_ = nullptr;
    libk_assert(control->state == plan_detail::SlotState::Owned);
    control->state = plan_detail::SlotState::Retiring;
    if (control->lease_count == 0 && control->reclaim != nullptr) {
        control->reclaim(control);
    }
}

inline auto DeploymentPlan::decode_rows(const ManifestView& view) noexcept
    -> bool {
    PlanStorage* storage_ = control_ == nullptr ? nullptr : &control_->storage;
    if (storage_ == nullptr) {
        return false;
    }
    const ByteView string_table = view.string_table();
    if (string_table.size() > MYOS_DEPLOY_STRING_MAX) {
        return false;
    }
    for (size_t index = 0; index < string_table.size(); ++index) {
        storage_->strings_[index] = string_table[index];
    }
    storage_->string_size_ = static_cast<uint32_t>(string_table.size());

    const auto symbol = [](StringRef ref) noexcept -> SymbolId {
        return SymbolId{ref.offset, ref.length};
    };

    for (uint32_t index = 0; index < view.task_count(); ++index) {
        ManifestTaskRow source{};
        if (!view.task_row(index, source)) {
            return false;
        }
        PlanTask& row = storage_->tasks_[index];
        row.name = symbol(source.name);
        row.pool_key = symbol(source.pool_key);
        row.vspace_key = symbol(source.vspace_key);
        row.cspace_key = symbol(source.cspace_key);
        row.images = {source.image_first, source.image_count};
        row.mappings = {source.mapping_first, source.mapping_count};
        row.objects = {source.object_first, source.object_count};
        row.executions = {source.execution_first, source.execution_count};
        row.imports = {source.import_first, source.import_count};
        row.dependencies = {source.dependency_first, source.dependency_count};
        row.exports = {source.export_first, source.export_count};
        row.pool_memory = source.pool_memory;
        row.pool_caps = source.pool_caps;
        row.kind_mask = source.kind_mask;
        row.critical_bytes = source.critical_bytes;
        row.cspace_slots = source.cspace_slots;
        row.cspace_pages = source.cspace_pages;
        row.bootstrap_mapping = source.bootstrap_mapping;
        row.flags = source.flags;
        row.readiness = source.readiness;
        row.terminal = source.terminal;
        row.restart = source.restart;
        row.readiness_value = source.readiness_value;
        row.bootstraps = {source.bootstrap_first, source.bootstrap_count};
    }
    storage_->task_count_ = view.task_count();

    for (uint32_t index = 0; index < view.image_count(); ++index) {
        ManifestImageRow source{};
        if (!view.image_row(index, source)) {
            return false;
        }
        PlanImage& row = storage_->images_[index];
        row.source = symbol(source.source);
        row.source_kind = source.source_kind;
        row.flags = source.flags;
    }
    storage_->image_count_ = view.image_count();

    for (uint32_t index = 0; index < view.mapping_count(); ++index) {
        ManifestMappingRow source{};
        if (!view.mapping_row(index, source)) {
            return false;
        }
        PlanMapping& row = storage_->mappings_[index];
        row.produced = symbol(source.produced);
        row.pager = symbol(source.pager);
        row.region = symbol(source.region);
        row.image = source.image;
        row.segment = source.segment;
        row.source = source.source;
        row.residency = source.residency;
        row.critical = source.critical;
        row.flags = source.flags;
        row.access = source.access;
        row.pager_policy = source.pager_policy;
        row.address = source.address;
        row.size = source.size;
    }
    storage_->mapping_count_ = view.mapping_count();

    for (uint32_t index = 0; index < view.object_count(); ++index) {
        ManifestObjectRow source{};
        if (!view.object_row(index, source)) {
            return false;
        }
        PlanObject& row = storage_->objects_[index];
        row.output = symbol(source.output);
        row.output_b = symbol(source.output_b);
        row.kind = source.kind;
        row.flags = source.flags;
        for (size_t ref = 0; ref < 4; ++ref) {
            row.refs[ref] = source.refs[ref];
        }
        for (size_t arg = 0; arg < 6; ++arg) {
            row.args[arg] = source.args[arg];
        }
    }
    storage_->object_count_ = view.object_count();

    for (uint32_t index = 0; index < view.execution_count(); ++index) {
        ManifestExecutionRow source{};
        if (!view.execution_row(index, source)) {
            return false;
        }
        PlanExecution& row = storage_->executions_[index];
        row.key = symbol(source.key);
        row.sc = symbol(source.sc);
        row.domain = symbol(source.domain);
        row.image = source.image;
        row.stack = source.stack;
        row.bootstrap = source.bootstrap;
        row.ipc = source.ipc;
        row.control = source.control;
        row.event = source.event;
        row.model = source.model;
        row.flags = source.flags;
        row.fault = source.fault;
        row.terminal = source.terminal;
        row.entry = source.entry;
        row.stack_top = source.stack_top;
        row.sc_budget = source.sc_budget;
        row.sc_period = source.sc_period;
        row.urgency = source.urgency;
        row.home_cpu = source.home_cpu;
    }
    storage_->execution_count_ = view.execution_count();

    for (uint32_t index = 0; index < view.import_count(); ++index) {
        ManifestImportRow source{};
        if (!view.import_row(index, source)) {
            return false;
        }
        PlanImport& row = storage_->imports_[index];
        row.source = symbol(source.source);
        row.destination = symbol(source.destination);
        row.mode = source.mode;
        row.selector = source.selector;
        row.flags = source.flags;
        row.attenuation = source.attenuation;
        row.source_class = source.source_class;
    }
    storage_->import_count_ = view.import_count();

    for (uint32_t index = 0; index < view.dependency_count(); ++index) {
        ManifestDependencyRow source{};
        if (!view.dependency_row(index, source)) {
            return false;
        }
        PlanDependency& row = storage_->dependencies_[index];
        row.target = source.target;
        row.kind = source.kind;
        row.flags = source.flags;
        row.relation = symbol(source.relation);
    }
    storage_->dependency_count_ = view.dependency_count();

    for (uint32_t index = 0; index < view.export_count(); ++index) {
        ManifestExportRow source{};
        if (!view.export_row(index, source)) {
            return false;
        }
        PlanExport& row = storage_->exports_[index];
        row.source = symbol(source.source);
        row.key = symbol(source.key);
        row.source_class = source.source_class;
        row.flags = source.flags;
        row.ceiling = source.ceiling;
    }
    storage_->export_count_ = view.export_count();
    for (uint32_t index = 0; index < view.bootstrap_count(); ++index) {
        ManifestBootstrapRow source{};
        if (!view.bootstrap_row(index, source)) {
            return false;
        }
        PlanBootstrap& row = storage_->bootstraps_[index];
        row.kind = source.kind;
        row.destination = symbol(source.destination);
    }
    storage_->bootstrap_count_ = view.bootstrap_count();
    return true;
}

} // namespace myos::deploy
