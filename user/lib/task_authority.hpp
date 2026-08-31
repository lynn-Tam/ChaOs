#pragma once

/*
 * Cut B userspace authority admission and capability import primitives.
 *
 * AuthoritySet is an index of borrowed current-CSpace source capabilities. It
 * never owns a selector. The source owner keeps the reciprocal Registration
 * in a fixed journal and must retire that journal before closing its own
 * TaskSpace. ImportTransaction adopts every destination selector immediately
 * into TaskSpace; it never keeps a second owner for an adopted result.
 */

#include <stddef.h>
#include <stdint.h>

#include <libk/assert.hpp>
#include <libk/concepts.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <uapi/capability.h>
#include <uapi/deploy.h>
#include <uapi/object.h>
#include <uapi/status.h>

#include <user/lib/deployment.hpp>
#include <user/lib/deployment_plan.hpp>
#include <user/lib/cap_attenuation.hpp>

namespace myos::deploy {

struct AuthorityId final {
    uint32_t slot{};
    uint32_t generation{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return generation != 0;
    }

    constexpr auto operator==(const AuthorityId&) const noexcept -> bool =
        default;
};

[[nodiscard]] constexpr auto valid_authority_ceiling(
    const myos_cap_attenuation& ceiling) noexcept -> bool {
    return attenuation::valid_descriptor(
        ceiling, attenuation::DescriptorForm::Ceiling);
}

[[nodiscard]] constexpr auto rights_within_ceiling(
    const myos_cap_attenuation& requested,
    const myos_cap_attenuation& ceiling) noexcept -> bool {
    return attenuation::within(
        requested, ceiling, MYOS_DEPLOY_IMPORT_TYPED_DELEGATE);
}

[[nodiscard]] constexpr auto attenuation_within_ceiling(
    const myos_cap_attenuation& requested,
    const myos_cap_attenuation& ceiling,
    uint16_t mode) noexcept -> bool {
    return attenuation::within(requested, ceiling, mode);
}

/* A reciprocal source-owner token.  It is deliberately non-copyable and its
 * destructor rejects an unretired token instead of silently losing a back
 * reference to the AuthoritySet entry. */
class Registration final {
public:
    Registration() noexcept = default;
    Registration(const Registration&) = delete;
    auto operator=(const Registration&) -> Registration& = delete;

    Registration(Registration&& other) noexcept
        : context_(other.context_), id_(other.id_), retire_(other.retire_),
          active_(other.active_) {
        other.clear();
    }

    auto operator=(Registration&& other) noexcept -> Registration& {
        if (this == &other) {
            return *this;
        }
        libk_assert(!active_);
        context_ = other.context_;
        id_ = other.id_;
        retire_ = other.retire_;
        active_ = other.active_;
        other.clear();
        return *this;
    }

    ~Registration() noexcept {
        // The source owner must use retire()/retire_all() before destruction.
        // Keeping this a checked fail-stop gate prevents stale borrowed
        // authority from surviving its sole real owner.
        if (active_) {
            libk_assert(false);
        }
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return active_ && context_ != nullptr && retire_ != nullptr
            && id_.valid();
    }

    [[nodiscard]] constexpr auto id() const noexcept -> AuthorityId {
        return id_;
    }

    /* BUSY starts retirement (denies new leases) but retains this token until
     * all existing leases release.  The caller retries the same token. */
    [[nodiscard]] auto retire() noexcept -> myos_status_t {
        if (!valid()) {
            return MYOS_STATUS_OK;
        }
        const myos_status_t status = retire_(context_, id_);
        if (status == MYOS_STATUS_OK) {
            clear();
        }
        return status;
    }

private:
    using RetireFn = myos_status_t (*)(void*, AuthorityId) noexcept;

    Registration(void* context, AuthorityId id, RetireFn retire) noexcept
        : context_(context), id_(id), retire_(retire), active_(true) {}

    void clear() noexcept {
        context_ = nullptr;
        id_ = {};
        retire_ = nullptr;
        active_ = false;
    }

    template<size_t, size_t, uint32_t>
    friend class AuthoritySet;

    void* context_{};
    AuthorityId id_{};
    RetireFn retire_{};
    bool active_{};
};

/*
 * Append-only reciprocal registration storage.  Retired entries remain
 * tombstones, so pressure is visible and cannot be hidden by retrying a
 * construction against the same finite journal.
 */
template<size_t Capacity>
class RegistrationJournal final {
    static_assert(Capacity != 0);

public:
    RegistrationJournal() noexcept = default;
    RegistrationJournal(const RegistrationJournal&) = delete;
    auto operator=(const RegistrationJournal&) -> RegistrationJournal& =
        delete;

    RegistrationJournal(RegistrationJournal&& other) noexcept
        : entries_(libk::move(other.entries_)) {}

    auto operator=(RegistrationJournal&& other) noexcept
        -> RegistrationJournal& {
        if (this == &other) {
            return *this;
        }
        libk_assert(live_size() == 0);
        entries_ = libk::move(other.entries_);
        return *this;
    }

    ~RegistrationJournal() noexcept {
        libk_assert(live_size() == 0);
    }

    [[nodiscard]] auto retire_all() noexcept -> myos_status_t {
        for (Registration& registration : entries_) {
            if (!registration.valid()) {
                continue;
            }
            const myos_status_t status = registration.retire();
            if (status != MYOS_STATUS_OK) {
                return status;
            }
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return entries_.size();
    }

    [[nodiscard]] auto live_size() const noexcept -> size_t {
        size_t result = 0;
        for (const Registration& registration : entries_) {
            result += registration.valid() ? 1 : 0;
        }
        return result;
    }

    [[nodiscard]] static constexpr auto capacity() noexcept -> size_t {
        return Capacity;
    }

    /* Register an external source and retain its reciprocal Registration in
     * this journal.  The AuthoritySet's raw registration remains private;
     * callers must use the journal so source-owner lifetime is explicit and
     * shutdown can retire every entry before the source CSpace is closed. */
    template<typename Set>
    [[nodiscard]] auto register_source(
        Set& authorities,
        cap::CapRef source,
        uint64_t identity,
        const myos_cap_attenuation& ceiling) noexcept
        -> libk::optional<AuthorityId> {
        if (entries_.size() == Capacity) {
            return libk::nullopt;
        }
        auto registration = authorities.register_source(
            source, identity, ceiling);
        if (!registration) {
            return libk::nullopt;
        }
        const AuthorityId id = registration->id();
        if (!adopt(libk::move(*registration))) {
            // No lease can exist yet, so exact retirement is immediate.  A
            // failed retirement is an ownership fault rather than a leak.
            const myos_status_t status = registration->retire();
            if (status != MYOS_STATUS_OK) {
                libk_assert(false);
            }
            return libk::nullopt;
        }
        return id;
    }

private:
    [[nodiscard]] auto adopt(Registration&& registration) noexcept -> bool {
        if (!registration.valid() || entries_.size() == Capacity) {
            return false;
        }
        return entries_.try_push_back(libk::move(registration));
    }

    template<size_t, size_t, uint32_t>
    friend class AuthoritySet;
    template<size_t>
    friend class RegistrationOwner;

    libk::InplaceVector<Registration, Capacity> entries_{};
};

/*
 * Common close boundary for TaskRecord and bootstrap source owners.  The
 * owner is intentionally generic over the closeable aggregate: no caller can
 * accidentally close a TaskSpace without draining this journal when using
 * this boundary.
 */
template<size_t RegistrationCapacity>
class RegistrationOwner final {
public:
    using journal_type = RegistrationJournal<RegistrationCapacity>;

    RegistrationOwner() noexcept = default;
    RegistrationOwner(const RegistrationOwner&) = delete;
    auto operator=(const RegistrationOwner&) -> RegistrationOwner& = delete;

    RegistrationOwner(RegistrationOwner&& other) noexcept
        : journal_(libk::move(other.journal_)) {}

    auto operator=(RegistrationOwner&& other) noexcept -> RegistrationOwner& {
        if (this == &other) {
            return *this;
        }
        libk_assert(journal_.live_size() == 0);
        journal_ = libk::move(other.journal_);
        return *this;
    }

    ~RegistrationOwner() noexcept = default;

private:
    template<typename Authorities>
    [[nodiscard]] auto register_source(
        Authorities& authorities,
        cap::CapRef source,
        uint64_t identity,
        const myos_cap_attenuation& ceiling) noexcept
        -> libk::optional<AuthorityId> {
        return journal_.register_source(
            authorities, source, identity, ceiling);
    }

    template<typename Authorities, typename Space>
    [[nodiscard]] auto register_source(
        Authorities& authorities,
        Space& space,
        LocalSlot slot,
        uint64_t identity,
        const myos_cap_attenuation& ceiling) noexcept
        -> libk::optional<AuthorityId> {
        const auto source = space.lookup(slot, ceiling.kind);
        if (!source) {
            return libk::nullopt;
        }
        return journal_.register_source(
            authorities, source.value(), identity, ceiling);
    }

    template<typename Space>
    [[nodiscard]] auto close(Space& space) noexcept -> myos_status_t {
        const myos_status_t registrations = journal_.retire_all();
        if (registrations != MYOS_STATUS_OK) {
            return registrations;
        }
        return space.close();
    }

    template<typename Space, size_t>
    friend class RegisteredSpace;

    template<typename>
    friend class TaskRecord;

public:

    [[nodiscard]] auto has_live_registrations() const noexcept -> bool {
        return journal_.live_size() != 0;
    }

private:
    journal_type journal_{};
};

/* A bootstrap/source aggregate keeps the Space and registration owner
 * together.  It intentionally exposes no mutable Space or direct close path
 * that could bypass registration retirement. */
template<typename Space, size_t RegistrationCapacity>
class RegisteredSpace final {
public:
    using space_type = Space;
    using owner_type = RegistrationOwner<RegistrationCapacity>;

    RegisteredSpace() noexcept = default;
    RegisteredSpace(const RegisteredSpace&) = delete;
    auto operator=(const RegisteredSpace&) -> RegisteredSpace& = delete;

    RegisteredSpace(RegisteredSpace&& other) noexcept
        : space_(libk::move(other.space_)),
          owner_(libk::move(other.owner_)), adopted_(other.adopted_) {
        other.adopted_ = false;
    }

    auto operator=(RegisteredSpace&& other) noexcept -> RegisteredSpace& {
        if (this == &other) {
            return *this;
        }
        libk_assert(space_.phase() == Phase::Closed);
        libk_assert(!owner_.has_live_registrations());
        space_ = libk::move(other.space_);
        owner_ = libk::move(other.owner_);
        adopted_ = other.adopted_;
        other.adopted_ = false;
        return *this;
    }

    ~RegisteredSpace() noexcept {
        libk_assert(!owner_.has_live_registrations());
        libk_assert(!adopted_ || space_.phase() == Phase::Closed);
    }

    /* One-shot bootstrap/source aggregate adoption.  Every precondition is
     * checked before moving the caller-owned Space, so rejection preserves
     * the original selector owner. */
    [[nodiscard]] auto adopt(Space&& source) noexcept -> bool {
        if (adopted_ || source.phase() != Phase::Open
            || owner_.has_live_registrations()) {
            return false;
        }
        space_ = libk::move(source);
        adopted_ = true;
        return true;
    }

    template<typename Authorities>
    [[nodiscard]] auto register_source(
        Authorities& authorities,
        LocalSlot slot,
        uint64_t identity,
        const myos_cap_attenuation& ceiling) noexcept
        -> libk::optional<AuthorityId> {
        if (!adopted_) {
            return libk::nullopt;
        }
        return owner_.register_source(
            authorities, space_, slot, identity, ceiling);
    }

    [[nodiscard]] auto close() noexcept -> myos_status_t {
        return adopted_ ? owner_.close(space_) : MYOS_STATUS_OK;
    }

    [[nodiscard]] constexpr auto phase() const noexcept -> Phase {
        return space_.phase();
    }

    [[nodiscard]] auto pool() const noexcept
        -> libk::optional<cap::CapRef> {
        return space_.pool();
    }

    [[nodiscard]] constexpr auto vspace_slot() const noexcept -> LocalSlot {
        return space_.vspace_slot();
    }

    [[nodiscard]] constexpr auto manager_slot() const noexcept -> LocalSlot {
        return space_.manager_slot();
    }

    [[nodiscard]] auto lookup(
        LocalSlot slot,
        myos_object_kind_t expected_kind) const noexcept
        -> libk::optional<cap::CapRef> {
        return space_.lookup(slot, expected_kind);
    }

private:
    Space space_{};
    owner_type owner_{};
    bool adopted_{};
};

inline constexpr size_t kImportBatchMax =
    MYOS_DEPLOY_TASK_IMPORT_MAX < 32U ? MYOS_DEPLOY_TASK_IMPORT_MAX : 32U;

/* Public-max cumulative TaskSpace demand.  Local selectors are append-only:
 * each mapping/object/execution may leave a closed construction tombstone,
 * and a typed-import task reserves one reusable descriptor-carrier tombstone,
 * so this is deliberately a cumulative bound rather than a live count. */
inline constexpr size_t kTaskLocalCapacity =
    1U + 2U * MYOS_DEPLOY_TASK_MAPPING_MAX
    + 2U * MYOS_DEPLOY_TASK_OBJECT_MAX
    + 3U * MYOS_DEPLOY_TASK_EXECUTION_MAX + 1U;

/* Exact public-max lease demand: one per pre-admitted execution-domain
 * binding, one per system-Pager binding, and one per current import batch. */
inline constexpr size_t kAuthorityLeaseCapacity =
    MYOS_DEPLOY_TASK_EXECUTION_MAX
    + MYOS_DEPLOY_TASK_MAPPING_MAX
    + kImportBatchMax;

/* Every accepted import mode creates at most one destination selector; a
 * failed attempt leaves one tombstone instead of creating a second entry. */
inline constexpr size_t kTaskImportRemoteCapacity =
    MYOS_DEPLOY_TASK_IMPORT_MAX;

static_assert(kTaskLocalCapacity == 434U);
static_assert(kTaskImportRemoteCapacity == 128U);
static_assert(kAuthorityLeaseCapacity == 176U);

inline constexpr size_t kBootstrapDomainRegistrationCapacity =
    MYOS_DEPLOY_TASK_EXECUTION_MAX;
inline constexpr size_t kBootstrapPagerRegistrationCapacity =
    MYOS_DEPLOY_TASK_MAPPING_MAX;
inline constexpr size_t kBootstrapRegistrationCapacity =
    kBootstrapDomainRegistrationCapacity
    + kBootstrapPagerRegistrationCapacity;

template<
    size_t Capacity,
    size_t LeaseCapacity = kAuthorityLeaseCapacity,
    uint32_t GenerationLimit = UINT32_MAX>
class AuthoritySet;

template<size_t Capacity, size_t LeaseCapacity, uint32_t GenerationLimit>
class AuthoritySet final {
    static_assert(Capacity != 0);
    static_assert(GenerationLimit != 0);

    struct Entry final {
        cap::CapRef source{};
        myos_cap_attenuation ceiling{};
        uint64_t identity{};
        uint32_t generation{1};
        size_t leases{};
        bool occupied{};
        bool retiring{};
        bool retired{};
    };

public:
    class Lease final {
    public:
        Lease() noexcept = default;
        Lease(const Lease&) = delete;
        auto operator=(const Lease&) -> Lease& = delete;

        Lease(Lease&& other) noexcept
            : authorities_(other.authorities_), id_(other.id_),
              active_(other.active_) {
            other.authorities_ = nullptr;
            other.id_ = {};
            other.active_ = false;
        }

        auto operator=(Lease&& other) noexcept -> Lease& {
            if (this == &other) {
                return *this;
            }
            release();
            authorities_ = other.authorities_;
            id_ = other.id_;
            active_ = other.active_;
            other.authorities_ = nullptr;
            other.id_ = {};
            other.active_ = false;
            return *this;
        }

        ~Lease() noexcept { release(); }

        [[nodiscard]] auto valid() const noexcept -> bool {
            return active_ && authorities_ != nullptr
                && authorities_->lease_valid(id_);
        }

        [[nodiscard]] constexpr auto id() const noexcept -> AuthorityId {
            return id_;
        }

        [[nodiscard]] auto source() const noexcept -> cap::CapRef {
            return valid() ? authorities_->source(id_) : cap::CapRef{};
        }

        [[nodiscard]] auto ceiling() const noexcept -> myos_cap_attenuation {
            return valid() ? authorities_->ceiling(id_)
                           : myos_cap_attenuation{};
        }

        void release() noexcept {
            if (!active_) {
                return;
            }
            libk_assert(authorities_ != nullptr);
            const bool released = authorities_->release(id_);
            libk_assert(released);
            authorities_ = nullptr;
            id_ = {};
            active_ = false;
        }

    private:
        friend class AuthoritySet;
        Lease(AuthoritySet* authorities, AuthorityId id) noexcept
            : authorities_(authorities), id_(id), active_(true) {}

        AuthoritySet* authorities_{};
        AuthorityId id_{};
        bool active_{};
    };

    AuthoritySet() noexcept = default;
    AuthoritySet(const AuthoritySet&) = delete;
    auto operator=(const AuthoritySet&) -> AuthoritySet& = delete;
    AuthoritySet(AuthoritySet&&) = delete;
    auto operator=(AuthoritySet&&) -> AuthoritySet& = delete;

    ~AuthoritySet() noexcept {
        libk_assert(live_leases_ == 0);
        for (const Entry& entry : entries_) {
            libk_assert(!entry.occupied);
        }
    }

private:
    [[nodiscard]] auto register_source(
        cap::CapRef source,
        uint64_t identity,
        const myos_cap_attenuation& ceiling) noexcept
        -> libk::optional<Registration> {
        if (!source || source.cspace != 0
            || !valid_authority_ceiling(ceiling)) {
            return libk::nullopt;
        }
        for (const Entry& entry : entries_) {
            if (entry.occupied && entry.identity == identity) {
                return libk::nullopt;
            }
        }
        for (size_t index = 0; index < Capacity; ++index) {
            Entry& entry = entries_[index];
            if (entry.occupied || entry.retired) {
                continue;
            }
            entry.source = source;
            entry.ceiling = ceiling;
            entry.identity = identity;
            entry.leases = 0;
            entry.occupied = true;
            entry.retiring = false;
            ++active_entries_;
            return Registration{
                this,
                AuthorityId{
                    static_cast<uint32_t>(index), entry.generation},
                &retire_erased};
        }
        return libk::nullopt;
    }

    template<size_t>
    friend class RegistrationJournal;

public:

    [[nodiscard]] auto lease(AuthorityId id) noexcept
        -> libk::optional<Lease> {
        Entry* entry = checked(id);
        if (entry == nullptr || entry->retiring
            || live_leases_ == LeaseCapacity) {
            return libk::nullopt;
        }
        ++entry->leases;
        ++live_leases_;
        return Lease{this, id};
    }

private:
    [[nodiscard]] auto source(AuthorityId id) const noexcept -> cap::CapRef {
        const Entry* entry = checked(id);
        return entry == nullptr ? cap::CapRef{} : entry->source;
    }

    [[nodiscard]] auto ceiling(AuthorityId id) const noexcept
        -> myos_cap_attenuation {
        const Entry* entry = checked(id);
        return entry == nullptr ? myos_cap_attenuation{} : entry->ceiling;
    }

    [[nodiscard]] auto lease_valid(AuthorityId id) const noexcept -> bool {
        return checked(id) != nullptr;
    }

public:
    [[nodiscard]] auto active_entries() const noexcept -> size_t {
        return active_entries_;
    }

    [[nodiscard]] constexpr auto live_leases() const noexcept -> size_t {
        return live_leases_;
    }

    [[nodiscard]] static constexpr auto capacity() noexcept -> size_t {
        return Capacity;
    }

    [[nodiscard]] static constexpr auto lease_capacity() noexcept -> size_t {
        return LeaseCapacity;
    }

private:
    [[nodiscard]] static auto retire_erased(
        void* context,
        AuthorityId id) noexcept -> myos_status_t {
        return static_cast<AuthoritySet*>(context)->retire(id);
    }

    [[nodiscard]] auto checked(AuthorityId id) noexcept -> Entry* {
        if (!id.valid() || id.slot >= Capacity) {
            return nullptr;
        }
        Entry& entry = entries_[id.slot];
        return entry.occupied && entry.generation == id.generation
            ? &entry : nullptr;
    }

    [[nodiscard]] auto checked(AuthorityId id) const noexcept
        -> const Entry* {
        if (!id.valid() || id.slot >= Capacity) {
            return nullptr;
        }
        const Entry& entry = entries_[id.slot];
        return entry.occupied && entry.generation == id.generation
            ? &entry : nullptr;
    }

    [[nodiscard]] auto retire(AuthorityId id) noexcept -> myos_status_t {
        Entry* entry = checked(id);
        if (entry == nullptr) {
            return MYOS_STATUS_INVALID_CAP;
        }
        entry->retiring = true;
        if (entry->leases != 0) {
            return MYOS_STATUS_BUSY;
        }
        entry->source = {};
        entry->ceiling = {};
        entry->identity = 0;
        entry->occupied = false;
        entry->retiring = false;
        --active_entries_;
        if (entry->generation >= GenerationLimit) {
            entry->retired = true;
        } else {
            ++entry->generation;
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto release(AuthorityId id) noexcept -> bool {
        Entry* entry = checked(id);
        if (entry == nullptr || entry->leases == 0 || live_leases_ == 0) {
            return false;
        }
        --entry->leases;
        --live_leases_;
        return true;
    }

    Entry entries_[Capacity]{};
    size_t active_entries_{};
    size_t live_leases_{};
};

enum class ImportMode : uint16_t {
    TypedDelegate = MYOS_DEPLOY_IMPORT_TYPED_DELEGATE,
    Duplicate = MYOS_DEPLOY_IMPORT_DUPLICATE,
    ChannelMint = MYOS_DEPLOY_IMPORT_CHANNEL_MINT,
    Move = MYOS_DEPLOY_IMPORT_MOVE,
};

/* ImportTransaction-local transport input.  The caller-facing
 * TaskAuthorityBindings contains only the source AuthorityId; TaskBuilder
 * fills this descriptor view after it has opened and owns the child
 * TaskSpace.  The descriptor is never an ownership object. */
struct ImportBinding final {
    AuthorityId authority{};
    LocalSlot descriptor{};
    myos_word_t descriptor_offset{};
    /* Borrowed current-CSpace source for a TaskKey import.  It is valid only
     * until ImportTransaction adopts the destination into the child space. */
    cap::CapRef source{};
};

struct ImportProjection final {
    AuthorityId authority{};
    bool task_key{};
    size_t remote_index{static_cast<size_t>(-1)};
    myos_cap_t manager{};
    myos_object_kind_t kind{MYOS_OBJECT_KIND_INVALID};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return (authority.valid() || task_key)
            && remote_index != static_cast<size_t>(-1)
            && manager != 0
            && kind > MYOS_OBJECT_KIND_INVALID
            && kind < MYOS_OBJECT_KIND_COUNT;
    }
};

template<typename B>
concept ImportBackend = requires(
    cap::CapRef source,
    cap::CapRef destination,
    cap::CapRef descriptor,
    myos_word_t rights,
    myos_word_t badge) {
    { B::duplicate(source, destination, rights) }
        -> libk::SameAs<SysResult>;
    { B::typed_delegate(source, destination, descriptor, 0) }
        -> libk::SameAs<SysResult>;
    { B::channel_mint(source, destination, badge, rights) }
        -> libk::SameAs<SysResult>;
};

template<typename Space, typename Authorities, size_t BatchMax = 32>
requires ImportBackend<typename Space::backend_type>
class ImportTransaction final {
    static_assert(BatchMax != 0 && BatchMax <= 32);

    using Backend = typename Space::backend_type;
    using Lease = typename Authorities::Lease;
    using Owner = typename Space::owner_type;

public:
    ImportTransaction() = delete;

    /*
     * A batch is all-or-nothing for the current call.  Every destination cap
     * is adopted into TaskSpace before the next syscall.  Earlier successful
     * batches belong to the enclosing unpublished Task and are intentionally
     * left to its single strong-close path.
     */
    [[nodiscard]] static auto run(
        Space& space,
        const TaskPlanView& task,
        uint32_t first,
        uint32_t count,
        const ImportBinding* bindings,
        Authorities& authorities,
        ImportProjection* outputs) noexcept -> myos_status_t {
        const PlanTask* row = task.row();
        if (row == nullptr || count > BatchMax
            || first > row->imports.count
            || count > row->imports.count - first
            || (count != 0 && (bindings == nullptr || outputs == nullptr))) {
            return MYOS_STATUS_BAD_ARGS;
        }
        if (count == 0) {
            return MYOS_STATUS_OK;
        }

        libk::optional<Lease> leases[BatchMax]{};
        for (uint32_t index = 0; index < count; ++index) {
            const PlanImport* import = task.import(first + index);
            if (import == nullptr || !valid_import(*import)
                || import->mode == MYOS_DEPLOY_IMPORT_MOVE
                || (import->source_class
                        == MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY
                    ? !bindings[index].authority.valid()
                    : import->source_class
                            != MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY
                        || !bindings[index].source)) {
                return MYOS_STATUS_BAD_ARGS;
            }
        }

        const auto manager = space.lookup(
            space.manager_slot(), MYOS_OBJECT_KIND_CSPACE);
        if (!manager || manager->cspace != 0
            || space.remote_size() > Space::remote_capacity()
            || count > Space::remote_capacity() - space.remote_size()) {
            return MYOS_STATUS_NO_MEMORY;
        }

        /* All policy/binding/descriptor checks happen before the first
         * backend call.  Leases acquired here are the only bounded mutation
         * in preflight and their destructors release local counters. */
        for (uint32_t index = 0; index < count; ++index) {
            const PlanImport& import = *task.import(first + index);
            cap::CapRef source{};
            if (import.source_class == MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY) {
                auto lease = authorities.lease(bindings[index].authority);
                if (!lease) {
                    return MYOS_STATUS_BUSY;
                }
                source = lease->source();
                const myos_cap_attenuation ceiling = lease->ceiling();
                if (!source || source.cspace != 0
                    || !attenuation_within_ceiling(
                        import.attenuation, ceiling, import.mode)) {
                    return MYOS_STATUS_DENIED;
                }
                leases[index] = libk::move(*lease);
            } else {
                source = bindings[index].source;
                /* TaskBuilder resolves a TaskKey to a capability that this
                 * construction transaction already owns in the caller's
                 * current CSpace.  The destination is the child CSpace;
                 * remote selectors cannot be used as syscall sources without
                 * introducing a second source-authority ABI. */
                if (!source || source.cspace != 0) {
                    return MYOS_STATUS_BAD_ARGS;
                }
                /* No external Authority lease is fabricated for a TaskKey;
                 * its attenuation is still applied by the kernel import
                 * syscall. */
            }
            if (import.mode == MYOS_DEPLOY_IMPORT_TYPED_DELEGATE) {
                if (!bindings[index].descriptor.valid()
                    || bindings[index].descriptor.kind
                        != MYOS_OBJECT_KIND_MEMORY
                    || !space.lookup(
                        bindings[index].descriptor,
                        MYOS_OBJECT_KIND_MEMORY)
                    || bindings[index].descriptor_offset
                        % MYOS_CAP_ATTENUATION_SIZE != 0
                    || bindings[index].descriptor_offset
                        > MYOS_DEPLOY_PAGE_SIZE - MYOS_CAP_ATTENUATION_SIZE) {
                    return MYOS_STATUS_BAD_ARGS;
                }
            } else if (bindings[index].descriptor.valid()
                       || bindings[index].descriptor_offset != 0) {
                return MYOS_STATUS_BAD_ARGS;
            }
            if (import.mode == MYOS_DEPLOY_IMPORT_CHANNEL_MINT
                && import.attenuation.words[1] == 0) {
                return MYOS_STATUS_BAD_ARGS;
            }
        }

        size_t adopted = 0;
        size_t remote_indices[BatchMax]{};
        for (uint32_t index = 0; index < count; ++index) {
            const PlanImport& import = *task.import(first + index);
            const cap::CapRef source = import.source_class
                    == MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY
                ? leases[index]->source()
                : bindings[index].source;
            SysResult result{};
            switch (static_cast<ImportMode>(import.mode)) {
            case ImportMode::Duplicate:
                result = Backend::duplicate(
                    source, manager.value(), import.attenuation.rights);
                break;
            case ImportMode::TypedDelegate: {
                const auto descriptor = space.lookup(
                    bindings[index].descriptor,
                    MYOS_OBJECT_KIND_MEMORY);
                if (!descriptor) {
                    return rollback(
                        space, remote_indices, adopted, outputs,
                        MYOS_STATUS_BAD_ARGS);
                }
                result = Backend::typed_delegate(
                    source, manager.value(), descriptor.value(),
                    bindings[index].descriptor_offset);
                break;
            }
            case ImportMode::ChannelMint:
                result = Backend::channel_mint(
                    source, manager.value(), import.attenuation.words[1],
                    import.attenuation.rights);
                break;
            case ImportMode::Move:
                // Move is rejected in preflight; this branch is a defensive
                // non-call gate and must remain syscall-free.
                return rollback(
                    space, remote_indices, adopted, outputs,
                    MYOS_STATUS_BAD_ARGS);
            }
            if (result.value == 0) {
                const myos_status_t failure = result.status == MYOS_STATUS_OK
                    ? MYOS_STATUS_INVALID_CAP : result.status;
                return rollback(
                    space, remote_indices, adopted, outputs, failure);
            }

            /* A non-zero result is a destination-CSpace selector even when
             * the syscall reports failure.  Form its exact owner before any
             * rollback branch; an unexpected non-OK result must close that
             * selector or fail-stop rather than leak it. */
            Owner owner{cap::CapRef{result.value, manager->selector}};
            if (result.status != MYOS_STATUS_OK) {
                const myos_status_t closed = owner.close();
                if (closed != MYOS_STATUS_OK) {
                    Backend::ownership_fault(closed);
                    return rollback(
                        space, remote_indices, adopted, outputs, closed);
                }
                return rollback(
                    space, remote_indices, adopted, outputs, result.status);
            }

            const auto remote = space.adopt_remote_index(libk::move(owner));
            if (!remote) {
                const myos_status_t closed = owner.close();
                if (closed != MYOS_STATUS_OK) {
                    Backend::ownership_fault(closed);
                    return rollback(
                        space, remote_indices, adopted, outputs, closed);
                }
                return rollback(
                    space, remote_indices, adopted, outputs,
                    MYOS_STATUS_NO_MEMORY);
            }
            remote_indices[adopted] = remote.value();
            outputs[adopted] = ImportProjection{
                .authority = import.source_class
                        == MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY
                    ? leases[index]->id() : AuthorityId{},
                .task_key = import.source_class
                    == MYOS_DEPLOY_IMPORT_SOURCE_TASK_KEY,
                .remote_index = remote.value(),
                .manager = manager->selector,
                .kind = static_cast<myos_object_kind_t>(
                    import.attenuation.kind)};
            ++adopted;
        }

        for (uint32_t index = 0; index < count; ++index) {
            if (task.import(first + index)->source_class
                    == MYOS_DEPLOY_IMPORT_SOURCE_AUTHORITY
                && !leases[index]->valid()) {
                return rollback(
                    space, remote_indices, adopted, outputs,
                    MYOS_STATUS_INVALID_CAP);
            }
        }
        return MYOS_STATUS_OK;
    }

private:
    [[nodiscard]] static constexpr auto valid_import(
        const PlanImport& import) noexcept -> bool {
        if (import.selector != MYOS_DEPLOY_SELECTOR_ALLOCATED_KEYED
            || import.mode > MYOS_DEPLOY_IMPORT_MOVE
            || import.mode == MYOS_DEPLOY_IMPORT_MOVE) {
            return false;
        }
        if (!attenuation::valid_descriptor(
                import.attenuation,
                import.mode == MYOS_DEPLOY_IMPORT_DUPLICATE
                    ? attenuation::DescriptorForm::DuplicateRequest
                    : attenuation::DescriptorForm::TypedRequest)) {
            return false;
        }
        if (import.mode == MYOS_DEPLOY_IMPORT_CHANNEL_MINT
            && import.attenuation.kind != MYOS_OBJECT_KIND_CHANNEL) {
            return false;
        }
        return true;
    }

    [[nodiscard]] static auto rollback(
        Space& space,
        const size_t* remote_indices,
        size_t adopted,
        ImportProjection* outputs,
        myos_status_t failure) noexcept -> myos_status_t {
        myos_status_t rollback_status = MYOS_STATUS_OK;
        for (size_t index = adopted; index != 0; --index) {
            const myos_status_t status = space.close_remote(
                remote_indices[index - 1]);
            if (rollback_status == MYOS_STATUS_OK
                && status != MYOS_STATUS_OK) {
                rollback_status = status;
            }
            if (outputs != nullptr) {
                outputs[index - 1] = {};
            }
        }
        return rollback_status == MYOS_STATUS_OK
            ? failure : rollback_status;
    }
};

} // namespace myos::deploy
