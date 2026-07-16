#pragma once

#include <arch/address_layout.hpp>
#include <arch/page_table.hpp>
#include <arch/page_editor.hpp>
#include <libk/expected.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <mm/translation.hpp>
#include <mm/physical_alias.hpp>
#include <mm/kernel_stack_layout.hpp>
#include <libk/sync/ticket_spin_lock.hpp>

namespace kernel {
class KernelStack;
}

namespace kernel::mm {

// System address-space owner.  Semantic kernel mappings and coherence state
// grow here; the architecture root is only their hardware materialization.
class KernelVSpace final : private libk::noncopyable_nonmovable {
    class ConstructionKey final {
        friend class KernelVSpace;
        constexpr ConstructionKey() noexcept = default;
    };

public:
    ~KernelVSpace() noexcept;
    class Edit final {
    public:
        Edit(const Edit&) = delete;
        auto operator=(const Edit&) -> Edit& = delete;
        Edit(Edit&&) noexcept = default;
        auto operator=(Edit&&) -> Edit& = delete;

        [[nodiscard]] auto pages() noexcept -> arch::PageEditor& {
            return editor_;
        }
        [[nodiscard]] auto targets() const noexcept
            -> const kernel::CpuSet& {
            return mutation_.targets();
        }
        [[nodiscard]] auto commit(
            ShootdownPlan&& plan,
            ShootdownTicket& ticket,
            RetireBatch* retire = nullptr) noexcept -> ShootdownStatus {
            return mutation_.commit(
                libk::move(plan), ticket, retire);
        }

    private:
        friend class KernelVSpace;
        Edit(
            TranslationState::Mutation&& mutation,
            arch::PageEditor&& editor) noexcept
            : mutation_(libk::move(mutation)),
              editor_(libk::move(editor)) {}

        TranslationState::Mutation mutation_;
        arch::PageEditor editor_;
    };

    using InitResult = libk::Expected<void, arch::RootError>;
    using EditResult = libk::Expected<Edit, ShootdownError>;

    [[nodiscard]] static auto initialize_in(
        libk::ManualLifetime<KernelVSpace>& storage,
        Pmm& pmm) noexcept -> InitResult;
    [[nodiscard]] static auto build_in(
        libk::ManualLifetime<KernelVSpace>& storage,
        Pmm& pmm) noexcept -> InitResult;
    [[nodiscard]] static auto adopt_in(
        libk::ManualLifetime<KernelVSpace>& storage,
        Pmm& pmm,
        arch::KernelRoot&& root) noexcept -> InitResult;

    KernelVSpace(
        [[maybe_unused]] ConstructionKey key,
        Pmm& pmm,
        arch::KernelRoot&& root) noexcept
        : root_(libk::move(root)),
          aliases_(pmm),
          pmm_(&pmm),
          stack_pages_(pmm.make_page_group()),
          next_stack_base_(
              arch::layout::dynamic_base
              + KernelStackLayout::GuardPages * page_size) {}

    [[nodiscard]] auto token() const noexcept -> arch::RootToken {
        return root_.token();
    }
    [[nodiscard]] auto translation() noexcept -> TranslationView {
        return TranslationView{coherence_, root_.token()};
    }
    [[nodiscard]] auto coherence(this auto& self) noexcept
        -> decltype(auto) {
        return (self.coherence_);
    }
    [[nodiscard]] auto begin_edit() noexcept -> EditResult;
    [[nodiscard]] auto create_user_root(Pmm& pmm) const noexcept
        -> libk::Expected<arch::UserRoot, arch::RootError> {
        return arch::UserRoot::create(root_, pmm);
    }
    [[nodiscard]] auto aliases() noexcept -> PhysicalAliasRegistry& {
        return aliases_;
    }

private:
    friend class kernel::KernelStack;

    enum class StackError : u8 {
        OutOfMemory,
        AddressSpaceExhausted,
        MappingFailed,
    };

    struct StackLease final {
        usize base{};
        usize size{};
    };

    [[nodiscard]] auto acquire_stack() noexcept
        -> libk::Expected<StackLease, StackError>;
    void release_stack(usize base) noexcept;
    [[nodiscard]] auto stack_link(usize base) noexcept -> usize&;

    arch::KernelRoot root_;
    TranslationState coherence_{};
    PhysicalAliasRegistry aliases_;
    Pmm* pmm_{};
    OwnedPageGroup stack_pages_;
    libk::TicketSpinLock stack_lock_{};
    usize next_stack_base_{};
    usize free_stack_{};
    usize stack_leases_{};
};

} // namespace kernel::mm
