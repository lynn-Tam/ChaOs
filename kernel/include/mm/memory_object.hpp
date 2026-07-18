#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/span.hpp>
#include <libk/sync/atomic.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <mm/pmm.hpp>
#include <mm/object_range.hpp>
#include <mm/permissions.hpp>
#include <resource/sponsorship.hpp>

namespace kernel::object {
template<typename T>
struct ObjectTraits;
}

namespace kernel::mm {

struct MemoryExtent final {
    ObjectRange object{};
    PageRange physical{};
    AccessMask access{};
    MemoryType type{MemoryType::Normal};
};

struct AnonymousConfig final {
    AccessMask access{AccessMask::of(Access::Read, Access::Write)};
    bool eager{};
};

enum class BootOwnership : u8 {
    Borrowed,
    Owned,
};

enum class BackingKind : u8 {
    Anonymous,
    Physical,
    BootImage,
};

enum class ContentState : u8 {
    Zero,
    Resident,
    Busy,
    Failed,
};

struct MemoryPage final {
    Page page{};
    AccessMask access{};
    MemoryType type{MemoryType::Normal};
};

enum class MemoryError : u8 {
    InvalidSize,
    InvalidRange,
    InvalidAccess,
    InvalidMemoryType,
    InvalidState,
    OutOfMemory,
    ResourceExhausted,
    GenerationExhausted,
    Busy,
    BackingFailed,
    NotBacked,
    AttachmentState,
    OwnershipMismatch,
};

enum class MemoryInvalidation : u8 {
    Destroy,
};

enum class MemoryState : u8 {
    Building,
    Live,
    Stopping,
    Retired,
};

enum class SealState : u8 {
    Loadable,
    Sealing,
    Executable,
};

struct ContentEpoch final {
    u64 raw{};

    [[nodiscard]] friend constexpr auto operator==(
        ContentEpoch, ContentEpoch) noexcept -> bool = default;
};

class MemoryObject;
class MemoryAttachment;

class MemoryWork final : private libk::noncopyable {
public:
    MemoryWork() noexcept = default;
    MemoryWork(MemoryWork&& other) noexcept;
    auto operator=(MemoryWork&& other) noexcept -> MemoryWork&;
    ~MemoryWork() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return attachment_ != nullptr;
    }
    void reset() noexcept;

private:
    friend class MemoryObject;
    explicit MemoryWork(MemoryAttachment& attachment) noexcept
        : attachment_(&attachment) {}

    MemoryAttachment* attachment_{};
};

struct MemoryAttachmentOps final {
    void (*invalidate)(
        void* context,
        MemoryWork&& work,
        MemoryInvalidation reason) noexcept;
    void (*released)(void* context) noexcept;
};

// Embedded in a Mapping and indexed non-owningly by MemoryObject. The Mapping
// must retain its structural MemoryObject reference until detach() completes.
class MemoryAttachment final : private libk::noncopyable_nonmovable {
public:
    MemoryAttachment(
        void* context,
        const MemoryAttachmentOps& ops) noexcept
        : context_(context), ops_(&ops) {}
    ~MemoryAttachment() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto busy() const noexcept -> bool;
    // Returns true when no MemoryWork still pins the Mapping relation.
    [[nodiscard]] auto detach() noexcept -> bool;

private:
    friend class MemoryObject;
    friend class MemoryWork;

    enum class State : u8 {
        Idle,
        Attached,
        Invalidating,
        Detached,
    };

    void drop_work() noexcept;

    libk::IntrusiveListHook memory_hook_{};
    MemoryObject* owner_{};
    void* context_{};
    const MemoryAttachmentOps* ops_{};
    libk::Atomic<usize> work_{};
    libk::Atomic<u8> state_{static_cast<u8>(State::Idle)};
    AccessMask access_{};
};

// Short backing borrow. Callers keep the MemoryObject operation pin or a
// structural Mapping reference alive for the complete lease lifetime.
class PageLease final : private libk::noncopyable {
public:
    PageLease() noexcept = default;
    PageLease(PageLease&& other) noexcept;
    auto operator=(PageLease&& other) noexcept -> PageLease&;
    ~PageLease() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }
    [[nodiscard]] auto page() const noexcept -> MemoryPage { return page_; }
    void reset() noexcept;

private:
    friend class MemoryObject;
    PageLease(MemoryObject& owner, MemoryPage page) noexcept
        : owner_(&owner), page_(page) {}

    MemoryObject* owner_{};
    MemoryPage page_{};
};

class MemoryObject final : private libk::noncopyable_nonmovable {
public:
    MemoryObject(Pmm& pmm, usize byte_size) noexcept;
    ~MemoryObject() noexcept;

    [[nodiscard]] auto initialize_anonymous(AnonymousConfig config) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto initialize_physical(
        libk::Span<const MemoryExtent> extents) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto initialize_boot_image(
        libk::Span<const MemoryExtent> extents,
        BootOwnership ownership,
        OwnedPageGroup&& owned = {}) noexcept
        -> libk::Expected<void, MemoryError>;

    [[nodiscard]] auto size() const noexcept -> usize {
        return logical_pages_ * page_size;
    }
    [[nodiscard]] auto page_count() const noexcept -> usize {
        return logical_pages_;
    }
    [[nodiscard]] auto kind() const noexcept -> BackingKind;
    [[nodiscard]] auto state() const noexcept -> MemoryState;
    [[nodiscard]] auto seal_state() const noexcept -> SealState;
    [[nodiscard]] auto content_epoch() const noexcept -> ContentEpoch;
    // Publishes immutable executable content. The synchronous E0 path only
    // succeeds once no writable mapping attachment remains; later async
    // callers use the same state transition after retiring those mappings.
    [[nodiscard]] auto seal() noexcept -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto query(usize page_index) const noexcept
        -> libk::Expected<ContentState, MemoryError>;
    [[nodiscard]] auto materialize(usize page_index) noexcept
        -> libk::Expected<PageLease, MemoryError>;
    [[nodiscard]] auto read(usize offset, libk::Span<byte> output) noexcept
        -> libk::Expected<void, MemoryError>;

    [[nodiscard]] auto attach(
        MemoryAttachment& attachment,
        AccessMask access) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto attachment_count() const noexcept -> usize;
    void retire() noexcept;

private:
    friend struct kernel::object::ObjectTraits<MemoryObject>;
    friend class PageLease;
    friend class MemoryAttachment;

    using AttachmentList = libk::IntrusiveList<
        MemoryAttachment,
        &MemoryAttachment::memory_hook_>;

    struct BackingOps final {
        BackingKind kind;
        ContentState (*query)(const void* backing, usize page_index) noexcept;
        libk::Expected<MemoryPage, MemoryError> (*materialize)(
            void* backing,
            usize page_index) noexcept;
        void (*destroy)(void* backing) noexcept;
    };

    [[nodiscard]] auto initialize_backing(
        BackingKind kind,
        libk::Span<const MemoryExtent> extents,
        AnonymousConfig anonymous,
        BootOwnership boot_ownership,
        OwnedPageGroup&& boot_pages) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto detach(MemoryAttachment& attachment) noexcept -> bool;
    void drop_page() noexcept;
    void finish_retire() noexcept;
    void fail_build() noexcept;
    void bind_sponsor(kernel::resource::Sponsorship& sponsor) noexcept;
    [[nodiscard]] auto reserve_dynamic(kernel::resource::Budget charge) noexcept
        -> libk::Expected<kernel::resource::Reservation, MemoryError>;

    Pmm* pmm_{};
    usize logical_pages_{};
    mutable libk::TicketSpinLock lock_{};
    AttachmentList attachments_{};
    void* backing_{};
    const BackingOps* backing_ops_{};
    OwnedPage backing_page_{};
    kernel::resource::Sponsorship backing_sponsorship_{};
    usize operations_{};
    MemoryState state_{MemoryState::Building};
    SealState seal_{SealState::Loadable};
    ContentEpoch content_epoch_{};
    AccessMask access_{};
    bool releasing_{};
    kernel::resource::Sponsorship* sponsor_{};
};

} // namespace kernel::mm
