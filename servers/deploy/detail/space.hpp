#pragma once

#include <concepts>
#include <stddef.h>
#include <stdint.h>

#include <array>
#include <utility>
#include <optional>
#include <utility>
#include <uapi/capability.h>
#include <servers/deploy/format.h>
#include <uapi/object.h>
#include <uapi/status.h>
#include <uapi/vm.h>
#include <servers/deploy/bundle.hpp>
#include <sys/handle.hpp>
#include <sys/syscall.hpp>

namespace deploy {

enum class Phase : uint8_t {
    Closed,
    Open,
    Draining,
    ResourceClosing,
    ResourceWaiting,
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

[[nodiscard]] constexpr auto committed(status_t status) noexcept -> bool {
    return status == STATUS_OK || status == STATUS_PENDING;
}

[[nodiscard]] constexpr auto retryable(status_t status) noexcept -> bool {
    return status == STATUS_BUSY || status == STATUS_RETRY;
}

struct Window final {
    word_t address{};
    word_t size{};

    /* Mapping callers use this checked rounding operation before constructing
     * a page-aligned window.  Zero represents overflow or an empty request. */
    [[nodiscard]] static constexpr auto round_size(word_t value) noexcept
        -> word_t {
        constexpr word_t page_size = DEPLOY_PAGE_SIZE;
        return value <= static_cast<word_t>(-1) - (page_size - 1)
            ? (value + page_size - 1) & ~(page_size - 1)
            : 0;
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return address != 0 && size != 0
            && (address % DEPLOY_PAGE_SIZE) == 0
            && (size % DEPLOY_PAGE_SIZE) == 0
            && size <= ~word_t{} - address;
    }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return address == 0 && size == 0;
    }

    [[nodiscard]] constexpr auto end() const noexcept -> word_t {
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
concept Backend = sys::cap::CapBackend<T>
    && requires(
        sys::cap::CapRef pool,
        sys::cap::CapRef vspace,
        sys::cap::CapRef region,
        sys::cap::CapRef memory,
        word_t words,
        word_t address,
        word_t size,
        word_t access,
        word_t types,
        word_t rights) {
    { T::resource_create_child(pool, words, words, words) }
        -> std::same_as<sys::SysResult>;
    { T::resource_close(pool) } -> std::same_as<status_t>;
    { T::vspace_create(pool) } -> std::same_as<sys::SysResult>;
    { T::cspace_create(pool, words, words) } -> std::same_as<sys::SysResult>;
    { T::vm_slice(
          vspace, address, size, access, rights) }
        -> std::same_as<sys::SysResult>;
    { T::vm_map(region, memory, address, size, words, access) }
        -> std::same_as<status_t>;
    { T::vm_unmap(region, address, size) }
        -> std::same_as<status_t>;
    { T::vm_clear(region) } -> std::same_as<status_t>;
};

struct LocalSlot final {
    static constexpr size_t InvalidIndex = static_cast<size_t>(-1);

    cap_t pool{};
    size_t index{};
    obj_kind_t kind{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return pool != 0 && kind > OBJECT_KIND_INVALID
            && kind < OBJECT_KIND_COUNT && ((OBJECT_KINDS >> kind) & 1);
    }

    [[nodiscard]] constexpr auto is_manager() const noexcept -> bool {
        return valid() && index == InvalidIndex;
    }
};

template<size_t LocalCapacity, size_t RemoteCapacity, typename B = sys::cap::SyscallBackend>
requires Backend<B>
class TaskSpace final {
public:
    using backend_type = B;
    using owner_type = sys::cap::BasicOwnedCap<B>;

    TaskSpace() noexcept = default;
    TaskSpace(const TaskSpace&) = delete;
    auto operator=(const TaskSpace&) -> TaskSpace& = delete;

    TaskSpace(TaskSpace&& other) noexcept
        : pool_(std::move(other.pool_)),
          local_(std::move(other.local_)), manager_(std::move(other.manager_)),
          remote_(std::move(other.remote_)),
          local_count_(std::exchange(other.local_count_, 0)),
          remote_count_(std::exchange(other.remote_count_, 0)),
          phase_(other.phase_),
          close_events_(other.close_events_), close_badge_(other.close_badge_),
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
            B::ownership_fault(STATUS_BUSY);
        }
        pool_ = std::move(other.pool_);
        local_ = std::move(other.local_);
        manager_ = std::move(other.manager_);
        remote_ = std::move(other.remote_);
        local_count_ = std::exchange(other.local_count_, 0);
        remote_count_ = std::exchange(other.remote_count_, 0);
        phase_ = other.phase_;
        close_events_ = other.close_events_;
        close_badge_ = other.close_badge_;
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
            B::ownership_fault(STATUS_BUSY);
        }
        const status_t status = pool_.close();
        if (status != STATUS_OK) {
            B::ownership_fault(status);
        }
        phase_ = Phase::Closed;
    }

    // The factory is caller-owned and one-shot.  A failed child creation does
    // not install any ownership; later failures leave a strong-closeable
    // aggregate whose caller may retry close().
    [[nodiscard]] auto open(
        sys::cap::CapRef parent_pool,
        word_t memory,
        word_t caps,
        word_t kinds,
        word_t cspace_slots,
        word_t cspace_pages) noexcept -> status_t {
        if (initialized_ || phase_ != Phase::Closed) {
            return STATUS_BUSY;
        }
        if (!parent_pool || parent_pool.cspace != 0 || memory == 0
            || caps == 0 || kinds == 0 || cspace_slots == 0
            || cspace_pages == 0) {
            return STATUS_BAD_ARGS;
        }

        const sys::SysResult child = B::resource_create_child(
            parent_pool, memory, caps, kinds);
        if (child.status != STATUS_OK || child.value == 0) {
            return child.status == STATUS_OK
                ? STATUS_INVALID_CAP
                : child.status;
        }
        pool_ = owner_type{sys::cap::CapRef{child.value, 0}};
        initialized_ = true;
        phase_ = Phase::Open;

        const auto fail = [&](status_t status) noexcept {
            static_cast<void>(close());
            return status;
        };

        const sys::SysResult vspace = B::vspace_create(pool_.reference());
        if (vspace.status != STATUS_OK || vspace.value == 0) {
            return fail(vspace.status == STATUS_OK
                ? STATUS_INVALID_CAP
                : vspace.status);
        }
        owner_type vspace_owner{sys::cap::CapRef{vspace.value, 0}};
        const auto vspace_slot = adopt_local(
            std::move(vspace_owner), OBJECT_KIND_VSPACE);
        if (!vspace_slot) {
            return fail(STATUS_NO_MEMORY);
        }
        vspace_slot_ = *vspace_slot;

        const sys::SysResult cspace = B::cspace_create(
            pool_.reference(), cspace_slots, cspace_pages);
        if (cspace.status != STATUS_OK || cspace.value == 0) {
            return fail(cspace.status == STATUS_OK
                ? STATUS_INVALID_CAP
                : cspace.status);
        }
        owner_type manager_owner{sys::cap::CapRef{cspace.value, 0}};
        manager_ = std::move(manager_owner);
        manager_slot_ = LocalSlot{
            .pool = pool_.selector(),
            .index = LocalSlot::InvalidIndex,
            .kind = OBJECT_KIND_CSPACE};
        return STATUS_OK;
    }

    [[nodiscard]] auto close() noexcept -> status_t {
        if (!initialized_ || phase_ == Phase::Closed) {
            return STATUS_OK;
        }
        if (phase_ == Phase::Open) {
            phase_ = Phase::Draining;
        }
        if (phase_ == Phase::Draining) {
            // Remote selectors require the manager CSpace until their last close.
            for (size_t i = remote_count_; i != 0; --i) {
                const auto status = remote_[i - 1].close();
                if (status != STATUS_OK) return status;
            }
            const auto status = manager_.close();
            if (status != STATUS_OK) return status;
            for (size_t i = local_count_; i != 0; --i) {
                const auto status = local_[i - 1].cap.close();
                if (status != STATUS_OK) return status;
            }
            local_count_ = remote_count_ = 0;
            phase_ = Phase::ResourceClosing;
        }
        if (phase_ == Phase::ResourceClosing) {
            if (!pool_) {
                phase_ = Phase::ResourceClosed;
            } else {
                if constexpr (requires { B::resource_close_async(pool_.reference(), close_events_, close_badge_); }) {
                    if (close_events_) {
                        const auto status = B::resource_close_async(pool_.reference(), close_events_, close_badge_);
                        if (status != STATUS_OK) return status;
                        phase_ = Phase::ResourceWaiting;
                        return STATUS_BUSY;
                    }
                }
                const status_t status = B::resource_close(
                    pool_.reference());
                if (status != STATUS_OK) {
                    return status;
                }
                phase_ = Phase::ResourceClosed;
            }
        }
        if (phase_ == Phase::ResourceWaiting) return STATUS_BUSY;
        if (phase_ == Phase::ResourceClosed) {
            const status_t status = pool_.close();
            if (status != STATUS_OK) {
                return status;
            }
            phase_ = Phase::Closed;
            return STATUS_OK;
        }
        return STATUS_INTERNAL;
    }

    [[nodiscard]] constexpr auto phase() const noexcept -> Phase {
        return phase_;
    }

    [[nodiscard]] auto pool() const noexcept -> std::optional<sys::cap::CapRef> {
        if (!pool_) {
            return std::nullopt;
        }
        return pool_.reference();
    }

    void close_events(sys::cap::CapRef events, word_t badge) noexcept {
        if (phase_ != Phase::Open || !events || badge == 0) B::ownership_fault(STATUS_BAD_ARGS);
        close_events_ = events;
        close_badge_ = badge;
    }
    void observe_close(word_t badges) noexcept {
        if (phase_ == Phase::ResourceWaiting && (badges & close_badge_) != 0)
            phase_ = Phase::ResourceClosed;
    }

    [[nodiscard]] constexpr auto vspace_slot() const noexcept -> LocalSlot {
        return vspace_slot_;
    }

    [[nodiscard]] constexpr auto manager_slot() const noexcept -> LocalSlot {
        return manager_slot_;
    }

    [[nodiscard]] auto adopt_local(
        owner_type&& owner, obj_kind_t kind) noexcept -> std::optional<LocalSlot> {
        if (phase_ != Phase::Open || !pool_ || !owner || owner.cspace() != 0
            || !valid_kind(kind) || local_count_ == LocalCapacity) return std::nullopt;
        const auto index = local_count_++;
        local_[index] = {std::move(owner), kind};
        return LocalSlot{.pool = pool_.selector(), .index = index, .kind = kind};
    }

    [[nodiscard]] auto adopt_remote(owner_type&& owner) noexcept -> bool {
        return adopt_remote_index(std::move(owner)).has_value();
    }

    [[nodiscard]] auto adopt_remote_index(owner_type&& owner) noexcept -> std::optional<size_t> {
        if (!can_adopt_remote(owner)) return std::nullopt;
        const auto index = remote_count_++;
        remote_[index] = std::move(owner);
        return index;
    }

    [[nodiscard]] auto close_remote(size_t index) noexcept -> status_t {
        if (phase_ != Phase::Open) return STATUS_CLOSED;
        return index < remote_count_ ? remote_[index].close() : STATUS_INVALID_CAP;
    }

    [[nodiscard]] auto can_adopt_remote() const noexcept -> bool {
        return phase_ == Phase::Open && manager_ && remote_count_ < RemoteCapacity;
    }

    [[nodiscard]] auto can_adopt_remote(const owner_type& owner) const noexcept -> bool {
        return can_adopt_remote() && owner && owner.cspace() == manager_.selector();
    }

    [[nodiscard]] auto lookup(
        LocalSlot slot,
        obj_kind_t expected_kind) const noexcept
        -> std::optional<sys::cap::CapRef> {
        if (phase_ != Phase::Open || !slot.valid()
            || slot.pool != pool_.selector() || slot.kind != expected_kind) {
            return std::nullopt;
        }
        if (slot.is_manager()) {
            if (slot.kind != OBJECT_KIND_CSPACE) {
                return std::nullopt;
            }
            if (!manager_) return std::nullopt;
            return manager_.reference();
        }
        if (slot.index >= local_count_
            || local_[slot.index].kind != expected_kind || !local_[slot.index].cap) {
            return std::nullopt;
        }
        return local_[slot.index].cap.reference();
    }

    [[nodiscard]] auto lookup_remote(
        size_t index,
        cap_t manager) const noexcept
        -> std::optional<sys::cap::CapRef> {
        if (phase_ != Phase::Open || manager == 0) {
            return std::nullopt;
        }
        if (!manager_ || manager_.selector() != manager || index >= remote_count_ || !remote_[index])
            return std::nullopt;
        return remote_[index].reference();
    }

    [[nodiscard]] auto close_slot(LocalSlot slot) noexcept -> status_t {
        if (phase_ != Phase::Open || !slot.valid()
            || slot.pool != pool_.selector()) {
            return STATUS_INVALID_CAP;
        }
        if (slot.is_manager()) {
            return STATUS_BAD_RIGHTS;
        }
        if (slot.index >= local_count_
            || local_[slot.index].kind != slot.kind) {
            return STATUS_INVALID_CAP;
        }
        return local_[slot.index].cap.close();
    }

    [[nodiscard]] constexpr auto local_cumulative() const noexcept -> size_t {
        return local_count_;
    }

    [[nodiscard]] static constexpr auto local_capacity() noexcept -> size_t {
        return LocalCapacity;
    }

    [[nodiscard]] static constexpr auto remote_capacity() noexcept -> size_t {
        return RemoteCapacity;
    }

    [[nodiscard]] constexpr auto remote_size() const noexcept -> size_t {
        return remote_count_;
    }

    [[nodiscard]] auto remote_live_size() const noexcept -> size_t {
        size_t count{};
        for (size_t i = 0; i < remote_count_; ++i) count += bool(remote_[i]);
        return count;
    }

private:
    [[nodiscard]] static constexpr auto valid_kind(
        obj_kind_t kind) noexcept -> bool {
        return kind > OBJECT_KIND_INVALID
            && kind < OBJECT_KIND_COUNT && ((OBJECT_KINDS >> kind) & 1);
    }

    struct entry { owner_type cap; obj_kind_t kind{}; };
    // Declaration order keeps remote owners before manager/local/pool destruction.
    owner_type pool_{};
    std::array<entry, LocalCapacity> local_{};
    owner_type manager_{};
    std::array<owner_type, RemoteCapacity> remote_{};
    size_t local_count_{}, remote_count_{};
    Phase phase_{Phase::Closed};
    sys::cap::CapRef close_events_{};
    word_t close_badge_{};
    bool initialized_{};
    LocalSlot vspace_slot_{};
    LocalSlot manager_slot_{};
};

template<typename B = sys::cap::SyscallBackend>
requires Backend<B>
class MappedBundle final {
public:
    using owner_type = sys::cap::BasicOwnedCap<B>;

    MappedBundle() noexcept = default;
    MappedBundle(const MappedBundle&) = delete;
    auto operator=(const MappedBundle&) -> MappedBundle& = delete;

    MappedBundle(MappedBundle&& other) noexcept
        : root_(other.root_),
          window_(other.window_),
          size_(other.size_),
          region_(std::move(other.region_)),
          view_(other.view_),
          phase_(other.phase_) {
        other.reset_empty();
    }

    auto operator=(MappedBundle&& other) noexcept -> MappedBundle& {
        if (this == &other) {
            return *this;
        }
        const status_t status = close();
        if (status != STATUS_OK) {
            B::ownership_fault(status);
        }
        root_ = other.root_;
        window_ = other.window_;
        size_ = other.size_;
        region_ = std::move(other.region_);
        view_ = other.view_;
        phase_ = other.phase_;
        other.reset_empty();
        return *this;
    }

    ~MappedBundle() noexcept {
        if (phase_ == LeasePhase::Empty || phase_ == LeasePhase::Closed) {
            return;
        }
        const status_t status = close();
        if (status != STATUS_OK) {
            B::ownership_fault(status);
        }
    }

    [[nodiscard]] auto open(
        sys::cap::CapRef root_vspace,
        sys::cap::CapRef bundle_memory,
        Window window,
        size_t bundle_size,
        Window forbidden = {}) noexcept -> status_t {
        if (phase_ != LeasePhase::Empty && phase_ != LeasePhase::Closed) {
            return STATUS_BUSY;
        }
        if (!root_vspace || root_vspace.cspace != 0 || !bundle_memory
            || bundle_memory.cspace != 0 || !window.valid()
            || bundle_size == 0 || bundle_size > window.size
            || !windows_disjoint(window, forbidden)) {
            return STATUS_BAD_ARGS;
        }
        reset_empty();
        root_ = root_vspace;
        window_ = window;
        size_ = bundle_size;
        const sys::SysResult created = B::vm_slice(
            root_,
            window.address,
            window.size,
            VM_READ,
            RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
        if (created.status != STATUS_OK || created.value == 0) {
            reset_empty();
            return created.status == STATUS_OK
                ? STATUS_INVALID_CAP
                : created.status;
        }
        region_ = owner_type{sys::cap::CapRef{created.value, 0}};
        phase_ = LeasePhase::Ready;
        const status_t mapped = B::vm_map(
            region_.reference(),
            bundle_memory,
            window.address,
            window.size,
            0,
            VM_READ);
        if (!committed(mapped)) {
            return mapped;
        }
        phase_ = LeasePhase::Mapped;
        view_ = boot::Bundle::parse(
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(window.address)),
            bundle_size);
        return static_cast<bool>(view_) ? STATUS_OK : STATUS_BAD_ARGS;
    }

    [[nodiscard]] auto close() noexcept -> status_t {
        if (phase_ == LeasePhase::Empty || phase_ == LeasePhase::Closed) {
            return STATUS_OK;
        }
        view_ = {};
        if (phase_ == LeasePhase::Mapped || phase_ == LeasePhase::Unmapping) {
            phase_ = LeasePhase::Unmapping;
            const status_t status = B::vm_unmap(
                region_.reference(), window_.address, window_.size);
            if (!committed(status)) {
                return status;
            }
            phase_ = LeasePhase::Ready;
        }
        if (phase_ == LeasePhase::Ready || phase_ == LeasePhase::Destroying) {
            phase_ = LeasePhase::Destroying;
            const status_t status = B::vm_clear(
                region_.reference());
            if (!committed(status)) {
                return status;
            }
            phase_ = LeasePhase::Closing;
        }
        if (phase_ == LeasePhase::Closing) {
            const status_t status = region_.close();
            if (status != STATUS_OK) {
                return status;
            }
            reset_empty();
            phase_ = LeasePhase::Closed;
        }
        return STATUS_OK;
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

    sys::cap::CapRef root_{};
    Window window_{};
    size_t size_{};
    owner_type region_{};
    boot::Bundle view_{};
    LeasePhase phase_{LeasePhase::Empty};
};

template<typename B = sys::cap::SyscallBackend>
requires Backend<B>
class ScratchWindow final {
public:
    using owner_type = sys::cap::BasicOwnedCap<B>;

    ScratchWindow() noexcept = default;
    ScratchWindow(const ScratchWindow&) = delete;
    auto operator=(const ScratchWindow&) -> ScratchWindow& = delete;

    ScratchWindow(ScratchWindow&& other) noexcept
        : root_(other.root_),
          window_(other.window_),
          region_(std::move(other.region_)),
          mapped_size_(other.mapped_size_),
          phase_(other.phase_) {
        other.reset_empty();
    }

    auto operator=(ScratchWindow&& other) noexcept -> ScratchWindow& {
        if (this == &other) {
            return *this;
        }
        const status_t status = close();
        if (status != STATUS_OK) {
            B::ownership_fault(status);
        }
        root_ = other.root_;
        window_ = other.window_;
        region_ = std::move(other.region_);
        mapped_size_ = other.mapped_size_;
        phase_ = other.phase_;
        other.reset_empty();
        return *this;
    }

    ~ScratchWindow() noexcept {
        if (phase_ == LeasePhase::Empty || phase_ == LeasePhase::Closed) {
            return;
        }
        const status_t status = close();
        if (status != STATUS_OK) {
            B::ownership_fault(status);
        }
    }

    [[nodiscard]] auto open(
        sys::cap::CapRef root_vspace,
        Window window,
        Window forbidden = {}) noexcept -> status_t {
        if (phase_ != LeasePhase::Empty && phase_ != LeasePhase::Closed) {
            return STATUS_BUSY;
        }
        if (!root_vspace || root_vspace.cspace != 0 || !window.valid()
            || !windows_disjoint(window, forbidden)) {
            return STATUS_BAD_ARGS;
        }
        reset_empty();
        root_ = root_vspace;
        window_ = window;
        const sys::SysResult created = B::vm_slice(
            root_,
            window.address,
            window.size,
            VM_READ | VM_WRITE,
            RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
        if (created.status != STATUS_OK || created.value == 0) {
            reset_empty();
            return created.status == STATUS_OK
                ? STATUS_INVALID_CAP
                : created.status;
        }
        region_ = owner_type{sys::cap::CapRef{created.value, 0}};
        phase_ = LeasePhase::Ready;
        return STATUS_OK;
    }

    [[nodiscard]] auto map(
        sys::cap::CapRef memory,
        word_t object_page,
        word_t size,
        word_t access) noexcept -> status_t {
        if (phase_ != LeasePhase::Ready || !memory
            || memory.cspace != 0 || size == 0
            || (size % DEPLOY_PAGE_SIZE) != 0
            || size > window_.size
            || (access & ~(VM_READ | VM_WRITE))
                != 0
            || access == 0
            || ((access & VM_WRITE) != 0
                && (access & VM_READ) == 0)) {
            return STATUS_BAD_ARGS;
        }
        const status_t status = B::vm_map(
            region_.reference(), memory, window_.address, size,
            object_page, access);
        if (!committed(status)) {
            return status;
        }
        mapped_size_ = size;
        phase_ = LeasePhase::Mapped;
        return STATUS_OK;
    }

    [[nodiscard]] auto unmap() noexcept -> status_t {
        if (phase_ != LeasePhase::Mapped && phase_ != LeasePhase::Unmapping) {
            return STATUS_BAD_ARGS;
        }
        phase_ = LeasePhase::Unmapping;
        const status_t status = B::vm_unmap(
            region_.reference(), window_.address, mapped_size_);
        if (!committed(status)) {
            return status;
        }
        mapped_size_ = 0;
        phase_ = LeasePhase::Ready;
        return STATUS_OK;
    }

    [[nodiscard]] auto close() noexcept -> status_t {
        if (phase_ == LeasePhase::Empty || phase_ == LeasePhase::Closed) {
            return STATUS_OK;
        }
        if (phase_ == LeasePhase::Mapped || phase_ == LeasePhase::Unmapping) {
            const status_t status = unmap();
            if (status != STATUS_OK) {
                return status;
            }
        }
        if (phase_ == LeasePhase::Ready || phase_ == LeasePhase::Destroying) {
            phase_ = LeasePhase::Destroying;
            const status_t status = B::vm_clear(
                region_.reference());
            if (!committed(status)) {
                return status;
            }
            phase_ = LeasePhase::Closing;
        }
        if (phase_ == LeasePhase::Closing) {
            const status_t status = region_.close();
            if (status != STATUS_OK) {
                return status;
            }
            reset_empty();
            phase_ = LeasePhase::Closed;
        }
        return STATUS_OK;
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

    [[nodiscard]] constexpr auto address() const noexcept -> word_t {
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

    sys::cap::CapRef root_{};
    Window window_{};
    owner_type region_{};
    word_t mapped_size_{};
    LeasePhase phase_{LeasePhase::Empty};
};

} // namespace deploy
