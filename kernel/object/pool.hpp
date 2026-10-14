#pragma once

#include <utility>
#include <libk/unique_handle.hpp>
#include <expected>
#include <libk/noncopyable.hpp>
#include <sync.hpp>
#include <object/ref.hpp>
#include <cap/grant.hpp>
#include <resource/sponsorship.hpp>
#include <work.hpp>
#include <libk/assert.hpp>
#include <base/slab.hpp>
#include <libk/types.hpp>
#include <libk/delegate.hpp>
#include <limits>
#include <memory>
#include <tuple>
#include <libk/manual_lifetime.hpp>
#include <mm/pmm.hpp>

namespace object {
template <typename T> struct traits;
}

namespace cap {
class Graph;
}

namespace object {

class Member;

class group final : private libk::noncopyable_nonmovable {
  public:
    enum class phase : u8 {
        open,
        closing,
        closed,
    };

    explicit group(resource::budget limit) noexcept;
    ~group() noexcept;

    [[nodiscard]] auto reserve(ref<> self, resource::budget charge) noexcept
        -> std::expected<resource::Reservation, resource::errc>;

    [[nodiscard]] auto limit() const noexcept -> resource::budget;
    [[nodiscard]] auto available() const noexcept -> resource::budget;
    [[nodiscard]] auto sponsorship_count() const noexcept -> usize;
    [[nodiscard]] auto state() const noexcept -> phase;
    [[nodiscard]] auto close() noexcept -> phase;
    [[nodiscard]] auto observe_refund(const ref<>& self, resource::RefundNotifier notifier) noexcept -> bool;
    void retire(cleanup&&) noexcept;

  private:
    friend struct traits<group>;
    template<class T> friend class pool;
    friend class resource::Reservation;
    friend class resource::Refund;
    friend class resource::Charge;
    friend class resource::Sponsorship;
    friend class Member;

    void bind_sponsor(resource::Sponsorship& sponsor) noexcept {
        libk_assert(sponsor_ == nullptr);
        sponsor_ = &sponsor;
    }

    [[nodiscard]] auto reserve(const resource::Sponsorship& parent, resource::budget charge) noexcept
        -> std::expected<resource::Reservation, resource::errc>;
    void cancel(resource::budget amount) noexcept;
    void commit() noexcept;
    void commit(Member& member) noexcept;
    void ready(Member& member) noexcept;
    void close_member(Member& member) noexcept;
    void attach(Member& member) noexcept;
    void detach(Member& member) noexcept;
    void attach(resource::Sponsorship& sponsorship) noexcept;
    void detach(resource::Sponsorship& sponsorship) noexcept;
    void refund(resource::budget amount) noexcept;
    void service() noexcept;
    [[nodiscard]] auto closed_locked() const noexcept -> bool;

    mutable sync::Spin lock_{};
    resource::Sponsorship* sponsor_{};
    resource::account account_;
    phase state_{phase::open};
    Member* members_{};
    usize sponsorship_count_{};
    bool servicing_{};
    cleanup cleanup_{};
};

// Embedded in the object slot. The initial grant root closes the entire
// lineage before this member retires its target.
class Member final : private libk::noncopyable_nonmovable {
    enum class phase : u8 {
        empty,
        pending,
        live,
        revoking,
        revoked,
        retiring,
    };

  public:
    Member() noexcept;
    ~Member() noexcept;

  private:
    friend class group;
    template<class T> friend class pool;

    auto root(cap::Graph&, resource::Reservation&&, ref<>, cap::View) noexcept
        -> std::expected<cap::GrantRef, cap::GrantError>;
    void commit() noexcept;
    void abort() noexcept;
    void retire() noexcept;
    void ready() noexcept;
    void detach() noexcept;
    auto pin() const noexcept -> ref<>;

    resource::Sponsorship fee_{};
    group* owner_{};
    cap::Graph* graph_{};
    cap::GrantKey root_{};
    Id target_{};
    ref<> hold_{};
    cap::GrantRevoke revoke_{};
    Member* previous_{};
    Member* next_{};
    phase phase_{phase::empty};
    bool independent_close_{};
};

} // namespace object

namespace resource {
class Sponsorship;
}

namespace object {

// Payload hooks own retirement policy. Objects without a hook only need the
// common lifetime gate and their destructor; the pool has no type whitelist.
template <typename T> struct traits {
    static void destroy(T& value) noexcept { std::destroy_at(&value); }
    static void retire(T& value) noexcept {
        if constexpr (requires { value.retire(); }) value.retire();
    }
    static void retire(T& value, cleanup&& cleanup) noexcept
        requires requires { value.retire(std::move(cleanup)); }
    {
        value.retire(std::move(cleanup));
    }
    static void bind_sponsor(T& value, resource::Sponsorship& charge) noexcept
        requires requires { value.bind_sponsor(charge); }
    {
        value.bind_sponsor(charge);
    }
};

template <typename T> class pool final : private libk::noncopyable_nonmovable {
    static_assert(kind<T> != ObjInvalid);

    struct Slot;
    using Storage = base::slab<Slot, mm::OwnedPage, mm::page_size>;
    using PageHeader = typename Storage::page;

    struct Slot final {
        object::anchor anchor{};
        libk::manual_lifetime<Member> member{};
        PageHeader* page{};
        Slot* next_free{};
        Slot* next_reclaim{};
        alignas(T) byte storage[sizeof(T)]{};

        [[nodiscard]] auto object() noexcept -> T* { return reinterpret_cast<T*>(storage); }
        [[nodiscard]] auto object() const noexcept -> const T* { return reinterpret_cast<const T*>(storage); }
    };

    static constexpr usize slots_per_page = Storage::capacity();

  public:
    [[nodiscard]] static constexpr auto slot_charge() noexcept -> resource::budget {
        return resource::budget{.memory = sizeof(Slot)};
    }

  private:
    struct PendingRelease final {
        object::pool<T>* owner{};
        void operator()(Slot*& slot) noexcept {
            owner->rollback(slot);
            slot = nullptr;
        }
    };

    using PendingToken = libk::unique_handle<Slot*, PendingRelease>;

  public:
    using reference = ref<T>;

    class pending final : private libk::noncopyable {
      public:
        pending(pending&&) noexcept = default;
        auto operator=(pending&&) noexcept -> pending& = default;

        [[nodiscard]] auto get() noexcept -> T& {
            libk_assert(token_);
            return *token_.get()->object();
        }

        [[nodiscard]] auto publish() noexcept -> reference {
            libk_assert(token_);
            Slot* const slot = token_.release();
            auto value = pool::reference_of(*slot);
            if (slot->member->owner_) slot->member->commit();
            return value;
        }

        void reset() noexcept { token_.reset(); }

        [[nodiscard]] auto reference() const noexcept -> ref<T> {
            libk_assert(token_);
            auto& slot = *token_.get();
            token_.get_deleter().owner->retain(slot.anchor, slot.anchor.generation_);
            return pool::reference_of(slot);
        }
        auto root(cap::Graph& graph, resource::Reservation&& fee, cap::View view) noexcept
            -> std::expected<cap::GrantRef, cap::GrantError> {
            return token_.get()->member->root(graph, std::move(fee), reference(), view);
        }

      private:
        friend class pool;
        pending(pool& pool, Slot& slot) noexcept : token_(&slot, PendingRelease{&pool}) {}

        PendingToken token_{};
    };

    explicit pool(mm::Pmm& pmm, WorkQueue& work) noexcept
        : pmm_(&pmm), work_(&work), job_(Work::Fn::bind<&pool::run_work>(*this)) {
        static_assert(__builtin_offsetof(Slot, anchor) == 0);
        static_assert(slots_per_page != 0);
        static_assert(alignof(T) <= mm::page_size);
    }

    ~pool() noexcept {
        work_->close(job_);
        drain();
        libk_assert(storage_.live() == 0);
        while (auto* page = storage_.take_page()) release_page(page);
    }

    template <typename... Args>
    [[nodiscard]] auto create(Args&&... args) noexcept {
        return create(resource::Reservation{}, std::forward<Args>(args)...);
    }

    template <typename... Args>
    [[nodiscard]] auto create(resource::Reservation&& charge, Args&&... args) noexcept {
        if constexpr (requires { T::prepare(charge.payer(), std::forward<Args>(args)...); }) {
            auto data = T::prepare(charge.payer(), std::forward<Args>(args)...);
            using Err = typename decltype(data)::error_type;
            using Result = std::expected<pending, Err>;
            if (!data) return Result{std::unexpected(data.error())};
            auto made = construct(std::move(charge), std::move(*data));
            if (!made) return Result{std::unexpected(made.error() == error::exhausted
                ? Err::GenerationExhausted : Err::OutOfMemory)};
            return Result{std::move(*made)};
        } else return construct(std::move(charge), std::forward<Args>(args)...);
    }

  private:
    template <typename... Args>
    auto construct(resource::Reservation&& sponsorship, Args&&... args) noexcept
        -> std::expected<pending, error> {
        auto claimed = claim_slot();
        if (!claimed) {
            return std::unexpected(claimed.error());
        }
        Slot* const slot = claimed.value();
        std::construct_at(slot->object(), std::forward<Args>(args)...);
        if constexpr (requires { slot->object()->bind_work(*work_); })
            slot->object()->bind_work(*work_);
        if (sponsorship) {
            slot->member->fee_.commit(std::move(sponsorship));
            if constexpr (requires(T& object, resource::Sponsorship& owner) {
                traits<T>::bind_sponsor(object, owner);
            }) traits<T>::bind_sponsor(*slot->object(), slot->member->fee_);
        }
        {
            sync::Lock guard{lock_};
            libk_assert(slot->anchor.phase_ == anchor::phase::constructing);
            slot->anchor.refs_ = 1;
            slot->anchor.phase_ = anchor::phase::live;
        }
        return (pending{*this, *slot});
    }

  public:
    [[nodiscard]] auto lookup(Id id) noexcept -> std::expected<reference, error> {
        sync::Lock guard{lock_};
        Slot* const slot = find_slot(id);
        if (slot == nullptr) {
            return std::unexpected(error::invalid_id);
        }
        if (!add_ref_locked(slot->anchor, id.generation)) {
            return std::unexpected(error::closed);
        }
        return (reference_of(*slot));
    }

  private:
    [[nodiscard]] auto request_retire(Id id) noexcept -> bool {
        Slot* slot{};
        {
            sync::Lock guard{lock_};
            slot = find_slot(id);
            if (slot == nullptr || slot->anchor.phase_ != anchor::phase::live) return false;
            slot->anchor.phase_ = anchor::phase::retiring;
            libk_assert(slot->anchor.refs_ != std::numeric_limits<usize>::max());
            ++slot->anchor.refs_;
        }
        finish_retire(*slot, id.generation);
        return true;
    }

  public:
    [[nodiscard]] auto live_count() const noexcept -> usize {
        sync::Lock guard{lock_};
        return storage_.live();
    }

  private:
    void finish_retire(Slot& slot, u64 generation) noexcept {
        // Group traversal pins live slots before releasing its lock. Once
        // retirement begins, detach before cleanup can make this slot reusable.
        slot.member->detach();
        cleanup done{slot.anchor, generation};
        if constexpr (requires(T& value, cleanup&& token) { traits<T>::retire(value, std::move(token)); }) {
            traits<T>::retire(*slot.object(), std::move(done));
        } else {
            traits<T>::retire(*slot.object());
            done.complete();
        }
    }

    [[nodiscard]] auto find_slot(Id id) noexcept -> Slot* {
        if (!id.valid()) return nullptr;
        auto* slot = storage_.find(id.slot);
        return slot != nullptr && slot->anchor.generation_ == id.generation ? slot : nullptr;
    }

    [[nodiscard]] auto claim_slot() noexcept -> std::expected<Slot*, error> {
        for (;;) {
            {
                sync::Lock guard{lock_};
                if (storage_.exhausted()) return std::unexpected(error::exhausted);
                if (auto entry = storage_.claim(); entry.slot != nullptr) {
                    auto& anchor = entry.slot->anchor;
                    anchor.generation_ = entry.generation;
                    anchor.refs_ = 0;
                    anchor.clean_ = anchor.queued_ = false;
                    anchor.phase_ = anchor::phase::constructing;
                    entry.slot->next_reclaim = nullptr;
                    (void)entry.slot->member.emplace();
                    return (entry.slot);
                }
            }
            auto page = pmm_->allocate_page();
            if (!page) return std::unexpected(error::out_of_memory);
            auto* storage = Storage::make(std::move(page).value(), [&](Slot& slot) {
                slot.anchor.owner_ = this;
                slot.anchor.ops_ = &anchor_ops_;
                slot.anchor.kind_ = object::kind<T>;
            });
            sync::Lock guard{lock_};
            storage_.add(*storage);
        }
    }

    [[nodiscard]] static auto reference_of(Slot& slot) noexcept -> reference {
        return reference{slot.anchor, slot.anchor.generation_};
    }

    void rollback(Slot* slot) noexcept {
        auto target = reference_of(*slot);
        if (slot->member->owner_) slot->member->abort();
        else libk_assert(target.retire());
    }

    [[nodiscard]] static auto slot_of(anchor& anchor) noexcept -> Slot& {
        return *reinterpret_cast<Slot*>(&anchor);
    }

    [[nodiscard]] auto add_ref_locked(anchor& anchor, u64 generation) noexcept -> bool {
        libk_assert(lock_.held());
        libk_assert(anchor.owner_ == this);
        if (anchor.generation_ != generation || anchor.phase_ != anchor::phase::live) {
            return false;
        }
        libk_assert(anchor.refs_ != std::numeric_limits<usize>::max());
        ++anchor.refs_;
        return true;
    }

    void retain(anchor& anchor, u64 generation) noexcept {
        sync::Lock guard{lock_};
        libk_assert(anchor.generation_ == generation && anchor.refs_ != 0);
        libk_assert(anchor.phase_ == anchor::phase::live || anchor.phase_ == anchor::phase::retiring);
        libk_assert(anchor.refs_ != std::numeric_limits<usize>::max());
        ++anchor.refs_;
    }

    void release_ref(anchor& anchor, u64 generation) noexcept {
        bool queued{}, retire{};
        {
            sync::Lock guard{lock_};
            libk_assert(anchor.owner_ == this && anchor.generation_ == generation);
            libk_assert(anchor.refs_ != 0);
            --anchor.refs_;
            if (anchor.refs_ == 0 && anchor.phase_ == anchor::phase::live) {
                anchor.phase_ = anchor::phase::retiring;
                ++anchor.refs_; // Cleanup owns the slot until its hook completes.
                auto& slot = slot_of(anchor);
                anchor.queued_ = true;
                slot.next_reclaim = reclaim_head_;
                reclaim_head_ = &slot;
                retire = true;
            } else queued = queue_reclaim_if_ready(slot_of(anchor));
        }
        if (retire || queued) notify_cleanup();
    }

    void finish_cleanup(anchor& anchor, u64 generation) noexcept {
        bool queued{};
        {
            sync::Lock guard{lock_};
            libk_assert(anchor.owner_ == this && anchor.generation_ == generation);
            libk_assert(anchor.phase_ == anchor::phase::retiring);
            libk_assert(!anchor.clean_ && anchor.refs_ != 0);
            anchor.clean_ = true;
            --anchor.refs_;
            queued = queue_reclaim_if_ready(slot_of(anchor));
        }
        if (queued) notify_cleanup();
    }

    static void anchor_retain(void* owner, anchor& anchor, u64 generation) noexcept {
        static_cast<pool*>(owner)->retain(anchor, generation);
    }

    static auto anchor_live(void* owner, anchor& anchor, u64 generation) noexcept -> bool {
        auto& pool = *static_cast<class pool*>(owner);
        sync::Lock guard{pool.lock_};
        return anchor.generation_ == generation && anchor.phase_ == anchor::phase::live;
    }

    static auto anchor_try_retain(void* owner, anchor& anchor, u64 generation) noexcept -> bool {
        auto& storage = *static_cast<pool*>(owner);
        sync::Lock guard{storage.lock_};
        return storage.add_ref_locked(anchor, generation);
    }

    static void anchor_drop_ref(void* owner, anchor& anchor, u64 generation) noexcept {
        static_cast<pool*>(owner)->release_ref(anchor, generation);
    }

    [[nodiscard]] static auto payload(anchor& anchor) noexcept -> void* { return slot_of(anchor).object(); }

    static auto destroy(void* owner, anchor& anchor, u64 generation) noexcept -> bool {
        auto& storage = *static_cast<pool*>(owner);
        auto& member = *slot_of(anchor).member;
        // The grant operation prevents managed-member detach until this call
        // returns; its target reference also pins the slot.
        if (member.owner_) {
            member.owner_->close_member(member);
            return true;
        }
        return storage.request_retire({reinterpret_cast<usize>(&anchor), generation});
    }

    [[nodiscard]] static auto anchor_request_retire(void* owner, anchor& anchor, u64 generation) noexcept
        -> bool {
        if (anchor.generation_ != generation) {
            return false;
        }
        return static_cast<pool*>(owner)->request_retire(Id{
            .slot = reinterpret_cast<usize>(&anchor),
            .generation = generation,
        });
    }

    static void anchor_finish_cleanup(void* owner, anchor& anchor, u64 generation) noexcept {
        static_cast<pool*>(owner)->finish_cleanup(anchor, generation);
    }

    inline static constexpr anchor::Ops anchor_ops_{
        .retain = anchor_retain,
        .live = anchor_live,
        .try_retain = anchor_try_retain,
        .drop_ref = anchor_drop_ref,
        .payload = payload,
        .destroy = destroy,
        .request_retire = anchor_request_retire,
        .finish_cleanup = anchor_finish_cleanup,
    };

    [[nodiscard]] auto queue_reclaim_if_ready(Slot& slot) noexcept -> bool {
        anchor& anchor = slot.anchor;
        if (anchor.phase_ != anchor::phase::retiring || !anchor.clean_ || anchor.refs_ != 0 ||
            anchor.queued_) {
            return false;
        }
        anchor.queued_ = true;
        slot.next_reclaim = reclaim_head_;
        reclaim_head_ = &slot;
        return true;
    }

    // Only the worker or exclusive pool teardown runs finalizers.
    void drain(usize budget = std::numeric_limits<usize>::max()) noexcept {
        for (usize i = 0; i < budget; ++i) {
            Slot* slot{};
            bool retire{};
            {
                sync::Lock guard{lock_};
                slot = reclaim_head_;
                if (slot == nullptr) return;
                reclaim_head_ = slot->next_reclaim;
                slot->next_reclaim = nullptr;
                slot->anchor.queued_ = false;
                libk_assert(slot->anchor.phase_ == anchor::phase::retiring);
                retire = !slot->anchor.clean_;
                if (retire) libk_assert(slot->anchor.refs_ == 1);
                else {
                    libk_assert(slot->anchor.refs_ == 0);
                    slot->anchor.phase_ = anchor::phase::quiescent;
                }
            }
            if (retire) {
                finish_retire(*slot, slot->anchor.generation_);
                continue;
            }
            traits<T>::destroy(*slot->object());
            auto refund = slot->member->fee_.detach();
            finalize_free(*slot);
            refund.complete();
        }
    }

    void notify_cleanup() noexcept { work_->post(job_); }
    void run_work() noexcept {
        drain(8);
        sync::Lock guard{lock_};
        if (reclaim_head_) work_->post(job_);
    }

    void finalize_free(Slot& slot) noexcept {
        PageHeader* release{};
        {
            sync::Lock guard{lock_};
            libk_assert(slot.anchor.phase_ == anchor::phase::quiescent);
            libk_assert(storage_.live() != 0 && slot.page->live != 0);
            slot.anchor.phase_ = anchor::phase::free;
            slot.anchor.clean_ = false;
            slot.member.reset();
            release = storage_.release(slot);
        }
        if (release != nullptr) {
            release_page(release);
        }
    }

    void release_page(PageHeader* page) noexcept {
        libk_assert(page != nullptr);
        libk_assert(page->live == 0);
        auto backing = Storage::dispose(*page);
        backing.reset();
    }

    mm::Pmm* pmm_{};
    WorkQueue* work_{};
    Work job_;
    mutable sync::Spin lock_{};
    Storage storage_{};
    Slot* reclaim_head_{};
};

template <typename T> using pending = typename pool<T>::pending;

// Typed storage only. Construction policy belongs to the payload/caller;
// every pool posts finalizers to the same actual worker.
template <class... Ts> class store {
  public:
    store(mm::Pmm& pmm, WorkQueue& work) noexcept : pmm_(&pmm), work_(&work) {
        ((void)std::get<libk::manual_lifetime<pool<Ts>>>(pools_).emplace(pmm, work), ...);
    }
    ~store() noexcept {
        while (work_->run()) {}
        (std::get<libk::manual_lifetime<pool<Ts>>>(pools_).reset(), ...);
    }
    auto pmm() noexcept -> mm::Pmm& { return *pmm_; }
    auto work() noexcept -> WorkQueue& { return *work_; }
    template <class T> auto get() noexcept -> pool<T>& {
        return *std::get<libk::manual_lifetime<pool<T>>>(pools_);
    }
  private:
    mm::Pmm* pmm_;
    WorkQueue* work_;
    std::tuple<libk::manual_lifetime<pool<Ts>>...> pools_;
};

} // namespace object
