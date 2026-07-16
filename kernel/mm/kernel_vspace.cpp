#include <mm/kernel_vspace.hpp>

#include <core/debug.hpp>
#include <libk/checked_arithmetic.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

KernelVSpace::~KernelVSpace() noexcept {
    KASSERT(stack_leases_ == 0);
}

auto KernelVSpace::initialize_in(
    libk::ManualLifetime<KernelVSpace>& storage,
    Pmm& pmm) noexcept -> InitResult {
    auto built = build_in(storage, pmm);
    if (!built) {
        return built;
    }
    arch::activate_root(storage->token());
    return libk::expected();
}

auto KernelVSpace::build_in(
    libk::ManualLifetime<KernelVSpace>& storage,
    Pmm& pmm) noexcept -> InitResult {
    KASSERT(!storage);
    auto built = arch::build_kernel_root(pmm);
    if (!built) {
        return libk::unexpected(built.error());
    }
    return adopt_in(storage, pmm, libk::move(built).value());
}

auto KernelVSpace::adopt_in(
    libk::ManualLifetime<KernelVSpace>& storage,
    Pmm& pmm,
    arch::KernelRoot&& root) noexcept -> InitResult {
    KASSERT(!storage);
    KASSERT(root.token());
    [[maybe_unused]] auto& vspace = storage.emplace(
        ConstructionKey{}, pmm, libk::move(root));
    return libk::expected();
}

auto KernelVSpace::begin_edit() noexcept -> EditResult {
    auto mutation = coherence_.begin();
    if (!mutation) {
        return libk::unexpected(mutation.error());
    }
    return libk::expected(Edit{
        libk::move(mutation).value(),
        arch::PageEditor::kernel(root_)});
}

auto KernelVSpace::acquire_stack() noexcept
    -> libk::Expected<StackLease, StackError> {
    kernel::sync::IrqLockGuard guard{stack_lock_};

    if (free_stack_ != 0) {
        const usize base = free_stack_;
        free_stack_ = stack_link(base);
        ++stack_leases_;
        return libk::expected(StackLease{
            .base = base,
            .size = KernelStackLayout::StackBytes,
        });
    }

    const usize base = next_stack_base_;
    const auto slot_end = libk::checked_add(
        base - KernelStackLayout::GuardPages * page_size,
        KernelStackLayout::SlotBytes);
    if (!slot_end || *slot_end > arch::layout::kernel_base) {
        return libk::unexpected(StackError::AddressSpaceExhausted);
    }

    OwnedPageGroup backing = pmm_->make_page_group();
    Page pages[KernelStackLayout::StackPages]{};
    {
        auto extension = backing.extend();
        for (usize index = 0; index < KernelStackLayout::StackPages; ++index) {
            auto page = extension.allocate_page();
            if (!page) {
                return libk::unexpected(StackError::OutOfMemory);
            }
            pages[index] = page.value();
        }
        extension.commit();
    }

    arch::PageEditor::Plan plan{};
    for (usize index = 0; index < KernelStackLayout::StackPages; ++index) {
        const auto virtual_page = VPage::from_base(
            VirtAddr{base + index * page_size});
        KASSERT(virtual_page && plan.include(*virtual_page));
    }
    OwnedPageGroup prepared = pmm_->make_page_group();
    if (!prepared.try_extend(plan.table_pages())) {
        return libk::unexpected(StackError::OutOfMemory);
    }

    auto mutation = coherence_.begin();
    if (!mutation) {
        return libk::unexpected(StackError::MappingFailed);
    }
    auto edit = libk::move(mutation).value();
    auto editor = arch::PageEditor::kernel(root_);
    usize mapped{};
    for (; mapped < KernelStackLayout::StackPages; ++mapped) {
        const auto virtual_page = VPage::from_base(
            VirtAddr{base + mapped * page_size});
        KASSERT(virtual_page);
        const auto installed = editor.map(
            *virtual_page,
            pages[mapped],
            arch::kernel_data_permissions(),
            prepared);
        if (!installed) {
            break;
        }
    }
    if (mapped != KernelStackLayout::StackPages) {
        while (mapped != 0) {
            --mapped;
            const auto virtual_page = VPage::from_base(
                VirtAddr{base + mapped * page_size});
            KASSERT(virtual_page);
            const auto removed = editor.unmap(*virtual_page);
            KASSERT(removed);
        }
        edit.abort();
        return libk::unexpected(StackError::MappingFailed);
    }
    edit.publish_fresh();

    while (auto page = backing.take()) {
        KASSERT(stack_pages_.attach(libk::move(*page)));
    }
    next_stack_base_ = *slot_end
        + KernelStackLayout::GuardPages * page_size;
    ++stack_leases_;
    return libk::expected(StackLease{
        .base = base,
        .size = KernelStackLayout::StackBytes,
    });
}

void KernelVSpace::release_stack(usize base) noexcept {
    KASSERT(base >= arch::layout::dynamic_base + page_size);
    KASSERT((base & (page_size - 1)) == 0);
    kernel::sync::IrqLockGuard guard{stack_lock_};
    KASSERT(stack_leases_ != 0);
    stack_link(base) = free_stack_;
    free_stack_ = base;
    --stack_leases_;
}

auto KernelVSpace::stack_link(usize base) noexcept -> usize& {
    const auto virtual_page = VPage::from_base(VirtAddr{base});
    KASSERT(virtual_page);
    auto editor = arch::PageEditor::kernel(root_);
    const auto mapped = editor.query(*virtual_page);
    KASSERT(mapped);
    KASSERT(stack_pages_.contains(mapped.value().page));
    return *reinterpret_cast<usize*>(
        stack_pages_.bytes(mapped.value().page));
}

} // namespace kernel::mm
