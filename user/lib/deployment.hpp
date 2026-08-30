#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/inplace_vector.hpp>
#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <uapi/capability.h>
#include <uapi/deploy.h>
#include <uapi/object.h>
#include <uapi/status.h>
#include <uapi/vm.h>
#include <user/lib/boot_bundle.hpp>
#include <user/lib/capability.hpp>
#include <user/lib/sys_result.hpp>

namespace myos::deploy {

enum class Phase : uint8_t {
    Closed,
    Open,
    Draining,
    ResourceClosing,
    ResourceClosed,
};

enum class LeasePhase : uint8_t {
    Empty,
    Ready,
    Mapped,
    Unmapping,
    Destroying,
    Closing,
    Closed,
};

[[nodiscard]] constexpr auto committed(myos_status_t status) noexcept -> bool {
    return status == MYOS_STATUS_OK || status == MYOS_STATUS_PENDING;
}

[[nodiscard]] constexpr auto retryable(myos_status_t status) noexcept -> bool {
    return status == MYOS_STATUS_BUSY || status == MYOS_STATUS_RETRY;
}

struct Window final {
    myos_word_t address{};
    myos_word_t size{};

    /* Mapping callers use this checked rounding operation before constructing
     * a page-aligned window.  Zero represents overflow or an empty request. */
    [[nodiscard]] static constexpr auto round_size(myos_word_t value) noexcept
        -> myos_word_t {
        constexpr myos_word_t page_size = MYOS_DEPLOY_PAGE_SIZE;
        return value <= static_cast<myos_word_t>(-1) - (page_size - 1)
            ? (value + page_size - 1) & ~(page_size - 1)
            : 0;
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return address != 0 && size != 0
            && (address % MYOS_DEPLOY_PAGE_SIZE) == 0
            && (size % MYOS_DEPLOY_PAGE_SIZE) == 0
            && size <= ~myos_word_t{} - address;
    }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return address == 0 && size == 0;
    }

    [[nodiscard]] constexpr auto end() const noexcept -> myos_word_t {
        return valid() ? address + size : 0;
    }
};

[[nodiscard]] constexpr auto windows_disjoint(
    Window first, Window second) noexcept -> bool {
    if (first.empty() || second.empty()) {
        return true;
    }
    if (!first.valid() || !second.valid()) {
        return false;
    }
    return first.end() <= second.address || second.end() <= first.address;
}

template<typename T>
concept Backend = cap::CapBackend<T>
    && requires(
        cap::CapRef pool,
        cap::CapRef vspace,
        cap::CapRef region,
        cap::CapRef memory,
        myos_word_t words,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        myos_word_t types,
        myos_word_t rights) {
    { T::resource_create_child(pool, words, words, words) }
        -> libk::SameAs<SysResult>;
    { T::resource_close(pool) } -> libk::SameAs<myos_status_t>;
    { T::vspace_create(pool) } -> libk::SameAs<SysResult>;
    { T::cspace_create(pool, words, words) } -> libk::SameAs<SysResult>;
    { T::vm_create_region(
          vspace, address, size, access, types, rights) }
        -> libk::SameAs<SysResult>;
    { T::vm_map(region, memory, address, size, words, access) }
        -> libk::SameAs<myos_status_t>;
    { T::vm_unmap(region, address, size) }
        -> libk::SameAs<myos_status_t>;
    { T::vm_destroy_region(region) } -> libk::SameAs<myos_status_t>;
};

struct LocalSlot final {
    static constexpr size_t InvalidIndex = static_cast<size_t>(-1);

    myos_cap_t pool{};
    size_t index{};
    myos_object_kind_t kind{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return pool != 0 && kind > MYOS_OBJECT_KIND_INVALID
            && kind < MYOS_OBJECT_KIND_COUNT;
    }

    [[nodiscard]] constexpr auto is_manager() const noexcept -> bool {
        return valid() && index == InvalidIndex;
    }
};

template<size_t LocalCapacity, size_t RemoteCapacity, typename B>
requires Backend<B>
class TaskSpace final {
public:
    using backend_type = B;
    using owner_type = cap::BasicOwnedCap<B>;
    using caps_type = cap::BasicDeploymentCaps<
        LocalCapacity, RemoteCapacity, B>;

    TaskSpace() noexcept = default;
    TaskSpace(const TaskSpace&) = delete;
    auto operator=(const TaskSpace&) -> TaskSpace& = delete;

    TaskSpace(TaskSpace&& other) noexcept
        : pool_(libk::move(other.pool_)),
          caps_(libk::move(other.caps_)),
          local_kinds_(libk::move(other.local_kinds_)),
          phase_(other.phase_),
          initialized_(other.initialized_),
          vspace_slot_(other.vspace_slot_),
          manager_slot_(other.manager_slot_) {
        other.phase_ = Phase::Closed;
        other.initialized_ = false;
        other.vspace_slot_ = {};
        other.manager_slot_ = {};
    }

    auto operator=(TaskSpace&& other) noexcept -> TaskSpace& {
        if (this == &other) {
            return *this;
        }
        if (phase_ != Phase::Closed || initialized_) {
            B::ownership_fault(MYOS_STATUS_BUSY);
        }
        pool_ = libk::move(other.pool_);
        caps_ = libk::move(other.caps_);
        local_kinds_ = libk::move(other.local_kinds_);
        phase_ = other.phase_;
        initialized_ = other.initialized_;
        vspace_slot_ = other.vspace_slot_;
        manager_slot_ = other.manager_slot_;
        other.phase_ = Phase::Closed;
        other.initialized_ = false;
        other.vspace_slot_ = {};
        other.manager_slot_ = {};
        return *this;
    }

    ~TaskSpace() noexcept {
        if (!initialized_ || phase_ == Phase::Closed) {
            return;
        }
        if (phase_ != Phase::ResourceClosed) {
            B::ownership_fault(MYOS_STATUS_BUSY);
        }
        const myos_status_t status = pool_.close();
        if (status != MYOS_STATUS_OK) {
            B::ownership_fault(status);
        }
        phase_ = Phase::Closed;
    }

    // The factory is caller-owned and one-shot.  A failed child creation does
    // not install any ownership; later failures leave a strong-closeable
    // aggregate whose caller may retry close().
    [[nodiscard]] auto open(
        cap::CapRef parent_pool,
        myos_word_t memory,
        myos_word_t caps,
        myos_word_t kinds,
        myos_word_t cspace_slots,
        myos_word_t cspace_pages) noexcept -> myos_status_t {
        if (initialized_ || phase_ != Phase::Closed) {
            return MYOS_STATUS_BUSY;
        }
        if (!parent_pool || parent_pool.cspace != 0 || memory == 0
            || caps == 0 || kinds == 0 || cspace_slots == 0
            || cspace_pages == 0) {
            return MYOS_STATUS_BAD_ARGS;
        }

        const SysResult child = B::resource_create_child(
            parent_pool, memory, caps, kinds);
        if (child.status != MYOS_STATUS_OK || child.value == 0) {
            return child.status == MYOS_STATUS_OK
                ? MYOS_STATUS_INVALID_CAP
                : child.status;
        }
        pool_ = owner_type{cap::CapRef{child.value, 0}};
        initialized_ = true;
        phase_ = Phase::Open;

        const auto fail = [&](myos_status_t status) noexcept {
            static_cast<void>(close());
            return status;
        };

        const SysResult vspace = B::vspace_create(pool_.reference());
        if (vspace.status != MYOS_STATUS_OK || vspace.value == 0) {
            return fail(vspace.status == MYOS_STATUS_OK
                ? MYOS_STATUS_INVALID_CAP
                : vspace.status);
        }
        owner_type vspace_owner{cap::CapRef{vspace.value, 0}};
        const auto vspace_slot = adopt_local(
            libk::move(vspace_owner), MYOS_OBJECT_KIND_VSPACE);
        if (!vspace_slot) {
            return fail(MYOS_STATUS_NO_MEMORY);
        }
        vspace_slot_ = *vspace_slot;

        const SysResult cspace = B::cspace_create(
            pool_.reference(), cspace_slots, cspace_pages);
        if (cspace.status != MYOS_STATUS_OK || cspace.value == 0) {
            return fail(cspace.status == MYOS_STATUS_OK
                ? MYOS_STATUS_INVALID_CAP
                : cspace.status);
        }
        owner_type manager_owner{cap::CapRef{cspace.value, 0}};
        if (!caps_.install_manager(libk::move(manager_owner))) {
            return fail(MYOS_STATUS_BAD_ARGS);
        }
        manager_slot_ = LocalSlot{
            .pool = pool_.selector(),
            .index = LocalSlot::InvalidIndex,
            .kind = MYOS_OBJECT_KIND_CSPACE};
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto close() noexcept -> myos_status_t {
        if (!initialized_ || phase_ == Phase::Closed) {
            return MYOS_STATUS_OK;
        }
        if (phase_ == Phase::Open) {
            phase_ = Phase::Draining;
        }
        if (phase_ == Phase::Draining) {
            const myos_status_t status = caps_.close();
            if (status != MYOS_STATUS_OK) {
                return status;
            }
            phase_ = Phase::ResourceClosing;
        }
        if (phase_ == Phase::ResourceClosing) {
            if (!pool_) {
                phase_ = Phase::ResourceClosed;
            } else {
                const myos_status_t status = B::resource_close(
                    pool_.reference());
                if (status != MYOS_STATUS_OK) {
                    return status;
                }
                phase_ = Phase::ResourceClosed;
            }
        }
        if (phase_ == Phase::ResourceClosed) {
            const myos_status_t status = pool_.close();
            if (status != MYOS_STATUS_OK) {
                return status;
            }
            phase_ = Phase::Closed;
            return MYOS_STATUS_OK;
        }
        return MYOS_STATUS_INTERNAL;
    }

    [[nodiscard]] constexpr auto phase() const noexcept -> Phase {
        return phase_;
    }

    [[nodiscard]] auto pool() const noexcept -> libk::optional<cap::CapRef> {
        if (!pool_) {
            return libk::nullopt;
        }
        return pool_.reference();
    }

    [[nodiscard]] constexpr auto vspace_slot() const noexcept -> LocalSlot {
        return vspace_slot_;
    }

    [[nodiscard]] constexpr auto manager_slot() const noexcept -> LocalSlot {
        return manager_slot_;
    }

    [[nodiscard]] auto adopt_local(
        owner_type&& owner,
        myos_object_kind_t kind) noexcept -> libk::optional<LocalSlot> {
        if (phase_ != Phase::Open || !pool_ || !owner
            || owner.cspace() != 0 || !valid_kind(kind)
            || caps_.local_size() >= LocalCapacity) {
            return libk::nullopt;
        }
        if (!local_kinds_.try_push_back(kind)) {
            return libk::nullopt;
        }
        const auto index = caps_.adopt_local_slot(libk::move(owner));
        if (!index) {
            static_cast<void>(local_kinds_.try_pop_back());
            return libk::nullopt;
        }
        return LocalSlot{
            .pool = pool_.selector(), .index = *index, .kind = kind};
    }

    [[nodiscard]] auto adopt_remote(owner_type&& owner) noexcept -> bool {
        if (phase_ != Phase::Open) {
            return false;
        }
        return caps_.adopt_remote(libk::move(owner));
    }

    [[nodiscard]] auto adopt_remote_index(owner_type&& owner) noexcept
        -> libk::optional<size_t> {
        if (phase_ != Phase::Open) {
            return libk::nullopt;
        }
        return caps_.adopt_remote_index(libk::move(owner));
    }

    [[nodiscard]] auto close_remote(size_t index) noexcept -> myos_status_t {
        if (phase_ != Phase::Open) {
            return MYOS_STATUS_CLOSED;
        }
        return caps_.close_remote(index);
    }

    [[nodiscard]] auto can_adopt_remote() const noexcept -> bool {
        return phase_ == Phase::Open && caps_.can_adopt_remote();
    }

    [[nodiscard]] auto can_adopt_remote(const owner_type& owner) const noexcept
        -> bool {
        return phase_ == Phase::Open && caps_.can_adopt_remote(owner);
    }

    [[nodiscard]] auto lookup(
        LocalSlot slot,
        myos_object_kind_t expected_kind) const noexcept
        -> libk::optional<cap::CapRef> {
        if (phase_ != Phase::Open || !slot.valid()
            || slot.pool != pool_.selector() || slot.kind != expected_kind) {
            return libk::nullopt;
        }
        if (slot.is_manager()) {
            if (slot.kind != MYOS_OBJECT_KIND_CSPACE) {
                return libk::nullopt;
            }
            return caps_.borrow_manager();
        }
        if (slot.index >= local_kinds_.size()
            || local_kinds_[slot.index] != expected_kind) {
            return libk::nullopt;
        }
        return caps_.borrow_local(slot.index);
    }

    [[nodiscard]] auto lookup_remote(
        size_t index,
        myos_cap_t manager) const noexcept
        -> libk::optional<cap::CapRef> {
        if (phase_ != Phase::Open || manager == 0) {
            return libk::nullopt;
        }
        const auto current_manager = caps_.borrow_manager();
        if (!current_manager || current_manager->selector != manager) {
            return libk::nullopt;
        }
        return caps_.borrow_remote(index);
    }

    [[nodiscard]] auto close_slot(LocalSlot slot) noexcept -> myos_status_t {
        if (phase_ != Phase::Open || !slot.valid()
            || slot.pool != pool_.selector()) {
            return MYOS_STATUS_INVALID_CAP;
        }
        if (slot.is_manager()) {
            return MYOS_STATUS_BAD_RIGHTS;
        }
        if (slot.index >= local_kinds_.size()
            || local_kinds_[slot.index] != slot.kind) {
            return MYOS_STATUS_INVALID_CAP;
        }
        return caps_.close_local(slot.index);
    }

    [[nodiscard]] constexpr auto local_cumulative() const noexcept -> size_t {
        return caps_.local_size();
    }

    [[nodiscard]] static constexpr auto local_capacity() noexcept -> size_t {
        return LocalCapacity;
    }

    [[nodiscard]] static constexpr auto remote_capacity() noexcept -> size_t {
        return RemoteCapacity;
    }

    [[nodiscard]] constexpr auto remote_size() const noexcept -> size_t {
        return caps_.remote_size();
    }

    [[nodiscard]] auto remote_live_size() const noexcept -> size_t {
        return caps_.remote_live_size();
    }

private:
    [[nodiscard]] static constexpr auto valid_kind(
        myos_object_kind_t kind) noexcept -> bool {
        return kind > MYOS_OBJECT_KIND_INVALID
            && kind < MYOS_OBJECT_KIND_COUNT;
    }

    owner_type pool_{};
    caps_type caps_{};
    libk::InplaceVector<myos_object_kind_t, LocalCapacity> local_kinds_{};
    Phase phase_{Phase::Closed};
    bool initialized_{};
    LocalSlot vspace_slot_{};
    LocalSlot manager_slot_{};
};

template<typename B>
requires Backend<B>
class MappedBundle final {
public:
    using owner_type = cap::BasicOwnedCap<B>;

    MappedBundle() noexcept = default;
    MappedBundle(const MappedBundle&) = delete;
    auto operator=(const MappedBundle&) -> MappedBundle& = delete;

    MappedBundle(MappedBundle&& other) noexcept
        : root_(other.root_),
          window_(other.window_),
          size_(other.size_),
          region_(libk::move(other.region_)),
          view_(other.view_),
          phase_(other.phase_) {
        other.reset_empty();
    }

    auto operator=(MappedBundle&& other) noexcept -> MappedBundle& {
        if (this == &other) {
            return *this;
        }
        const myos_status_t status = close();
        if (status != MYOS_STATUS_OK) {
            B::ownership_fault(status);
        }
        root_ = other.root_;
        window_ = other.window_;
        size_ = other.size_;
        region_ = libk::move(other.region_);
        view_ = other.view_;
        phase_ = other.phase_;
        other.reset_empty();
        return *this;
    }

    ~MappedBundle() noexcept {
        if (phase_ == LeasePhase::Empty || phase_ == LeasePhase::Closed) {
            return;
        }
        const myos_status_t status = close();
        if (status != MYOS_STATUS_OK) {
            B::ownership_fault(status);
        }
    }

    [[nodiscard]] auto open(
        cap::CapRef root_vspace,
        cap::CapRef bundle_memory,
        Window window,
        size_t bundle_size,
        Window forbidden = {}) noexcept -> myos_status_t {
        if (phase_ != LeasePhase::Empty && phase_ != LeasePhase::Closed) {
            return MYOS_STATUS_BUSY;
        }
        if (!root_vspace || root_vspace.cspace != 0 || !bundle_memory
            || bundle_memory.cspace != 0 || !window.valid()
            || bundle_size == 0 || bundle_size > window.size
            || !windows_disjoint(window, forbidden)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        reset_empty();
        root_ = root_vspace;
        window_ = window;
        size_ = bundle_size;
        const SysResult created = B::vm_create_region(
            root_,
            window.address,
            window.size,
            MYOS_VM_READ,
            MYOS_VM_NORMAL,
            MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
        if (created.status != MYOS_STATUS_OK || created.value == 0) {
            reset_empty();
            return created.status == MYOS_STATUS_OK
                ? MYOS_STATUS_INVALID_CAP
                : created.status;
        }
        region_ = owner_type{cap::CapRef{created.value, 0}};
        phase_ = LeasePhase::Ready;
        const myos_status_t mapped = B::vm_map(
            region_.reference(),
            bundle_memory,
            window.address,
            window.size,
            0,
            MYOS_VM_READ);
        if (!committed(mapped)) {
            return mapped;
        }
        phase_ = LeasePhase::Mapped;
        view_ = boot::Bundle::parse(
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(window.address)),
            bundle_size);
        return static_cast<bool>(view_) ? MYOS_STATUS_OK : MYOS_STATUS_BAD_ARGS;
    }

    [[nodiscard]] auto close() noexcept -> myos_status_t {
        if (phase_ == LeasePhase::Empty || phase_ == LeasePhase::Closed) {
            return MYOS_STATUS_OK;
        }
        view_ = {};
        if (phase_ == LeasePhase::Mapped || phase_ == LeasePhase::Unmapping) {
            phase_ = LeasePhase::Unmapping;
            const myos_status_t status = B::vm_unmap(
                region_.reference(), window_.address, window_.size);
            if (!committed(status)) {
                return status;
            }
            phase_ = LeasePhase::Ready;
        }
        if (phase_ == LeasePhase::Ready || phase_ == LeasePhase::Destroying) {
            phase_ = LeasePhase::Destroying;
            const myos_status_t status = B::vm_destroy_region(
                region_.reference());
            if (!committed(status)) {
                return status;
            }
            phase_ = LeasePhase::Closing;
        }
        if (phase_ == LeasePhase::Closing) {
            const myos_status_t status = region_.close();
            if (status != MYOS_STATUS_OK) {
                return status;
            }
            reset_empty();
            phase_ = LeasePhase::Closed;
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto view() const noexcept -> const boot::Bundle* {
        return phase_ == LeasePhase::Mapped && static_cast<bool>(view_)
            ? &view_
            : nullptr;
    }

    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return phase_ == LeasePhase::Mapped ? size_ : 0;
    }

    [[nodiscard]] constexpr auto phase() const noexcept -> LeasePhase {
        return phase_;
    }

private:
    void reset_empty() noexcept {
        root_ = {};
        window_ = {};
        size_ = 0;
        region_ = {};
        view_ = {};
        phase_ = LeasePhase::Empty;
    }

    cap::CapRef root_{};
    Window window_{};
    size_t size_{};
    owner_type region_{};
    boot::Bundle view_{};
    LeasePhase phase_{LeasePhase::Empty};
};

template<typename B>
requires Backend<B>
class ScratchWindow final {
public:
    using owner_type = cap::BasicOwnedCap<B>;

    ScratchWindow() noexcept = default;
    ScratchWindow(const ScratchWindow&) = delete;
    auto operator=(const ScratchWindow&) -> ScratchWindow& = delete;

    ScratchWindow(ScratchWindow&& other) noexcept
        : root_(other.root_),
          window_(other.window_),
          region_(libk::move(other.region_)),
          mapped_size_(other.mapped_size_),
          phase_(other.phase_) {
        other.reset_empty();
    }

    auto operator=(ScratchWindow&& other) noexcept -> ScratchWindow& {
        if (this == &other) {
            return *this;
        }
        const myos_status_t status = close();
        if (status != MYOS_STATUS_OK) {
            B::ownership_fault(status);
        }
        root_ = other.root_;
        window_ = other.window_;
        region_ = libk::move(other.region_);
        mapped_size_ = other.mapped_size_;
        phase_ = other.phase_;
        other.reset_empty();
        return *this;
    }

    ~ScratchWindow() noexcept {
        if (phase_ == LeasePhase::Empty || phase_ == LeasePhase::Closed) {
            return;
        }
        const myos_status_t status = close();
        if (status != MYOS_STATUS_OK) {
            B::ownership_fault(status);
        }
    }

    [[nodiscard]] auto open(
        cap::CapRef root_vspace,
        Window window,
        Window forbidden = {}) noexcept -> myos_status_t {
        if (phase_ != LeasePhase::Empty && phase_ != LeasePhase::Closed) {
            return MYOS_STATUS_BUSY;
        }
        if (!root_vspace || root_vspace.cspace != 0 || !window.valid()
            || !windows_disjoint(window, forbidden)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        reset_empty();
        root_ = root_vspace;
        window_ = window;
        const SysResult created = B::vm_create_region(
            root_,
            window.address,
            window.size,
            MYOS_VM_READ | MYOS_VM_WRITE,
            MYOS_VM_NORMAL,
            MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
        if (created.status != MYOS_STATUS_OK || created.value == 0) {
            reset_empty();
            return created.status == MYOS_STATUS_OK
                ? MYOS_STATUS_INVALID_CAP
                : created.status;
        }
        region_ = owner_type{cap::CapRef{created.value, 0}};
        phase_ = LeasePhase::Ready;
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto map(
        cap::CapRef memory,
        myos_word_t object_page,
        myos_word_t size,
        myos_word_t access) noexcept -> myos_status_t {
        if (phase_ != LeasePhase::Ready || !memory
            || memory.cspace != 0 || size == 0
            || (size % MYOS_DEPLOY_PAGE_SIZE) != 0
            || size > window_.size
            || (access & ~(MYOS_VM_READ | MYOS_VM_WRITE))
                != 0
            || access == 0
            || ((access & MYOS_VM_WRITE) != 0
                && (access & MYOS_VM_READ) == 0)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        const myos_status_t status = B::vm_map(
            region_.reference(), memory, window_.address, size,
            object_page, access);
        if (!committed(status)) {
            return status;
        }
        mapped_size_ = size;
        phase_ = LeasePhase::Mapped;
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto unmap() noexcept -> myos_status_t {
        if (phase_ != LeasePhase::Mapped && phase_ != LeasePhase::Unmapping) {
            return MYOS_STATUS_BAD_ARGS;
        }
        phase_ = LeasePhase::Unmapping;
        const myos_status_t status = B::vm_unmap(
            region_.reference(), window_.address, mapped_size_);
        if (!committed(status)) {
            return status;
        }
        mapped_size_ = 0;
        phase_ = LeasePhase::Ready;
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto close() noexcept -> myos_status_t {
        if (phase_ == LeasePhase::Empty || phase_ == LeasePhase::Closed) {
            return MYOS_STATUS_OK;
        }
        if (phase_ == LeasePhase::Mapped || phase_ == LeasePhase::Unmapping) {
            const myos_status_t status = unmap();
            if (status != MYOS_STATUS_OK) {
                return status;
            }
        }
        if (phase_ == LeasePhase::Ready || phase_ == LeasePhase::Destroying) {
            phase_ = LeasePhase::Destroying;
            const myos_status_t status = B::vm_destroy_region(
                region_.reference());
            if (!committed(status)) {
                return status;
            }
            phase_ = LeasePhase::Closing;
        }
        if (phase_ == LeasePhase::Closing) {
            const myos_status_t status = region_.close();
            if (status != MYOS_STATUS_OK) {
                return status;
            }
            reset_empty();
            phase_ = LeasePhase::Closed;
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] constexpr auto phase() const noexcept -> LeasePhase {
        return phase_;
    }

    [[nodiscard]] constexpr auto mapped() const noexcept -> bool {
        return phase_ == LeasePhase::Mapped;
    }

    [[nodiscard]] constexpr auto reusable() const noexcept -> bool {
        return phase_ == LeasePhase::Ready;
    }

    [[nodiscard]] constexpr auto address() const noexcept -> myos_word_t {
        return phase_ == LeasePhase::Mapped ? window_.address : 0;
    }

private:
    void reset_empty() noexcept {
        root_ = {};
        window_ = {};
        region_ = {};
        mapped_size_ = 0;
        phase_ = LeasePhase::Empty;
    }

    cap::CapRef root_{};
    Window window_{};
    owner_type region_{};
    myos_word_t mapped_size_{};
    LeasePhase phase_{LeasePhase::Empty};
};

} // namespace myos::deploy
