#pragma once

#include <cap/resolved.hpp>
#include <io/device.hpp>
#include <irq/irq.hpp>
#include <mm/memory_object.hpp>
#include <object/object_cleanup.hpp>

namespace kernel::object { class ObjectStore; }

namespace kernel::io {

class Executor;
enum class SpaceState : u8 { Empty, Binding, Opening, Active, Closing, Closed, Failed };
enum class SpaceError : u8 { InvalidState, InvalidAuthority, InvalidRange, Busy,
    UnsupportedMemory, BackingUnavailable, OutOfMemory, QuotaExceeded, Cancelled };
struct DeviceInfo final {
    libk::Array<u32, 64> configuration{};
    libk::Array<usize, 6> bar_sizes{};
};

// One immutable DMA arena and one exclusive device generation. Source
// attachments revoke the binding; page pins survive through hardware drain.
class Space final : private libk::noncopyable_nonmovable {
public:
    Space(mm::Pmm& pmm, Executor& executor, object::ObjectStore& objects,
        cap::GrantGraph& grants) noexcept;
    ~Space() noexcept;

    [[nodiscard]] auto bind(object::ObjectRef self,
        cap::Resolved<Device>& device, cap::Resolved<mm::MemoryObject>& memory,
        mm::ObjectRange range, usize first) noexcept
        -> libk::Expected<void, SpaceError>;
    [[nodiscard]] auto state() const noexcept -> SpaceState;
    [[nodiscard]] auto bar(usize index) noexcept
        -> libk::Expected<cap::GrantRef, SpaceError>;
    [[nodiscard]] auto interrupt() noexcept
        -> libk::Expected<cap::GrantRef, SpaceError>;
    [[nodiscard]] auto info() const noexcept -> libk::Expected<DeviceInfo, SpaceError>;
    void close() noexcept;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

private:
    friend class Executor;
    friend struct object::ObjectTraits<Space>;
    struct Pins final {
        static constexpr usize Capacity =
            (mm::page_size - sizeof(Pins*) - sizeof(usize)) / sizeof(mm::PageLease);
        Pins* next{};
        usize count{};
        mm::PageLease pages[Capacity]{};
    };
    static_assert(sizeof(Pins) <= mm::page_size);
    struct Completion final {
        object::ObjectRef self{};
        object::ObjectCleanup cleanup{};
        bool more{};
    };
    [[nodiscard]] auto prepare(cap::Resolved<Device>& device,
        cap::Resolved<mm::MemoryObject>& memory, mm::ObjectRange range,
        usize first, usize tables) noexcept -> libk::Expected<arch::IoRoot, SpaceError>;
    [[nodiscard]] auto service() noexcept -> Completion;
    [[nodiscard]] auto prepare_bars() noexcept -> libk::Expected<void, SpaceError>;
    [[nodiscard]] auto retire_bars() noexcept -> bool;
    [[nodiscard]] auto prepare_interrupt() noexcept -> libk::Expected<void, SpaceError>;
    [[nodiscard]] auto retire_interrupt() noexcept -> bool;
    void free_pages() noexcept;
    void bind_sponsor(resource::Sponsorship& sponsor) noexcept { sponsor_ = &sponsor; }
    static void stop_device(void* context) noexcept;
    static void invalidate_memory(void*, mm::MemoryWork&&, mm::MemoryInvalidation) noexcept;
    static void invalidate_device_grant(void*, cap::GrantWork&&, cap::GrantInvalidation) noexcept;
    static void invalidate_memory_grant(void*, cap::GrantWork&&, cap::GrantInvalidation) noexcept;
    // Service takes work under lock and releases it outside the lock;
    // callbacks never release their own token. No callback may touch Space
    // after publishing and unlocking.
    static void released(void*) noexcept {}
    inline static const mm::MemoryAttachmentOps memory_ops{invalidate_memory, released};
    inline static const cap::GrantAttachmentOps device_ops{invalidate_device_grant, released};
    inline static const cap::GrantAttachmentOps grant_ops{invalidate_memory_grant, released};

    mm::Pmm& pmm_;
    Executor& executor_;
    object::ObjectStore& objects_;
    cap::GrantGraph& grants_;
    mutable sync::SpinLock<sync::LockClass::IoSpace> lock_{};
    SpaceState state_{SpaceState::Empty};
    libk::Atomic<bool> closing_{};
    object::ObjectRef self_{};
    object::ObjectCleanup cleanup_{};
    object::ObjectHold<Device> device_{};
    object::ObjectHold<mm::MemoryObject> memory_{};
    libk::optional<DeviceLease> lease_{};
    struct Bar final {
        object::ObjectHold<mm::MemoryObject> memory{};
        cap::GrantRef grant{};
        cap::GrantRevoke revoke{};
    };
    libk::Array<Bar, 6> bars_{};
    object::ObjectHold<irq::Irq> interrupt_{};
    cap::GrantRef interrupt_grant_{};
    cap::GrantRevoke interrupt_revoke_{};
    mm::MemoryAttachment memory_attachment_{this, memory_ops};
    cap::GrantAttachment device_grant_{this, device_ops};
    cap::GrantAttachment memory_grant_{this, grant_ops};
    mm::MemoryWork memory_work_{};
    cap::GrantWork device_work_{};
    cap::GrantWork grant_work_{};
    resource::Sponsorship* sponsor_{};
    resource::Charge charge_{};
    mm::OwnedPageGroup metadata_{};
    Pins* pins_{};
    libk::IntrusiveListHook work_hook_{};
    bool work_open_{}; // protected by Executor lock
};

// Runtime has one service runner. The queue owns no authority: each admitted
// Space retains its structural self-reference until withdrawal is complete.
class Executor final : private libk::noncopyable_nonmovable {
public:
    using Notifier = libk::delegate<diag::concurrency::ObservationKey() noexcept>;
    ~Executor() noexcept;
    void submit(Space& space) noexcept;
    [[nodiscard]] auto run(usize budget) noexcept -> bool;
    void bind_notifier(Notifier notifier) noexcept;
    void unbind_notifier() noexcept;
private:
    friend class Space;
    void open(Space& space) noexcept;
    void withdraw(Space& space) noexcept;
    [[nodiscard]] auto take() noexcept -> Space*;
    mutable sync::SpinLock<sync::LockClass::IoWork> lock_{};
    libk::IntrusiveList<Space, &Space::work_hook_> queue_{};
    Notifier notifier_{};
};

} // namespace kernel::io
