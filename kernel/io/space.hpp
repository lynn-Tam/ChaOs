#pragma once

#include <cap/grant.hpp>
#include <irq/irq.hpp>
#include <ipc/notification_source.hpp>
#include <mm/kspace.hpp>
#include <mm/mem.hpp>
#include <time.hpp>
#include <uapi/io.h>

namespace io {
class Space;

// Private controller protocol and storage. No device enumeration or reset.
class Iommu final : private libk::noncopyable_nonmovable {
    friend class Space;
public:
    Iommu(mm::Pmm&, mm::KSpace&, Cpus&, WorkQueue&, const time::Clock&,
        object::ref<>, mm::Phys::Lease&&, object::ref<irq::Ctl>, irq::Line, IommuConfig) noexcept;
    ~Iommu() noexcept;
    auto open() noexcept -> status_t;
    auto state() const noexcept -> IoState;
    void retire(object::cleanup&&) noexcept;
private:
    enum class Phase : u8 { Empty, Disabling, Queues, Directory, Ready, Stopping, Unmapping, Closed, Failed };
    template<class T> auto read(usize) const noexcept -> T;
    template<class T> void write(usize, T) noexcept;
    auto page() noexcept -> std::expected<mm::Page, status_t>;
    auto context(u32, bool) noexcept -> u64*;
    auto attach(Space&) noexcept -> status_t;
    void detach(Space&) noexcept;
    auto sync(Space&, bool) noexcept -> status_t;
    void fail() noexcept;
    auto interrupt() noexcept -> bool;
    void disconnected() noexcept;
    void progress() noexcept;
    mm::KSpace& vm_;
    Cpus& cpus_;
    WorkQueue& work_;
    const time::Clock& clock_;
    object::ref<> payer_;
    mm::Phys::Lease claim_;
    object::ref<irq::Ctl> irq_owner_;
    irq::Route irq_;
    IommuConfig config_;
    mm::KMap mapping_;
    mm::PageGroup storage_;
    resource::Charge fee_;
    mm::Page directory_{}, commands_{}, faults_{};
    mutable sync::Spin lock_;
    Phase phase_{Phase::Empty};
    Space* domains_{};
    Space* inflight_{};
    u32 tail_{}, fault_head_{};
    u64 deadline_{};
    std::atomic<bool> irq_pending_{}, disconnected_{};
    bool closing_{}, mapped_{};
    object::cleanup cleanup_;
    std::optional<mm::Flush> flush_;
    Work job_;
};

// One binding, one page tree, one collection of mappings. A closed binding
// never reopens; the trusted device manager must reset before creating another.
class Space final : private libk::noncopyable_nonmovable {
    friend class Iommu;
    struct Map {
        struct Pin { explicit Pin(mm::PageHold&& p) noexcept : page(std::move(p)) {} mm::PageHold page; Pin* next{}; };
        Map(mm::Pmm&, Space&, usize, usize) noexcept;
        ~Map() noexcept;
        static void invalidate_mem(void*, mm::MemWork&&) noexcept;
        static void invalidate_grant(void*, cap::GrantWork&&) noexcept;
        static void released(void*) noexcept {}
        inline static const mm::MemOps mem_ops{invalidate_mem, released};
        inline static const cap::GrantAttachmentOps grant_ops{invalidate_grant, released};
        Space& owner;
        usize first, count;
        Map* next{};
        object::ref<mm::Mem> memory;
        mm::Slab<Pin, false> pins;
        Pin* pages{};
        mm::MemLink mem_link{this, mem_ops};
        cap::GrantAttachment grant{this, grant_ops};
        mm::MemWork mem_work;
        cap::GrantWork grant_work;
        bool removing{};
    };
public:
    Space(mm::Pmm&, WorkQueue&, object::ref<>, object::ref<Iommu>, u32) noexcept;
    ~Space() noexcept;
    auto open(object::ref<>, cap::Resolved<Iommu>&) noexcept -> status_t;
    auto map(cap::Resolved<mm::Mem>&, mm::ObjectRange, usize, mm::Perms) noexcept -> std::expected<usize, status_t>;
    auto unmap(usize, usize) noexcept -> status_t;
    auto watch(ipc::Notification&, u64) noexcept -> status_t;
    auto state() const noexcept -> IoState;
    void close() noexcept;
    void retire(object::cleanup&&) noexcept;
private:
    auto leaf(usize, bool) noexcept -> u64*;
    void progress() noexcept;
    auto release(Map&) noexcept -> bool;
    static void invalidated(void*, cap::GrantWork&&) noexcept;
    static void released(void*) noexcept {}
    inline static const cap::GrantAttachmentOps ctl_ops_{invalidated, released};
    mm::Pmm& pmm_;
    WorkQueue& work_;
    object::ref<> payer_, self_;
    object::ref<Iommu> controller_;
    u32 requester_, pscid_{};
    Space* next_{};
    mm::Slab<Map, false> maps_;
    Map* mappings_{};
    mm::PageGroup tables_;
    resource::Charge fee_;
    mm::Page root_{};
    mutable sync::Spin lock_;
    enum class Phase : u8 { Opening, Ready, Editing, Denying, Retiring, Closed, Failed };
    Phase phase_{Phase::Opening};
    bool preparing_{}, attached_{};
    std::atomic<bool> closing_{}, failed_{};
    cap::GrantAttachment ctl_grant_{this, ctl_ops_};
    cap::GrantWork ctl_work_;
    ipc::NotificationSource notice_;
    object::cleanup cleanup_;
    Work job_;
};
} // namespace io
