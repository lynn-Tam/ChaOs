#pragma once
#include <work.hpp>
#include <libk/unique_handle.hpp>

#include <expected>
#include <optional>


#include <libk/intrusive_list.hpp>
#include <uapi/io.h>
#include <cap/grant.hpp>
#include <io/host.hpp>
#include <irq/irq.hpp>
#include <mm/mem.hpp>
#include <object/ref.hpp>

namespace object { template<class> class pool; }

namespace io {

enum class SpaceState : u8 { Empty, Binding, Opening, Active, Closing, Closed, Failed, Faulted };
enum class SpaceError : u8 { Absent, InvalidState, Denied, InvalidRange, Busy,
    UnsupportedMemory, BackingUnavailable, OutOfMemory, QuotaExceeded, Cancelled };

// One immutable DMA arena and one exclusive hardware session. Source
// attachments revoke the binding; page pins survive through hardware drain.
class Space final : private libk::noncopyable_nonmovable {
public:
    Space(mm::Pmm& pmm, WorkQueue& work, object::pool<irq::Irq>& irqs, object::pool<mm::Mem>& memory,
        cap::Graph& grants) noexcept;
    ~Space() noexcept;

    [[nodiscard]] auto bind(object::ref<> self,
        cap::Resolved<Host>& host, cap::Resolved<mm::Mem>& memory,
        mm::ObjectRange range, usize first) noexcept
        -> std::expected<void, SpaceError>;
    [[nodiscard]] auto watch(ipc::Notification& notification, u64 badge) noexcept
        -> std::expected<void, SpaceError>;
    [[nodiscard]] auto state() const noexcept -> SpaceState;
    [[nodiscard]] auto reg(usize index) noexcept
        -> std::expected<std::pair<cap::GrantRef, usize>, SpaceError>;
    [[nodiscard]] auto interrupt() noexcept
        -> std::expected<cap::GrantRef, SpaceError>;
    void close() noexcept;
    void retire(object::cleanup&& cleanup) noexcept;

private:
    void run_work() noexcept;
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
    [[nodiscard]] auto prepare(cap::Resolved<Host>& host, usize requester,
        cap::Resolved<mm::Mem>& memory, mm::ObjectRange range,
        usize first, usize tables) noexcept -> std::expected<mm::PageTable, SpaceError>;
    [[nodiscard]] auto service() noexcept -> Completion;
    [[nodiscard]] auto prepare_regs() noexcept -> std::expected<void, SpaceError>;
    [[nodiscard]] auto retire_regs() noexcept -> bool;
    [[nodiscard]] auto prepare_interrupt() noexcept -> std::expected<void, SpaceError>;
    [[nodiscard]] auto retire_interrupt() noexcept -> bool;
    void free_pages() noexcept;
    void bind_sponsor(resource::Sponsorship& sponsor) noexcept { sponsor_ = &sponsor; }
    void stop_host(bool fault) noexcept;
    static void invalidate_memory(void*, mm::MemWork&&) noexcept;
    static void invalidate_host_grant(void*, cap::GrantWork&&) noexcept;
    static void invalidate_memory_grant(void*, cap::GrantWork&&) noexcept;
    // Service takes work under lock and releases it outside the lock;
    // callbacks never release their own token. No callback may touch Space
    // after publishing and unlocking.
    static void released(void*) noexcept {}
    inline static const mm::MemOps memory_ops{invalidate_memory, released};
    inline static const cap::GrantAttachmentOps host_ops{invalidate_host_grant, released};
    inline static const cap::GrantAttachmentOps grant_ops{invalidate_memory_grant, released};

    mm::Pmm& pmm_;
    WorkQueue& work_;
    object::pool<irq::Irq>& irqs_;
    object::pool<mm::Mem>& mems_;
    cap::Graph& grants_;
    mutable sync::Spin lock_{};
    SpaceState state_{SpaceState::Empty};
    libk::Atomic<bool> closing_{};
    libk::Atomic<bool> fault_signal_{};
    bool faulted_{};
    ipc::NotificationSource fault_source_{};
    object::ref<> self_{};
    object::cleanup cleanup_{};
    object::ref<Host> host_{};
    object::ref<mm::Mem> memory_{};
    struct Drop { void operator()(Hw* hw) const noexcept { hw->release(); } };
    libk::unique_handle<Hw*, Drop> hw_{};
    struct Reg final {
        usize index;
        libk::IntrusiveListHook hook{};
        object::ref<mm::Mem> memory{};
        cap::GrantRef grant{};
        cap::GrantRevoke revoke{};
    };
    mm::Slab<Reg, false> reg_store_;
    libk::IntrusiveList<Reg, &Reg::hook> regs_{};
    object::ref<irq::Irq> interrupt_{};
    cap::GrantRef interrupt_grant_{};
    cap::GrantRevoke interrupt_revoke_{};
    mm::MemLink memory_attachment_{this, memory_ops};
    cap::GrantAttachment host_grant_{this, host_ops};
    cap::GrantAttachment memory_grant_{this, grant_ops};
    mm::MemWork memory_work_{};
    cap::GrantWork host_work_{};
    cap::GrantWork grant_work_{};
    resource::Sponsorship* sponsor_{};
    resource::Charge charge_{};
    mm::PageGroup metadata_{};
    Pins* pins_{};
    Work job_{};
};

} // namespace io
