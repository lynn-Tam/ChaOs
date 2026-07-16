#pragma once

#include <core/debug.hpp>
#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/limits.hpp>
#include <libk/memory.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <libk/utility.hpp>
#include <mm/pmm.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

struct StableNodeKey final {
    usize slot{};
    u64 generation{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return slot != 0 && generation != 0;
    }
    [[nodiscard]] friend constexpr auto operator==(
        StableNodeKey, StableNodeKey) noexcept -> bool = default;
};

enum class NodePoolError : u8 {
    OutOfMemory,
    QuotaExceeded,
    GenerationExhausted,
};

// Stable page-backed storage for semantic nodes that are owned by a kernel
// object but are not independently capability-addressable ObjectStore objects.
// The pool owns storage/generation only; the enclosing subsystem remains the
// lifecycle and synchronization owner of T.
template<typename T>
class NodePool final : private libk::noncopyable_nonmovable {
    struct PageHeader;

    struct Slot final {
        PageHeader* page{};
        Slot* next_free{};
        u64 generation{};
        bool occupied{};
        bool quarantined{};
        alignas(T) byte storage[sizeof(T)]{};

        [[nodiscard]] auto object() noexcept -> T* {
            return reinterpret_cast<T*>(storage);
        }
        [[nodiscard]] auto object() const noexcept -> const T* {
            return reinterpret_cast<const T*>(storage);
        }
    };

    struct PageHeader final {
        explicit PageHeader(OwnedPage&& page) noexcept
            : backing(libk::move(page)) {}

        OwnedPage backing;
        PageHeader* next{};
        Slot* free_head{};
        usize live_count{};
        usize quarantined_count{};
    };

    static constexpr usize slot_offset =
        (sizeof(PageHeader) + alignof(Slot) - 1) & ~(alignof(Slot) - 1);
    static constexpr usize slots_per_page =
        (page_size - slot_offset) / sizeof(Slot);

public:
    struct Quota final {
        usize nodes{4096};
        usize pages{64};
    };

    struct Entry final {
        T* object{};
        StableNodeKey key{};
    };

    explicit NodePool(Pmm& pmm, Quota quota = {}) noexcept
        : pmm_(&pmm), quota_(quota) {
        static_assert(slots_per_page != 0);
        static_assert(alignof(T) <= page_size);
    }

    ~NodePool() noexcept {
        KASSERT(live_count_ == 0);
        while (pages_ != nullptr) {
            PageHeader* const page = pages_;
            pages_ = page->next;
            KASSERT(page_count_ != 0);
            --page_count_;
            release_page(*page);
        }
        KASSERT(page_count_ == 0);
    }

    template<typename... Args>
    [[nodiscard]] auto create(Args&&... args) noexcept
        -> libk::Expected<Entry, NodePoolError> {
        auto claimed = claim();
        if (!claimed) {
            return libk::unexpected(claimed.error());
        }
        Slot* const slot = claimed.value();
        T* const object = libk::construct_at(
            slot->object(), libk::forward<Args>(args)...);
        return libk::expected(Entry{
            object,
            StableNodeKey{
                reinterpret_cast<usize>(slot), slot->generation},
        });
    }

    // The caller first removes the node from every subsystem index. T is
    // destroyed outside the pool lock so payload cleanup cannot invert locks.
    void destroy(T& object) noexcept {
        Slot& slot = slot_of(object);
        KASSERT(slot.occupied);
        libk::destroy_at(&object);

        PageHeader* release{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(slot.occupied);
            slot.occupied = false;
            KASSERT(slot.page->live_count != 0);
            --slot.page->live_count;
            KASSERT(live_count_ != 0);
            --live_count_;
            if (!slot.quarantined) {
                slot.next_free = slot.page->free_head;
                slot.page->free_head = &slot;
            }
            if (slot.page->live_count == 0) {
                PageHeader** link = &pages_;
                while (*link != nullptr && *link != slot.page) {
                    link = &(*link)->next;
                }
                KASSERT(*link == slot.page);
                *link = slot.page->next;
                release = slot.page;
                KASSERT(page_count_ != 0);
                --page_count_;
                KASSERT(quarantined_ >= release->quarantined_count);
                quarantined_ -= release->quarantined_count;
                release->quarantined_count = 0;
            }
        }
        if (release != nullptr) {
            release_page(*release);
        }
    }

    // Returned storage is stable while the enclosing subsystem lock prevents
    // destroy(). NodePool does not manufacture a second operation-pin domain.
    [[nodiscard]] auto find(StableNodeKey key) noexcept -> T* {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!key.valid()) {
            return nullptr;
        }
        for (PageHeader* page = pages_; page != nullptr; page = page->next) {
            Slot* const first = slots(*page);
            const usize base = reinterpret_cast<usize>(first);
            const usize bytes = slots_per_page * sizeof(Slot);
            if (key.slot < base || key.slot - base >= bytes) {
                continue;
            }
            const usize displacement = key.slot - base;
            if (displacement % sizeof(Slot) != 0) {
                return nullptr;
            }
            Slot& slot = first[displacement / sizeof(Slot)];
            return slot.occupied && slot.generation == key.generation
                ? slot.object()
                : nullptr;
        }
        return nullptr;
    }

    [[nodiscard]] auto key_of(const T& object) const noexcept
        -> StableNodeKey {
        const Slot& slot = slot_of(object);
        KASSERT(slot.occupied);
        return StableNodeKey{
            reinterpret_cast<usize>(&slot), slot.generation};
    }

    [[nodiscard]] auto live_count() const noexcept -> usize {
        kernel::sync::IrqLockGuard guard{lock_};
        return live_count_;
    }

private:
    [[nodiscard]] static auto slots(PageHeader& page) noexcept -> Slot* {
        return reinterpret_cast<Slot*>(
            reinterpret_cast<usize>(&page) + slot_offset);
    }

    [[nodiscard]] static auto slot_of(T& object) noexcept -> Slot& {
        return *reinterpret_cast<Slot*>(
            reinterpret_cast<byte*>(&object)
            - __builtin_offsetof(Slot, storage));
    }
    [[nodiscard]] static auto slot_of(const T& object) noexcept -> const Slot& {
        return *reinterpret_cast<const Slot*>(
            reinterpret_cast<const byte*>(&object)
            - __builtin_offsetof(Slot, storage));
    }

    [[nodiscard]] auto claim() noexcept
        -> libk::Expected<Slot*, NodePoolError> {
        for (;;) {
            {
                kernel::sync::IrqLockGuard guard{lock_};
                if (live_count_ >= quota_.nodes) {
                    return libk::unexpected(NodePoolError::QuotaExceeded);
                }
                for (PageHeader* page = pages_;
                     page != nullptr;
                     page = page->next) {
                    while (page->free_head != nullptr) {
                        Slot* const slot = page->free_head;
                        page->free_head = slot->next_free;
                        slot->next_free = nullptr;
                        if (slot->generation
                            == libk::numeric_limits<u64>::max()) {
                            slot->quarantined = true;
                            ++quarantined_;
                            ++slot->page->quarantined_count;
                            continue;
                        }
                        ++slot->generation;
                        slot->occupied = true;
                        ++slot->page->live_count;
                        ++live_count_;
                        return libk::expected(slot);
                    }
                }
                if (page_count_ + growing_ >= quota_.pages) {
                    return libk::unexpected(
                        live_count_ == 0 && quarantined_ != 0
                            ? NodePoolError::GenerationExhausted
                            : NodePoolError::QuotaExceeded);
                }
                ++growing_;
            }

            auto made = make_page();
            if (!made) {
                kernel::sync::IrqLockGuard guard{lock_};
                KASSERT(growing_ != 0);
                --growing_;
                return libk::unexpected(made.error());
            }
            PageHeader* const page = made.value();
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(growing_ != 0);
            --growing_;
            ++page_count_;
            page->next = pages_;
            pages_ = page;
        }
    }

    [[nodiscard]] auto make_page() noexcept
        -> libk::Expected<PageHeader*, NodePoolError> {
        auto allocated = pmm_->allocate_page();
        if (!allocated) {
            return libk::unexpected(NodePoolError::OutOfMemory);
        }
        OwnedPage backing = libk::move(allocated).value();
        auto* const page = libk::construct_at(
            reinterpret_cast<PageHeader*>(backing.bytes()),
            libk::move(backing));
        Slot* const storage = slots(*page);
        for (usize index = slots_per_page; index > 0; --index) {
            Slot* const slot = libk::construct_at(&storage[index - 1]);
            slot->page = page;
            slot->next_free = page->free_head;
            page->free_head = slot;
        }
        return libk::expected(page);
    }

    void release_page(PageHeader& page) noexcept {
        KASSERT(page.live_count == 0);
        Slot* const storage = slots(page);
        for (usize index = 0; index < slots_per_page; ++index) {
            KASSERT(!storage[index].occupied);
            libk::destroy_at(&storage[index]);
        }
        OwnedPage backing = libk::move(page.backing);
        libk::destroy_at(&page);
        backing.reset();
    }

    Pmm* pmm_{};
    Quota quota_{};
    mutable libk::TicketSpinLock lock_{};
    PageHeader* pages_{};
    usize page_count_{};
    usize live_count_{};
    usize quarantined_{};
    usize growing_{};
};

} // namespace kernel::mm
