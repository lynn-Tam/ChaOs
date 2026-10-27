#pragma once
#include <mm/slab.hpp>

#include <mm/pmm.hpp>
#include <object/ref.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/unique_handle.hpp>
#include <uapi/abi.h>
#include <uapi/mem.h>
#include <wait.hpp>
#include <work.hpp>

namespace mm {

// All attenuated physical authorities share this one external-range registry.
// PMM remains authoritative for RAM and boot reservations.
class Phys final : private libk::noncopyable_nonmovable {
    struct Entry {
        explicit Entry(Pages range) noexcept : range(range) {}
        Pages range;
        libk::intrusive_list_hook hook;
    };
public:
    // Each external range claim has its own quota usage; Phys does not own the RAM.
    static constexpr bool fixed_usage = true;
    class Claim {
        friend class Phys;
        struct Data {
            object::ref<Phys> owner;
            Entry* claim{};
            static auto empty() noexcept -> Data { return {}; }
            static auto is_empty(const Data& d) noexcept -> bool { return !d.claim; }
        };
        struct Drop { void operator()(Data&) const noexcept; };
        libk::unique_handle<Data, Drop, Data> entry_;
        Claim(object::ref<Phys>&&, Entry*) noexcept;
    public:
        Claim() noexcept = default;
        explicit operator bool() const noexcept { return bool(entry_); }
        void reset() noexcept { entry_.reset(); }
        auto range() const noexcept -> Pages { return entry_ ? entry_.get().claim->range : Pages{}; }
        auto covers(Pages range) const noexcept -> bool {
            return entry_ && entry_.get().claim->range.contains(range);
        }
    };
    explicit Phys(Pmm& pmm, RegionList&& reserved) noexcept
        : pmm_(pmm), reserved_(std::move(reserved)), storage_(pmm) {}
    ~Phys() noexcept { libk_assert(claims_.empty()); }
    void bind_work(WorkQueue& work, object::Key owner) noexcept {
        work_ = &work; job_.bind(owner); events_.bind_work(work, owner); pmm_.watch(Pmm::Notify::bind<&Phys::schedule>(*this));
    }
    auto events() noexcept -> Source& { return events_; }
    auto info() const noexcept -> MemoryInfo;
    auto configure(usize low, usize high, usize floor) noexcept -> bool { return pmm_.configure(low, high, floor); }
    void retire(object::cleanup&&) noexcept;
    auto claim(object::ref<Phys>, const object::ref<>& quota, Pages) noexcept
        -> std::expected<Claim, status_t>;
private:
    void release(Entry*) noexcept;
    void schedule() noexcept { work_->post(job_); }
    void publish() noexcept { auto publication = events_.prepare(pmm_.pressured() ? SOURCE_READY : 0, true); }
    void drained() noexcept { if (cleanup_) cleanup_.complete(); }
    WorkQueue* work_{};
    Work job_{Work::Fn::bind<&Phys::publish>(*this)};
    Src<SOURCE_READY> events_{Source::Drained::bind<&Phys::drained>(*this)};
    object::cleanup cleanup_;
    Pmm& pmm_;
    RegionList reserved_;
    Slab<Entry, false> storage_;
    sync::Spin lock_;
    libk::intrusive_list<Entry, &Entry::hook> claims_;
};

} // namespace mm
