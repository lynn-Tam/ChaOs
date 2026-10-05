#pragma once

#include <expected>
#include <optional>


#include <array>
#include <cap/grant.hpp>
#include <io/device.hpp>
#include <irq/irq.hpp>
#include <mm/mem.hpp>
#include <object/ref.hpp>

namespace object { template<class> class pool; }
namespace mm { class objects; }

namespace io {

class Executor;
enum class SpaceState : u8 { Empty, Binding, Opening, Active, Closing, Closed, Failed, Faulted };
enum class SpaceError : u8 { InvalidState, Denied, InvalidRange, Busy,
    UnsupportedMemory, BackingUnavailable, OutOfMemory, QuotaExceeded, Cancelled };

// One immutable DMA arena and one exclusive device generation. Source
// attachments revoke the binding; page pins survive through hardware drain.
class Space final : private libk::noncopyable_nonmovable {
public:
    Space(mm::Pmm& pmm, Executor& executor, object::pool<irq::Irq>& irqs, object::pool<mm::Mem>& memory,
        cap::GrantGraph& grants) noexcept;
    ~Space() noexcept;

    [[nodiscard]] auto bind(object::ref<> self,
        cap::Resolved<Device>& device, cap::Resolved<mm::Mem>& memory,
        mm::ObjectRange range, usize first) noexcept
        -> std::expected<void, SpaceError>;
    [[nodiscard]] auto watch(ipc::Notification& notification, u64 badge) noexcept
        -> std::expected<void, SpaceError>;
    [[nodiscard]] auto state() const noexcept -> SpaceState;
    [[nodiscard]] auto bar(usize index) noexcept
        -> std::expected<cap::GrantRef, SpaceError>;
    [[nodiscard]] auto interrupt() noexcept
        -> std::expected<cap::GrantRef, SpaceError>;
    [[nodiscard]] auto info() const noexcept -> std::expected<DeviceInfo, SpaceError>;
    void close() noexcept;
    void retire(object::cleanup&& cleanup) noexcept;

private:
    friend class Executor;
    friend struct object::traits<Space>;
    struct Pins final {
        static constexpr usize Capacity =
            (mm::page_size - sizeof(Pins*) - sizeof(usize)) / sizeof(mm::PageHold);
        Pins* next{};
        usize count{};
        mm::PageHold pages[Capacity]{};
    };
    static_assert(sizeof(Pins) <= mm::page_size);
    struct Completion final {
        object::ref<> self{};
        object::cleanup cleanup{};
        bool more{};
    };
    [[nodiscard]] auto prepare(cap::Resolved<Device>& device,
        cap::Resolved<mm::Mem>& memory, mm::ObjectRange range,
        usize first, usize tables) noexcept -> std::expected<mm::PageTable, SpaceError>;
    [[nodiscard]] auto service() noexcept -> Completion;
    [[nodiscard]] auto prepare_bars() noexcept -> std::expected<void, SpaceError>;
    [[nodiscard]] auto retire_bars() noexcept -> bool;
    [[nodiscard]] auto prepare_interrupt() noexcept -> std::expected<void, SpaceError>;
    [[nodiscard]] auto retire_interrupt() noexcept -> bool;
    void free_pages() noexcept;
    void bind_sponsor(resource::Sponsorship& sponsor) noexcept { sponsor_ = &sponsor; }
    void stop_device(bool fault) noexcept;
    static void invalidate_memory(void*, mm::MemWork&&) noexcept;
    static void invalidate_device_grant(void*, cap::GrantWork&&, cap::GrantInvalidation) noexcept;
    static void invalidate_memory_grant(void*, cap::GrantWork&&, cap::GrantInvalidation) noexcept;
    // Service takes work under lock and releases it outside the lock;
    // callbacks never release their own token. No callback may touch Space
    // after publishing and unlocking.
    static void released(void*) noexcept {}
    inline static const mm::MemOps memory_ops{invalidate_memory, released};
    inline static const cap::GrantAttachmentOps device_ops{invalidate_device_grant, released};
    inline static const cap::GrantAttachmentOps grant_ops{invalidate_memory_grant, released};

    mm::Pmm& pmm_;
    Executor& executor_;
    object::pool<irq::Irq>& irqs_;
    object::pool<mm::Mem>& mems_;
    cap::GrantGraph& grants_;
    mutable sync::Spin lock_{};
    SpaceState state_{SpaceState::Empty};
    libk::Atomic<bool> closing_{};
    libk::Atomic<bool> fault_signal_{};
    bool faulted_{};
    ipc::NotificationSource fault_source_{};
    object::ref<> self_{};
    object::cleanup cleanup_{};
    object::ref<Device> device_{};
    object::ref<mm::Mem> memory_{};
    std::optional<DeviceLease> lease_{};
    struct Bar final {
        object::ref<mm::Mem> memory{};
        cap::GrantRef grant{};
        cap::GrantRevoke revoke{};
    };
    std::array<Bar, 6> bars_{};
    object::ref<irq::Irq> interrupt_{};
    cap::GrantRef interrupt_grant_{};
    cap::GrantRevoke interrupt_revoke_{};
    mm::MemLink memory_attachment_{this, memory_ops};
    cap::GrantAttachment device_grant_{this, device_ops};
    cap::GrantAttachment memory_grant_{this, grant_ops};
    mm::MemWork memory_work_{};
    cap::GrantWork device_work_{};
    cap::GrantWork grant_work_{};
    resource::Sponsorship* sponsor_{};
    resource::Charge charge_{};
    mm::PageGroup metadata_{};
    Pins* pins_{};
    libk::IntrusiveListHook work_hook_{};
    bool work_open_{}; // protected by Executor lock
};

// Runtime has one service runner. The queue owns no capability: each admitted
// Space retains its structural self-reference until withdrawal is complete.
class Executor final : private libk::noncopyable_nonmovable {
public:
    using Notifier = libk::delegate<void() noexcept>;
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
    mutable sync::Spin lock_{};
    libk::IntrusiveList<Space, &Space::work_hook_> queue_{};
    Notifier notifier_{};
};

} // namespace io
