#pragma once

#include <base/slab.hpp>
#include <mm/pmm.hpp>
#include <resource/quota.hpp>
#include <memory>
#include <type_traits>

namespace mm {

enum class SlabErr : u8 {
    OutOfMemory,
    QuotaExceeded,
    GenerationExhausted,
    ResourceExhausted,
};

struct SlabQuota {
    usize nodes{4096};
    usize pages{64};
};

// The caller serializes payload access; this pool owns only storage and ids.
template <class T, bool Indexed = true, bool Trim = true> class Slab final {
    struct Slot;
    using Storage = base::slab<Slot, OwnedPage, page_size, resource::Usage>;
    using PageHeader = typename Storage::page;
    struct Id {
        u64 generation{};
        bool occupied{};
    };
    struct Empty {};
    struct Slot {
        PageHeader* page{};
        Slot* next_free{};
        [[no_unique_address]] std::conditional_t<Indexed, Id, Empty> id{};
        alignas(T) byte storage[sizeof(T)]{};
        auto object() noexcept -> T* { return reinterpret_cast<T*>(storage); }
    };

  public:
    using Quota = SlabQuota;
    struct Entry {
        T* object{};
        libk::key<T> key{};
    };
    static constexpr auto quota_for(usize nodes) noexcept -> Quota {
        constexpr auto count = Storage::capacity();
        return {nodes, nodes / count + (nodes % count != 0)};
    }
    static constexpr auto slot_size() noexcept -> usize { return sizeof(Slot); }
    explicit Slab(Pmm& pmm, Quota quota = {~usize{}, ~usize{}}) noexcept
        : pmm_(&pmm), quota_(quota) {}
    Slab(const Slab&) = delete;
    auto operator=(const Slab&) -> Slab& = delete;
    // Exclusive ownership transfer: payload addresses and keys stay stable.
    // The moved-from allocator is empty and has no allocation quota.
    Slab(Slab&& other) noexcept
        : pmm_(other.pmm_), quota_(std::exchange(other.quota_, Quota{0, 0})),
          storage_(std::move(other.storage_)) {
        libk_assert(other.growing_ == 0);
    }
    ~Slab() noexcept {
        libk_assert(storage_.live() == 0 && growing_ == 0);
        while (!drain(8)) {}
    }
    // Exclusive terminal ownership: no records or in-flight allocation remain.
    auto drain(usize budget) noexcept -> bool {
        for (usize i = 0; i < budget; ++i) {
            PageHeader* page;
            {
                sync::Lock guard{lock_};
                libk_assert(storage_.live() == 0 && growing_ == 0);
                page = storage_.take_page();
            }
            if (!page) return true;
            release_page(*page);
        }
        sync::Lock guard{lock_};
        return storage_.pages() == 0;
    }
    using Result = std::conditional_t<Indexed, Entry, T*>;
    template <class... Args> auto create(const object::ref<>& quota, Args&&... args) noexcept -> std::expected<Result, SlabErr> {
        auto claimed = claim(quota);
        if (!claimed) return std::unexpected(claimed.error());
        auto* slot = claimed.value();
        auto* object = std::construct_at(slot->object(), std::forward<Args>(args)...);
        if constexpr (Indexed) return Entry{object, key_of(*object)};
        else return object;
    }
    // Reserve real backing and fees before accepting allocation-free work.
    auto prepay(const object::ref<>& quota, usize nodes) noexcept -> std::expected<bool, SlabErr>
        requires (!Trim) {
        if (nodes > quota_.nodes) return std::unexpected(SlabErr::QuotaExceeded);
        const auto pages = quota_for(nodes).pages;
        for (usize step = 0; step < 64; ++step) {
            {
                sync::Lock guard{lock_};
                if (storage_.pages() >= pages) return true;
                if (storage_.pages() + growing_ >= quota_.pages)
                    return std::unexpected(SlabErr::QuotaExceeded);
                ++growing_;
            }
            auto made = make_page(quota);
            sync::Lock guard{lock_};
            --growing_;
            if (!made) return std::unexpected(made.error());
            storage_.add(**made);
        }
        sync::Lock guard{lock_};
        return storage_.pages() >= pages;
    }
    void destroy(T& object) noexcept {
        auto& slot = slot_of(object);
        if constexpr (Indexed) libk_assert(slot.id.occupied);
        std::destroy_at(&object);
        PageHeader* page{};
        {
            sync::Lock guard{lock_};
            libk_assert(storage_.live() != 0);
            if constexpr (Indexed) slot.id.occupied = false;
            // Content metadata remains reusable across allocation failures.
            page = storage_.release(slot, Trim);
        }
        if (page) release_page(*page);
    }
    auto find(libk::key<T> key) noexcept -> T*
        requires Indexed
    {
        sync::Lock guard{lock_};
        if (!key.valid()) return nullptr;
        auto* slot = storage_.find(key.slot);
        return slot && slot->id.occupied && slot->id.generation == key.generation ? slot->object() : nullptr;
    }
    auto key_of(const T& object) const noexcept -> libk::key<T>
        requires Indexed
    {
        const auto& slot = slot_of(object);
        if constexpr (Indexed) libk_assert(slot.id.occupied);
        return {reinterpret_cast<usize>(&slot), slot.id.generation};
    }
    auto live_count() const noexcept -> usize {
        sync::Lock guard{lock_};
        return storage_.live();
    }
    template<class Visit> void each(Visit&& visit) noexcept requires Indexed {
        sync::Lock guard{lock_};
        libk_assert(!growing_);
        for (auto* page = storage_.head(); page; page = storage_.next(*page))
            for (auto& slot : Storage::slots(*page)) if (slot.id.occupied) visit(*slot.object());
    }
    template<class Visit> void charges(Visit&& visit) noexcept {
        sync::Lock guard{lock_};
        libk_assert(!growing_);
        for (auto* page = storage_.head(); page; page = storage_.next(*page)) visit(page->extra);
    }

  private:
    static auto slot_of(const T& object) noexcept -> const Slot& {
        return *reinterpret_cast<const Slot*>(reinterpret_cast<const byte*>(&object) -
                                              __builtin_offsetof(Slot, storage));
    }
    static auto slot_of(T& object) noexcept -> Slot& {
        return const_cast<Slot&>(slot_of(std::as_const(object)));
    }
    auto claim(const object::ref<>& quota) noexcept -> std::expected<Slot*, SlabErr> {
        for (;;) {
            {
                sync::Lock guard{lock_};
                if (storage_.exhausted()) return std::unexpected(SlabErr::GenerationExhausted);
                if (storage_.live() >= quota_.nodes) return std::unexpected(SlabErr::QuotaExceeded);
                if (auto entry = storage_.claim(); entry.slot) {
                    if constexpr (Indexed) entry.slot->id = {entry.generation, true};
                    return (entry.slot);
                }
                if (storage_.pages() + growing_ >= quota_.pages)
                    return std::unexpected(SlabErr::QuotaExceeded);
                ++growing_;
            }
            auto made = make_page(quota);
            sync::Lock guard{lock_};
            libk_assert(growing_ != 0);
            --growing_;
            if (!made) return std::unexpected(made.error());
            storage_.add(*made.value());
        }
    }
    auto make_page(const object::ref<>& quota) noexcept -> std::expected<PageHeader*, SlabErr> {
        auto usage = resource::acquire(quota, {.memory = page_size});
        if (!usage) return std::unexpected(SlabErr::ResourceExhausted);
        auto backing = pmm_->allocate_page();
        if (!backing) return std::unexpected(SlabErr::OutOfMemory);
        auto* page = Storage::make(std::move(backing).value(), [](Slot&) noexcept {});
        page->extra = std::move(*usage);
        return (page);
    }
    static void release_page(PageHeader& page) noexcept {
        libk_assert(page.live == 0);
        auto usage = std::move(page.extra);
        auto backing = Storage::dispose(page);
        backing.reset();
        usage.reset();
    }

    Pmm* pmm_;
    Quota quota_;
    mutable sync::Spin lock_{};
    Storage storage_{};
    usize growing_{};
};

} // namespace mm
