#include <expected>
#include <mm/kspace.hpp>
#include <utility>

#include <base/types.hpp>
#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <sync.hpp>

namespace mm {

KSpace::~KSpace() noexcept { libk_assert(stack_leases_ == 0); }

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
    if (!slot_end || *slot_end > stack_end_) {
        return std::unexpected(PtErr::BadAddr);
    }

    PageGroup backing = pmm_->group();
    Page pages[Stack::StackPages]{};
    for (auto& page : pages) {
        auto made = backing.allocate();
        if (!made) return std::unexpected(PtErr::NoMemory);
        page = *made;
    }

    auto edit = tlb_.begin();
    auto va = *VPage::from_base(Virt{base});
    usize i{};
    auto mapped = root_.map(va, Stack::StackPages, [&] { return pages[i++]; }, PtPerm::Rw);
    if (!mapped) return std::unexpected(mapped.error());
    edit.publish_fresh();

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
    const auto virtual_page = VPage::from_base(Virt{base});
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
