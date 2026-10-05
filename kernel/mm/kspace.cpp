#include <expected>
#include <mm/kspace.hpp>
#include <utility>

#include <base/types.hpp>
#include <boot/link.hpp>
#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <sync.hpp>

extern "C" {
extern char kernel_text_start[], kernel_text_end[];
extern char kernel_rodata_start[], kernel_rodata_end[];
extern char kernel_data_start[], kernel_data_end[];
extern char kernel_bss_start[], kernel_bss_end[];
extern char kernel_bootstack_start[], kernel_bootstack_end[];
}

namespace mm {

KSpace::~KSpace() noexcept { libk_assert(stack_leases_ == 0); }

auto KSpace::build_in(libk::ManualLifetime<KSpace>& storage, Pmm& pmm) noexcept -> InitResult {
    libk_assert(!storage);
    auto root = PageTable::create(pmm, PageTable::Kind::Kernel);
    if (!root) return std::unexpected(root.error());
    auto map = [&](mm::Virt va, mm::Pages pages, PtPerm perms) noexcept -> std::expected<void, PtErr> {
        auto first = mm::VPage::from_base(va);
        if (!first || !pages.valid()) return std::unexpected(PtErr::BadAddr);
        auto count = root->count();
        for (usize i = 0; i < pages.page_count(); ++i) {
            auto va = first->checked_add(i);
            if (!va || !count.include(*va)) return std::unexpected(PtErr::BadAddr);
        }
        auto reserve = pmm.group();
        if (!reserve.grow(count.pages())) return std::unexpected(PtErr::NoMemory);
        for (usize i = 0; i < pages.page_count(); ++i) {
            auto vpage = first->checked_add(i);
            auto page = pages.base().checked_add(i);
            if (!vpage || !page) return std::unexpected(PtErr::BadAddr);
            auto installed = root->map(*vpage, *page, perms, reserve);
            if (!installed) return installed;
        }
        return {};
    };
    const struct Section {
        const char* begin;
        const char* end;
        PtPerm perms;
    } sections[] = {
        {kernel_text_start, kernel_text_end, PtPerm::Rx},
        {kernel_rodata_start, kernel_rodata_end, PtPerm::Ro},
        {kernel_data_start, kernel_data_end, PtPerm::Rw},
        {kernel_bss_start, kernel_bss_end, PtPerm::Rw},
        {kernel_bootstack_start, kernel_bootstack_end, PtPerm::Rw},
    };
    for (const auto& s : sections) {
        const usize begin = reinterpret_cast<usize>(s.begin), end = reinterpret_cast<usize>(s.end);
        libk_assert(begin <= end && begin % mm::page_size == 0 && end % mm::page_size == 0);
        if (begin == end) continue;
        auto physical = kernel_phys(mm::Virt{begin});
        if (!physical) return std::unexpected(PtErr::BadPhys);
        auto pages = mm::Pages::from_aligned_bytes(*physical, end - begin);
        if (!pages) return std::unexpected(PtErr::BadPhys);
        auto installed = map(mm::Virt{begin}, *pages, s.perms);
        if (!installed) return std::unexpected(installed.error());
    }
    const auto& direct = pmm.direct_map();
    for (auto pages : direct.ranges()) {
        auto va = direct.map(pages.base().base(), pages.page_count() * mm::page_size);
        libk_assert(va);
        auto installed = map(*va, pages, PtPerm::Rw);
        if (!installed) return std::unexpected(installed.error());
    }
    // MMIO has the same immutable resource inventory, but is not allocatable RAM.
    for (const auto& region : pmm.regions()) {
        if (region.is_ram()) continue;
        const auto pages = region.range;
        auto installed = map(mm::Virt{mm::DirectBegin + pages.base().base().raw()}, pages, PtPerm::Rw);
        if (!installed) return std::unexpected(installed.error());
    }
    // Secondaries enter physically; these leaves bridge to their high entry.
    for (auto pages : {boot_pages(), secondary_pages()}) {
        auto installed = map(mm::Virt{pages.base().base().raw()}, pages, PtPerm::Rx);
        if (!installed) return std::unexpected(installed.error());
    }
    return adopt_in(storage, pmm, std::move(*root));
}

auto KSpace::adopt_in(libk::ManualLifetime<KSpace>& storage, Pmm& pmm, mm::PageTable&& root) noexcept
    -> InitResult {
    libk_assert(!storage);
    libk_assert(root.kind() == mm::PageTable::Kind::Kernel);
    [[maybe_unused]] auto& vspace = storage.emplace(pmm, std::move(root));
    return {};
}

auto KSpace::acquire_stack() noexcept -> std::expected<usize, Stack::Error> {
    sync::Lock guard{stack_lock_};

    if (free_stack_ != 0) {
        const usize base = free_stack_;
        free_stack_ = stack_link(base);
        ++stack_leases_;
        return base;
    }

    const usize base = next_stack_base_;
    const auto slot_end = libk::checked_add(base - Stack::GuardPages * page_size, Stack::SlotBytes);
    if (!slot_end || *slot_end > kernel_begin().raw()) {
        return std::unexpected(Stack::Error::AddressSpaceExhausted);
    }

    PageGroup backing = pmm_->group();
    Page pages[Stack::StackPages]{};
    {
        for (usize index = 0; index < Stack::StackPages; ++index) {
            auto page = backing.allocate();
            if (!page) {
                return std::unexpected(Stack::Error::OutOfMemory);
            }
            pages[index] = page.value();
        }
    }

    auto& editor = root_;
    auto plan = editor.count();
    for (usize index = 0; index < Stack::StackPages; ++index) {
        const auto virtual_page = VPage::from_base(Virt{base + index * page_size});
        libk_assert(virtual_page && plan.include(*virtual_page));
    }
    PageGroup prepared = pmm_->group();
    if (!prepared.grow(plan.pages())) {
        return std::unexpected(Stack::Error::OutOfMemory);
    }

    auto mutation = tlb_.begin();

    auto edit = std::move(mutation);
    usize mapped{};
    for (; mapped < Stack::StackPages; ++mapped) {
        const auto virtual_page = VPage::from_base(Virt{base + mapped * page_size});
        libk_assert(virtual_page);
        const auto installed = editor.map(*virtual_page, pages[mapped], arch::PtPerm::Rw, prepared);
        if (!installed) {
            break;
        }
    }
    if (mapped != Stack::StackPages) {
        while (mapped != 0) {
            --mapped;
            const auto virtual_page = VPage::from_base(Virt{base + mapped * page_size});
            libk_assert(virtual_page);
            const auto removed = editor.unmap(*virtual_page);
            libk_assert(removed);
        }
        edit.abort();
        return std::unexpected(Stack::Error::MappingFailed);
    }
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

auto Stack::create(KSpace& owner) noexcept -> CreateResult {
    auto base = owner.acquire_stack();
    if (!base) return std::unexpected(base.error());
    return Stack{Slot{&owner, *base}};
}

void Stack::Drop::operator()(Slot& s) const noexcept { s.owner->release_stack(s.base); }

} // namespace mm
