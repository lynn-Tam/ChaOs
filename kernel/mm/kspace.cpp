#include <expected>
#include <mm/kspace.hpp>
#include <resource/quota.hpp>
#include <utility>

#include <libk/types.hpp>
#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <sync.hpp>

namespace mm {

KSpace::~KSpace() noexcept { libk_assert(stack_leases_ == 0); }

auto KSpace::map(KMap& map, Pages pages) noexcept -> std::expected<void, PtErr> {
    if (map.owner_ || !pages.valid()) return std::unexpected(PtErr::BadAddr);
    sync::Lock guard{stack_lock_};
    // Device reservations grow downward; stacks grow upward in the same arena.
    usize end = stack_end_;
    auto* link = &devices_;
    for (; *link; link = &(*link)->next_) {
        const auto& range = (*link)->range_;
        if (pages.byte_size() <= end - range.limit()->raw()) break;
        end = range.base().raw();
    }
    if (end < next_stack_base_ || pages.byte_size() > end - next_stack_base_)
        return std::unexpected(PtErr::NoMemory);
    const auto va = VPage::from_base(va_t{end - pages.byte_size()});
    if (!va) return std::unexpected(PtErr::BadAddr);
    auto count = root_.count();
    for (usize n = 0; n < pages.page_count(); ++n)
        if (!count.include(*va->checked_add(n))) return std::unexpected(PtErr::BadAddr);
    auto fee = count.pages() ? resource::acquire(quota_, {.memory = count.pages() * page_size})
                             : std::expected<resource::Usage, resource::errc>{resource::Usage{}};
    if (!fee) return std::unexpected(PtErr::NoMemory);
    auto edit = tlb_.begin();
    const auto result = root_.map(va->base(), pages, PtPerm::Rw);
    if (!result) return result;
    // This gap has no live translation. Removal drains TLBs before reuse.
    edit.publish_fresh();
    table_charge_.merge(std::move(*fee));
    map.owner_ = this;
    map.range_ = {va->base(), pages.byte_size()};
    map.link_ = link;
    map.next_ = *link;
    if (map.next_) map.next_->link_ = &map.next_;
    *link = &map;
    return {};
}
void KSpace::unmap(KMap& map, Flush& flush, Cpus& cpus, CpuId local) noexcept {
    libk_assert(map.owner_ == this && !flush.submitted());
    sync::Lock guard{stack_lock_};
    flush.pages_ = pmm_->group();
    auto edit = tlb_.begin();
    for (usize offset = 0; offset < map.range_.size(); offset += page_size) {
        const auto page = VPage::from_base(va_t{map.address() + offset});
        auto removed = root_.unmap(*page);
        libk_assert(removed);
        for (auto& table : removed->tables)
            libk_assert(flush.adopt(std::move(table), table_charge_.split({.memory = page_size})));
    }
    // Keep the reservation linked until remote CPUs acknowledge the invalidation.
    (void) edit.commit(flush, &cpus, local, false, true);
}
void KSpace::release(KMap& map, const Flush& flush) noexcept {
    libk_assert(map.owner_ == this && flush.complete());
    sync::Lock guard{stack_lock_};
    *map.link_ = map.next_;
    if (map.next_) map.next_->link_ = map.link_;
    map.owner_ = nullptr;
    map.link_ = nullptr;
    map.next_ = nullptr;
    map.range_ = {};
}
auto KSpace::acquire_stack() noexcept -> std::expected<usize, PtErr> {
    sync::Lock guard{stack_lock_};

    if (free_stack_ != 0) {
        const usize base = free_stack_;
        free_stack_ = stack_link(base);
        ++stack_leases_;
        return base;
    }

    const usize base = next_stack_base_;
    const auto slot_end = libk::checked_add(base - Stack::GuardPages * page_size, Stack::SlotBytes);
    usize device_begin = stack_end_;
    for (auto* map = devices_; map; map = map->next_) device_begin = map->range_.base().raw();
    if (!slot_end || *slot_end > device_begin) {
        return std::unexpected(PtErr::BadAddr);
    }

    PageGroup backing = pmm_->group();
    Page pages[Stack::StackPages]{};
    for (auto& page : pages) {
        auto made = backing.allocate();
        if (!made) return std::unexpected(PtErr::NoMemory);
        page = *made;
    }

    auto count = root_.count();
    const auto first = *VPage::from_base(va_t{base});
    for (usize i = 0; i < Stack::StackPages; ++i) libk_assert(count.include(*first.checked_add(i)));
    auto fee = count.pages() ? resource::acquire(quota_, {.memory = count.pages() * page_size})
                             : std::expected<resource::Usage, resource::errc>{resource::Usage{}};
    if (!fee) return std::unexpected(PtErr::NoMemory);
    auto edit = tlb_.begin();
    auto va = *VPage::from_base(va_t{base});
    usize i{};
    auto mapped = root_.map(va, Stack::StackPages, [&] { return pages[i++]; }, PtPerm::Rw);
    if (!mapped) return std::unexpected(mapped.error());
    edit.publish_fresh();

    table_charge_.merge(std::move(*fee));
    stack_pages_.append(std::move(backing));
    next_stack_base_ = *slot_end + Stack::GuardPages * page_size;
    ++stack_leases_;
    return base;
}

void KSpace::release_stack(usize base) noexcept {
    libk_assert(base >= mm::DynamicBegin + page_size);
    libk_assert((base & (page_size - 1)) == 0);
    sync::Lock guard{stack_lock_};
    libk_assert(stack_leases_ != 0);
    stack_link(base) = free_stack_;
    free_stack_ = base;
    --stack_leases_;
}

auto KSpace::stack_link(usize base) noexcept -> usize& {
    const auto virtual_page = VPage::from_base(va_t{base});
    libk_assert(virtual_page);
    auto& editor = root_;
    const auto mapped = editor.query(*virtual_page);
    libk_assert(mapped);
    libk_assert(stack_pages_.contains(mapped.value().page));
    return *reinterpret_cast<usize*>(stack_pages_.bytes(mapped.value().page));
}

auto Stack::create(KSpace& owner) noexcept -> std::expected<Stack, PtErr> {
    auto base = owner.acquire_stack();
    if (!base) return std::unexpected(base.error());
    return Stack{Slot{&owner, *base}};
}

void Stack::Drop::operator()(Slot& s) const noexcept { s.owner->release_stack(s.base); }

} // namespace mm
