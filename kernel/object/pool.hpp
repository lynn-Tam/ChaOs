#pragma once
#include <object/scope.hpp>
#include <base/slab.hpp>
#include <tuple>
#include <libk/manual_lifetime.hpp>

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
    static void bind_allocation(T& value, resource::Allocation& usage) noexcept
        requires requires { value.bind_allocation(usage); }
    {
        value.bind_allocation(usage);
    }
};

template <typename T> class pool final : private libk::noncopyable_nonmovable {
    static_assert(kind<T> != ObjInvalid);

    struct Slot;
    using Storage = base::slab<Slot, mm::OwnedPage, mm::page_size>;
    using PageHeader = typename Storage::page;

    struct Slot final {
        object::anchor anchor{};
        libk::manual_lifetime<Membership> member{};
        resource::Allocation allocation;
        PageHeader* page{};
        Slot* next_free{};
        Slot* next_reclaim{};
        alignas(T) byte storage[sizeof(T)]{};

        [[nodiscard]] auto object() noexcept -> T* { return reinterpret_cast<T*>(storage); }
        [[nodiscard]] auto object() const noexcept -> const T* { return reinterpret_cast<const T*>(storage); }
    };

    static constexpr usize slots_per_page = Storage::capacity();

  public:
    // Logical slot charge, not an exclusive frame reservation. Shared slab
    // padding and reusable vacant slots remain resident until the page empties.
    [[nodiscard]] static constexpr auto slot_usage() noexcept -> resource::Capacity {
        return resource::Capacity{.memory = sizeof(Slot)};
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

    template<class... Args>
    [[nodiscard]] auto create(Scope::Admission&& owner, resource::Reservation&& usage, Args&&... args) noexcept {
        auto made = create(std::move(usage), std::forward<Args>(args)...);
        if (made) {
            auto* slot = made->token_.get();
            if (!owner.attach(*slot->member, made->reference().key())) {
                using Err = typename decltype(made)::error_type;
                if constexpr (std::is_same_v<Err, error>) return decltype(made){std::unexpected(error::closed)};
                else return decltype(made){std::unexpected(Err::InvalidState)};
            }
        }
        return made;
    }

    template <typename... Args>
    [[nodiscard]] auto create(resource::Reservation&& usage, Args&&... args) noexcept {
        if constexpr (requires { T::prepare(usage.quota(), std::forward<Args>(args)...); }) {
            auto data = T::prepare(usage.quota(), std::forward<Args>(args)...);
            using Err = typename decltype(data)::error_type;
            using Result = std::expected<pending, Err>;
            if (!data) return Result{std::unexpected(data.error())};
            auto made = construct(std::move(usage), std::move(*data));
            if (!made) return Result{std::unexpected(made.error() == error::exhausted
                ? Err::GenerationExhausted : Err::OutOfMemory)};
            return Result{std::move(*made)};
        } else return construct(std::move(usage), std::forward<Args>(args)...);
    }

  private:
    template <typename... Args>
    auto construct(resource::Reservation&& reservation, Args&&... args) noexcept
        -> std::expected<pending, error> {
        auto claimed = claim_slot();
        if (!claimed) {
            return std::unexpected(claimed.error());
        }
        Slot* const slot = claimed.value();
        std::construct_at(slot->object(), std::forward<Args>(args)...);
        if constexpr (requires { slot->object()->control(); }) slot->object()->control().bind(slot->anchor);
        if constexpr (requires { slot->object()->bind_membership(*slot->member); })
            slot->object()->bind_membership(*slot->member);
        if (reservation) {
            slot->allocation.commit(std::move(reservation));
            if constexpr (requires { slot->object()->control(); })
                slot->allocation.bind_control(slot->object()->control());
            if constexpr (requires(T& object, resource::Allocation& owner) {
                traits<T>::bind_allocation(object, owner);
            }) traits<T>::bind_allocation(*slot->object(), slot->allocation);
        }
        {
            sync::Lock guard{lock_};
            libk_assert(slot->anchor.phase_ == anchor::phase::constructing);
            slot->anchor.refs_ = 1;
            slot->anchor.membership_ = &*slot->member;
            slot->anchor.phase_ = anchor::phase::live;
        }
        if constexpr (requires { slot->object()->bind_work(*work_, Key{}); })
            slot->object()->bind_work(*work_, Key{reinterpret_cast<usize>(&slot->anchor), slot->anchor.generation_});
        slot->anchor.index(true);
        return (pending{*this, *slot});
    }

  public:
    [[nodiscard]] auto lookup(Key id) noexcept -> std::expected<reference, error> {
        sync::Lock guard{lock_};
        Slot* const slot = find_slot(id);
        if (slot == nullptr) {
            return std::unexpected(error::invalid_id);
        }
        if (!slot->anchor.gate_.open() || !add_ref_locked(slot->anchor, id.generation)) {
            return std::unexpected(error::closed);
        }
        return (reference_of(*slot));
    }

  private:
    [[nodiscard]] auto request_retire(Key id) noexcept -> bool {
        Slot* slot{};
        bool start{}, scan{};
        {
            sync::Lock guard{lock_};
            slot = find_slot(id);
            if (!slot || slot->anchor.phase_ != anchor::phase::live) return false;
            auto& anchor = slot->anchor;
            scan = anchor.gate_.close();
            anchor.phase_ = anchor::phase::closing;
            libk_assert(anchor.refs_ != std::numeric_limits<usize>::max());
            ++anchor.refs_;
            start = anchor.gate_.empty();
            if (start) anchor.phase_ = anchor::phase::draining;
        }
        if (start) finish_retire(*slot, id.generation);
        else if (scan) {
            queue_scan(slot->anchor);
        }
        return true;
    }

  public:
    [[nodiscard]] auto live_count() const noexcept -> usize {
        sync::Lock guard{lock_};
        return storage_.live();
    }

  private:
    void finish_retire(Slot& slot, u64 generation) noexcept {
        // Membership remains visible until the payload's real cleanup ACK.
        cleanup done{slot.anchor, generation};
        if constexpr (requires(T& value, cleanup&& token) { traits<T>::retire(value, std::move(token)); }) {
            traits<T>::retire(*slot.object(), std::move(done));
        } else {
            traits<T>::retire(*slot.object());
            done.complete();
        }
    }

    [[nodiscard]] auto find_slot(Key id) noexcept -> Slot* {
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
                    const auto id = new_id<anchor>();
                    if (!id.valid()) {
                        storage_.release(*entry.slot, false);
                        return std::unexpected(error::exhausted);
                    }
                    entry.slot->anchor.id_ = id;
                    auto& anchor = entry.slot->anchor;
                    anchor.generation_ = entry.generation;
                    anchor.refs_ = 0;
                    anchor.queued_ = false;
                    anchor.scanning_ = false;
                    anchor.gate_.reset();
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
        libk_assert(anchor.generation_ == generation);
        const auto before = anchor.refs_.fetch_add(1, std::memory_order_relaxed);
        libk_assert(before && before != std::numeric_limits<usize>::max());
    }

    void release_ref(anchor& anchor, u64 generation) noexcept {
        auto refs = anchor.refs_.load(std::memory_order_relaxed);
        while (refs > 1)
            if (anchor.refs_.compare_exchange_weak(refs, refs - 1,
                    std::memory_order_release, std::memory_order_relaxed)) return;
        bool queued{}, retire{}, scan{};
        {
            sync::Lock guard{lock_};
            libk_assert(anchor.owner_ == this && anchor.generation_ == generation);
            refs = anchor.refs_.load(std::memory_order_acquire);
            for (;;) {
                libk_assert(refs);
                if (refs == 1 && anchor.phase_ == anchor::phase::live) break;
                if (anchor.refs_.compare_exchange_weak(refs, refs - 1,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) break;
            }
            if (refs == 1 && anchor.phase_ == anchor::phase::live) {
                scan = anchor.gate_.close();
                anchor.phase_ = anchor::phase::closing;
                // Transfer this last Ref to cleanup without a transient zero:
                // an installed Link may concurrently retain its pinned storage.
                auto& slot = slot_of(anchor);
                if (anchor.gate_.empty()) {
                    anchor.queued_ = true;
                    slot.next_reclaim = reclaim_head_;
                    reclaim_head_ = &slot;
                    retire = true;
                }
            } else queued = queue_reclaim_if_ready(slot_of(anchor));
        }
        if (scan) {
            queue_scan(anchor);
        }
        if (retire || queued) notify_cleanup();
    }

    void release_use(anchor& anchor, u64 generation) noexcept {
        if (!anchor.gate_.leave()) return;
        bool queued{};
        {
            sync::Lock guard{lock_};
            libk_assert(anchor.generation_ == generation);
            // The zero transition and reclaim decision share the pool lock:
            // no worker may recycle this slot while this exit still uses it.
            if (!anchor.gate_.finish_leave()) return;
            if (anchor.phase_ == anchor::phase::closing && !anchor.queued_) {
                auto& slot = slot_of(anchor);
                anchor.queued_ = queued = true;
                slot.next_reclaim = reclaim_head_;
                reclaim_head_ = &slot;
            }
        }
        if (queued) notify_cleanup();
    }

    void finish_cleanup(anchor& anchor, u64 generation) noexcept {
        slot_of(anchor).member->detach();
        bool queued{};
        {
            sync::Lock guard{lock_};
            libk_assert(anchor.owner_ == this && anchor.generation_ == generation);
            libk_assert(anchor.phase_ == anchor::phase::draining && anchor.refs_ != 0);
            anchor.phase_ = anchor::phase::quiescent;
            --anchor.refs_;
            queued = queue_reclaim_if_ready(slot_of(anchor));
        }
        if (queued) notify_cleanup();
    }

    static void anchor_retain(void* owner, anchor& anchor, u64 generation) noexcept {
        static_cast<pool*>(owner)->retain(anchor, generation);
    }

    static auto anchor_live(void* owner, anchor& anchor, u64 generation) noexcept -> bool {
        (void)owner;
        return anchor.generation_ == generation && anchor.gate_.open();
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
        if (anchor.gate_.close()) {
            storage.queue_scan(anchor);
        }
        // The grant operation prevents managed-member detach until this call
        // returns; its target reference also pins the slot.
        if (member.close()) return true;
        return storage.request_retire({reinterpret_cast<usize>(&anchor), generation});
    }

    [[nodiscard]] static auto anchor_request_retire(void* owner, anchor& anchor, u64 generation) noexcept
        -> bool {
        if (anchor.generation_ != generation) {
            return false;
        }
        return static_cast<pool*>(owner)->request_retire(Key{
            .slot = reinterpret_cast<usize>(&anchor),
            .generation = generation,
        });
    }

    static void anchor_finish_cleanup(void* owner, anchor& anchor, u64 generation) noexcept {
        static_cast<pool*>(owner)->finish_cleanup(anchor, generation);
    }

    static void anchor_release_use(void* owner, anchor& anchor, u64 generation) noexcept {
        static_cast<pool*>(owner)->release_use(anchor, generation);
    }

    static auto anchor_control(anchor& anchor) noexcept -> Control* {
        if constexpr (requires(T& value) { value.control(); }) return &slot_of(anchor).object()->control();
        else return nullptr;
    }

    static auto anchor_allocation(anchor& anchor) noexcept -> resource::Allocation* { return &slot_of(anchor).allocation; }

    inline static constexpr anchor::Ops anchor_ops_{
        .retain = anchor_retain,
        .live = anchor_live,
        .try_retain = anchor_try_retain,
        .drop_ref = anchor_drop_ref,
        .payload = payload,
        .destroy = destroy,
        .request_retire = anchor_request_retire,
        .finish_cleanup = anchor_finish_cleanup,
        .release_use = anchor_release_use,
        .control = anchor_control,
        .allocation = anchor_allocation,
    };

    [[nodiscard]] auto queue_reclaim_if_ready(Slot& slot) noexcept -> bool {
        anchor& anchor = slot.anchor;
        if (anchor.phase_ != anchor::phase::quiescent || anchor.refs_ != 0 ||
            anchor.queued_) {
            return false;
        }
        anchor.queued_ = true;
        slot.next_reclaim = reclaim_head_;
        reclaim_head_ = &slot;
        return true;
    }

    // Only the worker or exclusive pool teardown runs finalizers.
    void queue_scan(anchor& target) noexcept {
        {
            sync::Lock guard{lock_};
            libk_assert(!target.queued_ && !target.scanning_ && target.refs_ && target.refs_ != std::numeric_limits<usize>::max());
            ++target.refs_; // The queued scan owns storage as well as its Gate use.
            target.scanning_ = target.queued_ = true;
            auto& slot = slot_of(target);
            slot.next_reclaim = reclaim_head_; reclaim_head_ = &slot;
        }
        notify_cleanup();
    }

    void drain(usize budget = std::numeric_limits<usize>::max()) noexcept {
        for (usize i = 0; i < budget; ++i) {
            Slot* slot{};
            bool retire{}, scan{};
            {
                sync::Lock guard{lock_};
                slot = reclaim_head_;
                if (slot == nullptr) return;
                reclaim_head_ = slot->next_reclaim;
                slot->next_reclaim = nullptr;
                slot->anchor.queued_ = false;
                scan = slot->anchor.scanning_;
                retire = !scan && slot->anchor.phase_ == anchor::phase::closing;
                if (retire) {
                    libk_assert(slot->anchor.refs_ && slot->anchor.gate_.empty());
                    slot->anchor.phase_ = anchor::phase::draining;
                } else if (!scan) libk_assert(slot->anchor.phase_ == anchor::phase::quiescent && slot->anchor.refs_ == 0);
            }
            if (scan) {
                auto& target = slot->anchor;
                const auto generation = target.generation_;
                if (target.gate_.invalidate(8)) {
                    { sync::Lock guard{lock_}; target.scanning_ = false; }
                    release_use(target, generation);
                    release_ref(target, generation);
                } else {
                    sync::Lock guard{lock_};
                    libk_assert(!target.queued_);
                    target.queued_ = true;
                    slot->next_reclaim = reclaim_head_; reclaim_head_ = slot;
                }
                continue;
            }
            if (retire) {
                finish_retire(*slot, slot->anchor.generation_);
                continue;
            }
            slot->anchor.index(false);
            auto release = slot->allocation.detach();
            traits<T>::destroy(*slot->object());
            finalize_free(*slot);
            release.complete();
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
// pools share finite work steps claimed by the CPU maintenance runners.
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
