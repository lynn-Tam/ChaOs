#pragma once

#include <cap/grant.hpp>
#include <op.hpp>
#include <libk/manual_lifetime.hpp>
#include <irq/irq.hpp>
#include <mm/kspace.hpp>
#include <mm/mem.hpp>
#include <time.hpp>
#include <uapi/io.h>

namespace io {
class Space;

struct Map {
    struct Pin {
        Pin(mm::PageHold&& p, usize i) noexcept : page(std::move(p)), index(i) {}
        mm::PageHold page;
        usize index;
        Pin* next{};
    };
    Map(mm::Pmm&, Space&, usize, usize) noexcept;
    ~Map() noexcept;
    static void invalidate_mem(void*, mm::MemWork&&) noexcept;
    static void invalidate_grant(void*, object::LinkWork&&) noexcept;
    static void released(void*, mm::MemWork&&) noexcept;
    static void released(void*, object::LinkWork&&) noexcept;
    inline static const mm::MemOps mem_ops{invalidate_mem, released};
    inline static const cap::AttachOps grant_ops{invalidate_grant, released};
    Space& owner;
    usize first, count, offset{};
    libk::intrusive_tree_hook hook;
    mm::Perms perms;
    object::ref<mm::Mem> memory;
    mm::Slab<Pin, false> pins;
    Pin* pages{};
    mm::MemLink mem_link{this, mem_ops};
    cap::Attachment grant{this, grant_ops};
    mm::MemWork mem_work;
    object::LinkWork grant_work;

};

struct MapCmp {
    bool operator()(const Map& a, const Map& b) const noexcept { return a.first < b.first; }
    bool operator()(usize a, const Map& b) const noexcept { return a < b.first; }
    bool operator()(const Map& a, usize b) const noexcept { return a.first < b; }
};
using MapTree = libk::intrusive_tree<Map, &Map::hook, MapCmp>;

class IoOp final : private libk::noncopyable_nonmovable {
    friend class Space;
    friend class Iommu;
    struct Receipt { Space* owner; u32 tail{}; bool submitted{}, acknowledged{}; };
    Space* owner_;
    Op state_;
    Receipt receipt_;
    MapTree retired_;
    resource::Charge tables_fee_;
    mm::PageGroup pages_;
    usize value_{};
    status_t result_{STATUS_OK};
    bool enable_{true};
    struct Edit {
        enum Phase : u8 { Seek, Pin, Count, Reserve, Apply } phase{Seek};
        Map* mapping{};
        mm::ObjectRange range{};
        mm::Perms perms{};
        usize position{}, tables{};
        Map::Pin* pin{};
        std::optional<mm::PageTable::Count> plan{};
        bool install{}, automatic{};
    };
    std::optional<Edit> edit_;
public:
    static constexpr bool fixed_bill = true;
    explicit IoOp(object::ref<>) noexcept;
    IoOp(Space&, bool) noexcept;
    ~IoOp() noexcept { libk_assert(retired_.empty() && !tables_fee_ && !pages_.page_count() && !edit_ && !receipt_.submitted); }
    auto op() noexcept -> Op& { return state_; }
    void bind_work(WorkQueue& work, object::Key owner) noexcept { state_.bind(work, owner); }
    void retire(object::cleanup&& done) noexcept { state_.retire(std::move(done)); }
};

// Private controller protocol and storage. No device enumeration or reset.
class Iommu final : private libk::noncopyable_nonmovable {
    friend class Space;
public:
    Iommu(mm::Pmm&, mm::KSpace&, Cpus&, WorkQueue&, const time::Clock&,
        object::ref<>, mm::Phys::Lease&&, object::ref<irq::Ctl>, irq::Line, IommuConfig) noexcept;
    ~Iommu() noexcept;
    template<class Transaction> auto billing(const object::ref<>& to, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        {
            sync::Lock guard{lock_};
            if (phase_ != Phase::Ready || closing_ || billing_ || inflight_) return std::unexpected(STATUS_BUSY);
            billing_ = true;
        }
        libk::scope_exit resume{[&]() noexcept {
            { sync::Lock guard{lock_}; billing_ = false; }
            work_.post(job_);
        }};
        object::ref<> payer{to};
        return transaction([&](auto&& visit) { visit(fee_); }, [&] { std::swap(payer_, payer); });
    }
    template<class Publish> auto admit(Publish&& publish) noexcept -> status_t { return authority_.admit(std::forward<Publish>(publish)); }
    auto activate() noexcept -> status_t;
    void links(u64 cursor, LinkRecords& out) const noexcept;
    auto open(object::ref<> cpu) noexcept -> status_t;
    auto state() const noexcept -> IoState;
    void bind_work(WorkQueue& work, object::Key owner) noexcept { job_.bind(owner); initialization_.bind(work, owner); authority_.bind(owner); }
    void retire(object::cleanup&&) noexcept;
private:
    enum class Phase : u8 { Empty, Prepared, Disabling, Queues, Directory, Ready, Stopping, Unmapping, Closed, Failed };
    template<class T> auto read(usize) const noexcept -> T;
    template<class T> void write(usize, T) noexcept;
    auto page() noexcept -> std::expected<mm::Page, status_t>;
    auto context(u32, bool) noexcept -> u64*;
    auto attach(Space&) noexcept -> status_t;
    void detach(Space&) noexcept;
    auto sync(IoOp::Receipt&, bool) noexcept -> status_t;
    void fail() noexcept;
    auto interrupt() noexcept -> irq::Route::Delivery;
    auto disconnected() noexcept -> Source::Publication;
    void progress() noexcept;
    void advance() noexcept;
    void complete() noexcept;
    void close() noexcept; // Caller holds lock_; propagates the one close transition.
    void post() noexcept { work_.post(job_); }
    void invalidated() noexcept;
    cap::Authority<2> authority_{Source::Action::bind<&Iommu::invalidated>(*this), Source::Action::bind<&Iommu::post>(*this)};
    Op initialization_{{}, Source::Action::bind<&Iommu::post>(*this), Source::Action::bind<&Iommu::advance>(*this)};
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
    IoOp::Receipt* inflight_{};
    u32 tail_{}, fault_head_{};
    u64 deadline_{};
    std::atomic<bool> irq_pending_{}, disconnected_{};
    bool closing_{}, mapped_{}, billing_{}, advancing_{};
    object::cleanup cleanup_;
    std::optional<mm::Flush> flush_;
    Work job_;
};

// One binding, one page tree, one collection of mappings. A closed binding
// never reopens; the trusted device manager must reset before creating another.
class Space final : private libk::noncopyable_nonmovable {
    friend class Iommu;
    friend class IoOp;
    friend struct Map;
public:
    Space(mm::Pmm&, WorkQueue&, object::ref<>, object::ref<Iommu>, u32) noexcept;
    ~Space() noexcept;
    template<class Transaction> auto billing(const object::ref<>& to, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        {
            sync::Lock guard{lock_};
            if (phase_ != Phase::Ready || active_ || preparing_ || closing_.load() || failed_.load())
                return std::unexpected(STATUS_BUSY);
            preparing_ = true;
        }
        libk::scope_exit resume{[&]() noexcept {
            { sync::Lock guard{lock_}; preparing_ = false; }
            work_.post(job_);
        }};
        object::ref<> payer{to};
        return transaction([&](auto&& visit) {
            visit(fee_); maps_.charges(visit);
            for (auto* map = mappings_.minimum(); map; map = mappings_.next(*map)) map->pins.charges(visit);
        }, [&] { std::swap(payer_, payer); });
    }
    auto open(object::ref<>, cap::Resolved<Iommu>&, object::ref<> cpu) noexcept -> status_t;
    auto map(Op::Submission, cap::Resolved<mm::Mem>&, mm::ObjectRange, usize, mm::Perms) noexcept -> std::expected<usize, status_t>;
    auto unmap(Op::Submission, usize, usize) noexcept -> status_t;
    auto state() const noexcept -> IoState;
    auto links(u64 cursor, u64 epoch, LinkRecords&) const noexcept -> status_t;
    auto events() noexcept -> Source& { return events_; }
    void close() noexcept;
    void bind_work(WorkQueue&, object::Key owner) noexcept { job_.bind(owner); ordinary_.bind(owner); events_.bind_work(work_, owner); }
    void retire(object::cleanup&&) noexcept;
private:
    void advance_edit(IoOp&) noexcept;
    void progress() noexcept;
    void advance() noexcept;
    void publish_state() noexcept;
    void complete() noexcept;
    void post() noexcept { work_.post(job_); }
    auto state_locked() const noexcept -> IoState;
    auto release(Map&) noexcept -> bool;
    static void invalidated(void*, object::LinkWork&&) noexcept;
    static void released(void*, object::LinkWork&& done) noexcept { done.reset(); }
    inline static const cap::AttachOps ctl_ops_{invalidated, released};
    mm::Pmm& pmm_;
    WorkQueue& work_;
    object::ref<> payer_, self_;
    object::ref<Iommu> controller_;
    u32 requester_, pscid_{};
    Space* next_{};
    mm::Slab<Map, false> maps_;
    MapTree mappings_;
    u64 map_epoch_{1}, observed_epoch_{};
    libk::manual_lifetime<mm::PageTable> root_;
    resource::Charge fee_;
    mutable sync::Spin lock_;
    enum class Phase : u8 { Opening, Ready, Closing, Closed, Failed };
    Phase phase_{Phase::Opening};
    libk::manual_lifetime<IoOp> control_;
    IoOp* active_{};
    object::ref<> operation_;
    auto transaction(bool enable) noexcept -> IoOp&;
    auto accept(Op::Submission, bool commit = true) noexcept -> status_t;
    bool preparing_{}, attached_{};
    std::atomic<bool> closing_{}, failed_{};
    cap::Attachment ctl_grant_{this, ctl_ops_};
    object::LinkWork ctl_work_;
    Src<(u64{1} << (IO_SPACE_FAILED + 1)) - 1> events_{Source::Drained::bind<&Space::post>(*this)};
    object::cleanup cleanup_;
    Work job_;
    Work ordinary_{Work::Fn::bind<&Space::advance>(*this), Work::Fn::bind<&Space::post>(*this)};
};
} // namespace io
