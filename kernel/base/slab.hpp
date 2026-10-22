#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <libk/intrusive_tree.hpp>
#include <libk/intrusive_list.hpp>

namespace base {

// Storage only: the owner supplies backing, locking and payload lifetime.
// Generations belong to the pool, so returning a page cannot resurrect an id.
template<class Slot, class Backing, std::size_t Bytes, class Extra = std::tuple<>>
class slab {
public:
    struct page {
        explicit page(Backing&& block) noexcept : backing(std::move(block)) {}
        Backing backing;
        [[no_unique_address]] Extra extra{};
        libk::intrusive_tree_hook tree;
        libk::intrusive_list_hook available;
        Slot* free{};
        std::size_t live{};
    };
    struct entry {
        Slot* slot{};
        std::uint64_t generation{};
    };

    slab() = default;
    slab(const slab&) = delete;
    auto operator=(const slab&) -> slab& = delete;
    slab(slab&& other) noexcept
        : index_(std::move(other.index_)), available_(std::move(other.available_)),
          live_(std::exchange(other.live_, 0)), next_(other.next_) {}

    static constexpr auto offset() noexcept -> std::size_t {
        return (sizeof(page) + alignof(Slot) - 1) & ~(alignof(Slot) - 1);
    }
    static constexpr auto capacity() noexcept -> std::size_t {
        return (Bytes - offset()) / sizeof(Slot);
    }
    static auto slots(page& block) noexcept -> std::span<Slot> {
        return {reinterpret_cast<Slot*>(reinterpret_cast<std::byte*>(&block)
            + offset()), capacity()};
    }
    static auto make(Backing&& backing, auto init) noexcept -> page* {
        static_assert(offset() < Bytes && capacity() != 0);
        static_assert(alignof(Slot) <= Bytes);
        auto* block = std::construct_at(reinterpret_cast<page*>(backing.bytes()),
                                       std::move(backing));
        auto storage = slots(*block);
        for (auto i = storage.size(); i != 0; --i) {
            auto* slot = std::construct_at(&storage[i - 1]);
            init(*slot);
            slot->page = block;
            slot->next_free = block->free;
            block->free = slot;
        }
        return block;
    }
    // The owner has drained all slots and detached any usage in extra.
    static auto dispose(page& block) noexcept -> Backing {
        for (auto& slot : slots(block)) std::destroy_at(&slot);
        auto backing = std::move(block.backing);
        std::destroy_at(&block);
        return backing;
    }

    void add(page& block) noexcept {
        index_.insert(block);
        if (block.free) available_.push_front(block);
    }
    auto take_page() noexcept -> page* {
        auto* block = index_.minimum();
        if (block) {
            index_.erase(*block);
            if (block->available.is_linked()) available_.erase(*block);
        }
        return block;
    }
    auto claim() noexcept -> entry {
        if (exhausted() || available_.empty()) return {};
        auto& block = available_.front();
        auto* slot = block.free;
        block.free = slot->next_free;
        if (!block.free) available_.erase(block);
        slot->next_free = nullptr;
        ++block.live; ++live_;
        return {slot, next_++};
    }
    // The slot's trusted back pointer avoids a pool search on release.
    auto release(Slot& slot, bool trim = true) noexcept -> page* {
        auto& block = *slot.page;
        --block.live; --live_;
        if (!block.free) available_.push_front(block);
        slot.next_free = block.free; block.free = &slot;
        if (!trim || block.live) return nullptr;
        available_.erase(block); index_.erase(block);
        return &block;
    }
    auto find(std::uintptr_t address) const noexcept -> Slot* {
        // The index proves membership before any candidate address is read.
        const auto* block = index_.find(address);
        if (!block) return nullptr;
        const auto first = reinterpret_cast<std::uintptr_t>(block) + offset();
        if (address < first || address - first >= capacity() * sizeof(Slot)) return nullptr;
        const auto delta = address - first;
        return delta % sizeof(Slot) ? nullptr : reinterpret_cast<Slot*>(address);
    }
    auto head() const noexcept -> page* { return const_cast<page*>(index_.minimum()); }
    auto next(const page& block) const noexcept -> page* { return const_cast<page*>(index_.next(block)); }
    auto pages() const noexcept -> std::size_t { return index_.size(); }
    auto live() const noexcept -> std::size_t { return live_; }
    auto exhausted() const noexcept -> bool {
        return next_ == std::numeric_limits<std::uint64_t>::max();
    }

private:
    struct Order {
        bool operator()(const page& a, const page& b) const noexcept {
            return reinterpret_cast<std::uintptr_t>(&a) < reinterpret_cast<std::uintptr_t>(&b);
        }
        bool operator()(const page& a, std::uintptr_t b) const noexcept {
            const auto first = reinterpret_cast<std::uintptr_t>(&a);
            return b >= first && b - first >= Bytes;
        }
        bool operator()(std::uintptr_t a, const page& b) const noexcept {
            return a < reinterpret_cast<std::uintptr_t>(&b);
        }
    };
    libk::intrusive_tree<page, &page::tree, Order> index_;
    libk::intrusive_list<page, &page::available> available_;
    std::size_t live_{};
    std::uint64_t next_{1};
};

} // namespace base
