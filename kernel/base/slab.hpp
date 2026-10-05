#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <tuple>
#include <utility>

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
        page* next{};
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
    // The owner has drained all slots and detached any charge in extra.
    static auto dispose(page& block) noexcept -> Backing {
        for (auto& slot : slots(block)) std::destroy_at(&slot);
        auto backing = std::move(block.backing);
        std::destroy_at(&block);
        return backing;
    }

    void add(page& block) noexcept {
        block.next = head_;
        head_ = &block;
        ++pages_;
    }
    auto take_page() noexcept -> page* {
        auto* block = head_;
        if (block) {
            head_ = block->next;
            block->next = nullptr;
            --pages_;
        }
        return block;
    }
    auto claim() noexcept -> entry {
        if (exhausted()) return {};
        for (auto* block = head_; block; block = block->next) {
            if (!block->free) continue;
            auto* slot = block->free;
            block->free = slot->next_free;
            slot->next_free = nullptr;
            ++block->live;
            ++live_;
            return {slot, next_++};
        }
        return {};
    }
    // An unlinked empty page is returned for destruction outside the owner lock.
    auto release(Slot& slot, bool trim = true) noexcept -> page* {
        auto* block = slot.page;
        --block->live;
        --live_;
        slot.next_free = block->free;
        block->free = &slot;
        if (!trim || block->live) return nullptr;
        auto** link = &head_;
        while (*link != block) link = &(*link)->next;
        *link = block->next;
        block->next = nullptr;
        --pages_;
        return block;
    }
    auto find(std::uintptr_t address) const noexcept -> Slot* {
        for (auto* block = head_; block; block = block->next) {
            const auto first = reinterpret_cast<std::uintptr_t>(slots(*block).data());
            const auto bytes = capacity() * sizeof(Slot);
            if (address >= first && address - first < bytes) {
                const auto delta = address - first;
                return delta % sizeof(Slot) == 0
                    ? &slots(*block)[delta / sizeof(Slot)] : nullptr;
            }
        }
        return nullptr;
    }
    auto head() const noexcept -> page* { return head_; }
    auto pages() const noexcept -> std::size_t { return pages_; }
    auto live() const noexcept -> std::size_t { return live_; }
    auto exhausted() const noexcept -> bool {
        return next_ == std::numeric_limits<std::uint64_t>::max();
    }

private:
    page* head_{};
    std::size_t pages_{};
    std::size_t live_{};
    std::uint64_t next_{1};
};

} // namespace base
