#pragma once

#include <utility>

#include <expected>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/unique_handle.hpp>
#include <mm/table.hpp>
#include <mm/tlb.hpp>
#include <sync.hpp>

namespace mm {

class KSpace;

class Stack final {
  public:
    static constexpr usize GuardPages = 1;
    static constexpr usize StackPages = 4;
    static constexpr usize StackBytes = StackPages * page_size;
    static constexpr usize SlotBytes = (StackPages + 2 * GuardPages) * page_size;
    enum class Error : u8 { OutOfMemory, AddressSpaceExhausted, MappingFailed };
    using CreateResult = std::expected<Stack, Error>;
    Stack(Stack&&) noexcept = default;
    auto operator=(Stack&&) noexcept -> Stack& = default;
    [[nodiscard]] static auto create(KSpace&) noexcept -> CreateResult;
    [[nodiscard]] auto base() const noexcept -> usize { return slot_.get().base; }
    [[nodiscard]] auto size() const noexcept -> usize { return slot_ ? StackBytes : 0; }
    [[nodiscard]] auto top() const noexcept -> usize { return base() + size(); }
    [[nodiscard]] auto contains(usize a) const noexcept -> bool { return a >= base() && a < top(); }
    [[nodiscard]] auto lower_guard() const noexcept -> usize { return base() - page_size; }
    [[nodiscard]] auto upper_guard() const noexcept -> usize { return top(); }

  private:
    struct Slot {
        KSpace* owner{};
        usize base{};
        static auto empty() noexcept -> Slot { return {}; }
        static auto is_empty(const Slot& s) noexcept -> bool { return s.owner == nullptr; }
    };
    struct Drop {
        void operator()(Slot&) const noexcept;
    };
    explicit Stack(Slot s) noexcept : slot_(s) {}
    libk::unique_handle<Slot, Drop, Slot> slot_;
};

// Owns the shared kernel root and reusable guarded stack slots.
class KSpace final : private libk::noncopyable_nonmovable {
  public:
    ~KSpace() noexcept;
    using InitResult = std::expected<void, mm::PtErr>;

    [[nodiscard]] static auto build_in(libk::ManualLifetime<KSpace>& storage, Pmm& pmm) noexcept
        -> InitResult;
    [[nodiscard]] static auto adopt_in(libk::ManualLifetime<KSpace>& storage, Pmm& pmm,
                                       mm::PageTable&& root) noexcept -> InitResult;

    KSpace(Pmm& pmm, mm::PageTable&& root) noexcept
        : root_(std::move(root)), pmm_(&pmm), stack_pages_(pmm.group()),
          next_stack_base_(mm::DynamicBegin + Stack::GuardPages * page_size) {}

    [[nodiscard]] auto cpu_root() const noexcept -> usize { return root_.cpu_root(); }
    [[nodiscard]] auto root() noexcept -> Root { return Root{tlb_, root_.cpu_root()}; }
    [[nodiscard]] auto tlb(this auto& self) noexcept -> decltype(auto) { return (self.tlb_); }
    [[nodiscard]] auto pages() const noexcept -> const PageTable& { return root_; }
    [[nodiscard]] auto create_user_root(Pmm& pmm) const noexcept -> std::expected<mm::PageTable, mm::PtErr> {
        return mm::PageTable::create(pmm, mm::PageTable::Kind::User, &root_);
    }

  private:
    friend class Stack;

    [[nodiscard]] auto acquire_stack() noexcept -> std::expected<usize, Stack::Error>;
    void release_stack(usize base) noexcept;
    [[nodiscard]] auto stack_link(usize base) noexcept -> usize&;

    mm::PageTable root_;
    Tlb tlb_{};
    Pmm* pmm_{};
    PageGroup stack_pages_;
    sync::Spin stack_lock_{};
    usize next_stack_base_{};
    usize free_stack_{};
    usize stack_leases_{};
};

} // namespace mm
