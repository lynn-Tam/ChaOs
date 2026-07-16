#pragma once

#include <core/debug.hpp>
#include <core/types.hpp>
#include <libk/delegate.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/unique_handle.hpp>
#include <libk/limits.hpp>
#include <libk/memory.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <libk/utility.hpp>
#include <mm/pmm.hpp>
#include <object/object_anchor.hpp>
#include <object/object_cleanup.hpp>
#include <object/object_id.hpp>
#include <object/object_ref.hpp>
#include <object/object_traits.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::object {

template<typename T>
class ObjectPool final : private libk::noncopyable_nonmovable {
    static_assert(StorableObject<T>);

    struct PageHeader;

    struct Slot final {
        ObjectAnchor anchor{};
        PageHeader* page{};
        Slot* next_free{};
        Slot* next_reclaim{};
        alignas(T) byte storage[sizeof(T)]{};

        [[nodiscard]] auto object() noexcept -> T* {
            return reinterpret_cast<T*>(storage);
        }
        [[nodiscard]] auto object() const noexcept -> const T* {
            return reinterpret_cast<const T*>(storage);
        }
    };

    static_assert(__builtin_offsetof(Slot, anchor) == 0);

    struct PageHeader final {
        explicit PageHeader(kernel::mm::OwnedPage&& owned) noexcept
            : backing(libk::move(owned)) {}

        kernel::mm::OwnedPage backing;
        PageHeader* next{};
        Slot* free_head{};
        usize free_count{};
        usize live_count{};
    };

    static constexpr usize slot_offset =
        (sizeof(PageHeader) + alignof(Slot) - 1) & ~(alignof(Slot) - 1);
    static constexpr usize slots_per_page =
        (kernel::mm::page_size - slot_offset) / sizeof(Slot);

public:
    using Error = ObjectError;

private:
    struct PendingRelease final {
        ObjectPool* pool{};
        void operator()(Slot*& slot) noexcept {
            pool->rollback(slot);
            slot = nullptr;
        }
    };

    using PendingToken = libk::UniqueHandle<Slot*, PendingRelease>;

public:
    using Hold = ObjectHold<T>;
    using Pin = ObjectPin<T>;

    class Pending final : private libk::noncopyable {
    public:
        Pending(Pending&&) noexcept = default;
        auto operator=(Pending&&) noexcept -> Pending& = default;

        [[nodiscard]] auto get() noexcept -> T& {
            KASSERT(token_);
            return *token_.get()->object();
        }

        [[nodiscard]] auto publish() noexcept -> Hold {
            KASSERT(token_);
            Slot* const slot = token_.release();
            return token_.get_deleter().pool->publish(slot);
        }

    private:
        friend class ObjectPool;
        Pending(ObjectPool& pool, Slot& slot) noexcept
            : token_(&slot, PendingRelease{&pool}) {}

        PendingToken token_{};
    };

    explicit ObjectPool(
        kernel::mm::Pmm& pmm,
        libk::delegate<void() noexcept>& reclaim_notify) noexcept
        : pmm_(&pmm), reclaim_notify_(&reclaim_notify) {
        static_assert(slots_per_page != 0);
        static_assert(alignof(T) <= kernel::mm::page_size);
    }

    ~ObjectPool() noexcept {
        drain_reclaim();
        KASSERT(live_objects_ == 0);
        while (pages_head_ != nullptr) {
            PageHeader* const page = pages_head_;
            pages_head_ = page->next;
            release_page(page);
        }
    }

    template<typename... Args>
    [[nodiscard]] auto create(Args&&... args) noexcept
        -> libk::Expected<Pending, Error> {
        auto claimed = claim_slot();
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
        Slot* const slot = claimed.value();
        libk::construct_at(slot->object(), libk::forward<Args>(args)...);
        return libk::expected(Pending{*this, *slot});
    }

    [[nodiscard]] auto hold(ObjectId id) noexcept
        -> libk::Expected<Hold, Error> {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_slot(id);
        if (slot == nullptr) {
            return libk::unexpected(Error::InvalidIdentity);
        }
        if (!add_ref_locked(slot->anchor, id.generation)) {
            return libk::unexpected(Error::InvalidLifecycle);
        }
        return libk::expected(make_hold(*slot));
    }

    [[nodiscard]] auto pin(ObjectId id) noexcept
        -> libk::Expected<Pin, Error> {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_slot(id);
        if (slot == nullptr) {
            return libk::unexpected(Error::InvalidIdentity);
        }
        void* const payload = pin_locked(slot->anchor, id.generation);
        if (payload == nullptr) {
            return libk::unexpected(Error::InvalidLifecycle);
        }
        return libk::expected(Pin{
            slot->anchor, *static_cast<T*>(payload), id.generation});
    }

    [[nodiscard]] auto request_retire(ObjectId id) noexcept -> bool {
        Slot* slot{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            slot = find_slot(id);
            if (slot == nullptr
                || slot->anchor.lifecycle_ != ObjectLifecycle::Live) {
                return false;
            }
            slot->anchor.lifecycle_ = ObjectLifecycle::Retiring;
            KASSERT(
                slot->anchor.active_pins_
                != libk::numeric_limits<usize>::max());
            ++slot->anchor.active_pins_;
        }

        if constexpr (requires(T& object) {
            ObjectTraits<T>::prepare_retire(object);
        }) {
            if (!ObjectTraits<T>::prepare_retire(*slot->object())) {
                kernel::sync::IrqLockGuard guard{lock_};
                KASSERT(slot->anchor.lifecycle_ == ObjectLifecycle::Retiring);
                KASSERT(slot->anchor.active_pins_ != 0);
                --slot->anchor.active_pins_;
                slot->anchor.lifecycle_ = ObjectLifecycle::Live;
                return false;
            }
        }

        ObjectCleanup cleanup{
            slot->anchor, slot->anchor.generation_};
        if constexpr (requires(T& object, ObjectCleanup&& token) {
            ObjectTraits<T>::retire(object, libk::move(token));
        }) {
            ObjectTraits<T>::retire(
                *slot->object(), libk::move(cleanup));
        } else {
            ObjectTraits<T>::retire(*slot->object());
            cleanup.complete();
        }
        return true;
    }

    // Finalizers run outside the pool lock. Only the designated reclaimer (or
    // exclusive test/teardown code) may drain; never hard interrupt or commit.
    void drain_reclaim() noexcept {
        for (;;) {
            Slot* slot{};
            {
                kernel::sync::IrqLockGuard guard{lock_};
                slot = reclaim_head_;
                if (slot == nullptr) {
                    return;
                }
                reclaim_head_ = slot->next_reclaim;
                slot->next_reclaim = nullptr;
                slot->anchor.reclaim_queued_ = false;
                KASSERT(
                    slot->anchor.lifecycle_ == ObjectLifecycle::Retiring);
                KASSERT(slot->anchor.cleanup_complete_);
                KASSERT(slot->anchor.strong_refs_ == 0);
                KASSERT(slot->anchor.active_pins_ == 0);
                slot->anchor.lifecycle_ = ObjectLifecycle::Quiescent;
            }

            ObjectTraits<T>::destroy(*slot->object());
            finalize_free(*slot);
        }
    }

    [[nodiscard]] auto live_count() const noexcept -> usize {
        kernel::sync::IrqLockGuard guard{lock_};
        return live_objects_;
    }

private:
    [[nodiscard]] static auto slots(PageHeader& page) noexcept -> Slot* {
        return reinterpret_cast<Slot*>(
            reinterpret_cast<usize>(&page) + slot_offset);
    }

    [[nodiscard]] static auto id_of(const Slot& slot) noexcept -> ObjectId {
        return ObjectId{
            .slot = reinterpret_cast<usize>(&slot.anchor),
            .generation = slot.anchor.generation_,
            .kind = slot.anchor.kind_,
        };
    }

    [[nodiscard]] auto find_slot(ObjectId id) noexcept -> Slot* {
        if (!id.valid() || id.kind != ObjectTraits<T>::kind) {
            return nullptr;
        }
        for (PageHeader* page = pages_head_; page != nullptr; page = page->next) {
            Slot* const first = slots(*page);
            const usize first_address = reinterpret_cast<usize>(first);
            const usize slots_bytes = slots_per_page * sizeof(Slot);
            if (id.slot < first_address
                || id.slot - first_address >= slots_bytes) {
                continue;
            }
            const usize displacement = id.slot - first_address;
            if (displacement % sizeof(Slot) != 0) {
                return nullptr;
            }
            auto* const candidate = reinterpret_cast<Slot*>(id.slot);
            if (candidate->anchor.generation_ != id.generation) {
                return nullptr;
            }
            return candidate;
        }
        return nullptr;
    }

    [[nodiscard]] auto claim_slot() noexcept
        -> libk::Expected<Slot*, Error> {
        for (;;) {
            {
                kernel::sync::IrqLockGuard guard{lock_};
                if (next_generation_ == libk::numeric_limits<u64>::max()) {
                    return libk::unexpected(Error::GenerationExhausted);
                }
                for (PageHeader* page = pages_head_;
                     page != nullptr;
                     page = page->next) {
                    if (page->free_head == nullptr) {
                        continue;
                    }
                    Slot* const slot = page->free_head;
                    page->free_head = slot->next_free;
                    slot->next_free = nullptr;
                    --page->free_count;
                    ++page->live_count;
                    ++live_objects_;
                    slot->anchor.generation_ = next_generation_++;
                    slot->anchor.strong_refs_ = 0;
                    slot->anchor.active_pins_ = 0;
                    slot->anchor.cleanup_complete_ = false;
                    slot->anchor.reclaim_queued_ = false;
                    slot->anchor.lifecycle_ = ObjectLifecycle::Constructing;
                    return libk::expected(slot);
                }
            }

            auto grown = make_page();
            if (!grown) {
                return libk::unexpected(grown.error());
            }
            PageHeader* const page = grown.value();
            kernel::sync::IrqLockGuard guard{lock_};
            page->next = pages_head_;
            pages_head_ = page;
        }
    }

    [[nodiscard]] auto make_page() noexcept
        -> libk::Expected<PageHeader*, Error> {
        auto allocation = pmm_->allocate_page();
        if (!allocation) {
            return libk::unexpected(Error::OutOfMemory);
        }
        kernel::mm::OwnedPage backing = libk::move(allocation).value();
        const usize base = reinterpret_cast<usize>(backing.bytes());
        auto* const page = libk::construct_at(
            reinterpret_cast<PageHeader*>(base),
            libk::move(backing));

        Slot* const storage = slots(*page);
        for (usize index = slots_per_page; index > 0; --index) {
            Slot* const slot = libk::construct_at(&storage[index - 1]);
            slot->anchor.owner_ = this;
            slot->anchor.ops_ = &anchor_ops_;
            slot->anchor.kind_ = ObjectTraits<T>::kind;
            slot->page = page;
            slot->next_free = page->free_head;
            page->free_head = slot;
            ++page->free_count;
        }
        return libk::expected(page);
    }

    [[nodiscard]] auto publish(Slot* slot) noexcept -> Hold {
        KASSERT(slot != nullptr);
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(
            slot->anchor.lifecycle_ == ObjectLifecycle::Constructing);
        KASSERT(slot->anchor.strong_refs_ == 0);
        slot->anchor.strong_refs_ = 1;
        slot->anchor.lifecycle_ = ObjectLifecycle::Live;
        return make_hold(*slot);
    }

    [[nodiscard]] auto add_hold(Slot& slot) noexcept
        -> libk::Expected<Hold, Error> {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!add_ref_locked(slot.anchor, slot.anchor.generation_)) {
            return libk::unexpected(Error::InvalidLifecycle);
        }
        return libk::expected(make_hold(slot));
    }

    [[nodiscard]] auto make_ref(Slot& slot) noexcept
        -> libk::Expected<ObjectRef, Error> {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!add_ref_locked(slot.anchor, slot.anchor.generation_)) {
            return libk::unexpected(Error::InvalidLifecycle);
        }
        return libk::expected(ObjectRef{
            slot.anchor, slot.anchor.generation_});
    }

    [[nodiscard]] static auto make_hold(Slot& slot) noexcept -> Hold {
        return Hold{
            ObjectRef{slot.anchor, slot.anchor.generation_},
            *slot.object()};
    }

    void rollback(Slot* slot) noexcept {
        KASSERT(slot != nullptr);
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(
                slot->anchor.lifecycle_ == ObjectLifecycle::Constructing);
            slot->anchor.lifecycle_ = ObjectLifecycle::Quiescent;
        }
        ObjectTraits<T>::destroy(*slot->object());
        finalize_free(*slot);
    }

    [[nodiscard]] static auto slot_of(ObjectAnchor& anchor) noexcept
        -> Slot& {
        return *reinterpret_cast<Slot*>(&anchor);
    }

    [[nodiscard]] auto add_ref_locked(
        ObjectAnchor& anchor,
        u64 generation) noexcept -> bool {
        KASSERT(anchor.owner_ == this);
        if (anchor.generation_ != generation
            || anchor.lifecycle_ != ObjectLifecycle::Live) {
            return false;
        }
        KASSERT(
            anchor.strong_refs_ != libk::numeric_limits<usize>::max());
        ++anchor.strong_refs_;
        return true;
    }

    [[nodiscard]] auto pin_locked(
        ObjectAnchor& anchor,
        u64 generation) noexcept -> void* {
        KASSERT(anchor.owner_ == this);
        if (anchor.generation_ != generation
            || anchor.lifecycle_ != ObjectLifecycle::Live) {
            return nullptr;
        }
        KASSERT(
            anchor.active_pins_ != libk::numeric_limits<usize>::max());
        ++anchor.active_pins_;
        return slot_of(anchor).object();
    }

    [[nodiscard]] auto try_ref(
        ObjectAnchor& anchor,
        u64 generation) noexcept -> bool {
        kernel::sync::IrqLockGuard guard{lock_};
        return add_ref_locked(anchor, generation);
    }

    [[nodiscard]] auto try_pin(
        ObjectAnchor& anchor,
        u64 generation) noexcept -> void* {
        kernel::sync::IrqLockGuard guard{lock_};
        return pin_locked(anchor, generation);
    }

    void release_ref(ObjectAnchor& anchor, u64 generation) noexcept {
        bool queued{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(anchor.owner_ == this);
            KASSERT(anchor.generation_ == generation);
            KASSERT(anchor.strong_refs_ != 0);
            --anchor.strong_refs_;
            queued = queue_reclaim_if_ready(slot_of(anchor));
        }
        if (queued) {
            notify_reclaimer();
        }
    }

    void release_pin(ObjectAnchor& anchor, u64 generation) noexcept {
        bool queued{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(anchor.owner_ == this);
            KASSERT(anchor.generation_ == generation);
            KASSERT(anchor.active_pins_ != 0);
            --anchor.active_pins_;
            queued = queue_reclaim_if_ready(slot_of(anchor));
        }
        if (queued) {
            notify_reclaimer();
        }
    }

    void finish_cleanup(ObjectAnchor& anchor, u64 generation) noexcept {
        bool queued{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(anchor.owner_ == this);
            KASSERT(anchor.generation_ == generation);
            KASSERT(anchor.lifecycle_ == ObjectLifecycle::Retiring);
            KASSERT(!anchor.cleanup_complete_);
            KASSERT(anchor.active_pins_ != 0);
            anchor.cleanup_complete_ = true;
            --anchor.active_pins_;
            queued = queue_reclaim_if_ready(slot_of(anchor));
        }
        if (queued) {
            notify_reclaimer();
        }
    }

    [[nodiscard]] static auto anchor_try_ref(
        void* owner,
        ObjectAnchor& anchor,
        u64 generation) noexcept -> bool {
        return static_cast<ObjectPool*>(owner)->try_ref(anchor, generation);
    }

    static void anchor_drop_ref(
        void* owner,
        ObjectAnchor& anchor,
        u64 generation) noexcept {
        static_cast<ObjectPool*>(owner)->release_ref(anchor, generation);
    }

    [[nodiscard]] static auto anchor_try_pin(
        void* owner,
        ObjectAnchor& anchor,
        u64 generation) noexcept -> void* {
        return static_cast<ObjectPool*>(owner)->try_pin(anchor, generation);
    }

    static void anchor_drop_pin(
        void* owner,
        ObjectAnchor& anchor,
        u64 generation) noexcept {
        static_cast<ObjectPool*>(owner)->release_pin(anchor, generation);
    }

    [[nodiscard]] static auto anchor_request_retire(
        void* owner,
        ObjectAnchor& anchor,
        u64 generation) noexcept -> bool {
        if (anchor.generation_ != generation) {
            return false;
        }
        return static_cast<ObjectPool*>(owner)->request_retire(
            ObjectId{
                .slot = reinterpret_cast<usize>(&anchor),
                .generation = generation,
                .kind = anchor.kind_,
            });
    }

    static void anchor_finish_cleanup(
        void* owner,
        ObjectAnchor& anchor,
        u64 generation) noexcept {
        static_cast<ObjectPool*>(owner)->finish_cleanup(anchor, generation);
    }

    inline static constexpr ObjectAnchor::Ops anchor_ops_{
        .try_ref = anchor_try_ref,
        .drop_ref = anchor_drop_ref,
        .try_pin = anchor_try_pin,
        .drop_pin = anchor_drop_pin,
        .request_retire = anchor_request_retire,
        .finish_cleanup = anchor_finish_cleanup,
    };

    [[nodiscard]] auto queue_reclaim_if_ready(Slot& slot) noexcept -> bool {
        ObjectAnchor& anchor = slot.anchor;
        if (anchor.lifecycle_ != ObjectLifecycle::Retiring
            || !anchor.cleanup_complete_
            || anchor.strong_refs_ != 0
            || anchor.active_pins_ != 0
            || anchor.reclaim_queued_) {
            return false;
        }
        anchor.reclaim_queued_ = true;
        slot.next_reclaim = reclaim_head_;
        reclaim_head_ = &slot;
        return true;
    }

    void notify_reclaimer() noexcept {
        KASSERT(reclaim_notify_ != nullptr);
        if (*reclaim_notify_) {
            (*reclaim_notify_)();
        }
    }

    void finalize_free(Slot& slot) noexcept {
        PageHeader* release{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(
                slot.anchor.lifecycle_ == ObjectLifecycle::Quiescent);
            PageHeader& page = *slot.page;
            KASSERT(page.live_count != 0);
            KASSERT(live_objects_ != 0);
            --page.live_count;
            --live_objects_;

            slot.anchor.lifecycle_ = ObjectLifecycle::Free;
            slot.anchor.cleanup_complete_ = false;
            slot.next_free = page.free_head;
            page.free_head = &slot;
            ++page.free_count;

            if (page.live_count == 0) {
                PageHeader** link = &pages_head_;
                while (*link != nullptr && *link != &page) {
                    link = &(*link)->next;
                }
                KASSERT(*link == &page);
                *link = page.next;
                release = &page;
            }
        }
        if (release != nullptr) {
            release_page(release);
        }
    }

    void release_page(PageHeader* page) noexcept {
        KASSERT(page != nullptr);
        KASSERT(page->live_count == 0);
        kernel::mm::OwnedPage backing = libk::move(page->backing);
        libk::destroy_at(page);
        backing.reset();
    }

    kernel::mm::Pmm* pmm_{};
    libk::delegate<void() noexcept>* reclaim_notify_{};
    mutable libk::TicketSpinLock lock_{};
    PageHeader* pages_head_{};
    Slot* reclaim_head_{};
    u64 next_generation_{1};
    usize live_objects_{};
};

} // namespace kernel::object
