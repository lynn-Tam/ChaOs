#include <mm/memory_object.hpp>

#include <object/pager_pool.hpp>
#include <pager/pager.hpp>

#include <core/debug.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/limits.hpp>
#include <libk/mem.h>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

namespace {

[[nodiscard]] auto validate_extents(
    Pmm& pmm,
    usize logical_pages,
    libk::Span<const MemoryExtent> extents,
    BackingKind kind,
    BootOwnership ownership,
    const OwnedPageGroup& owned) noexcept
    -> libk::Expected<void, MemoryError> {
    if (extents.empty()) {
        return libk::unexpected(MemoryError::InvalidRange);
    }

    usize previous_end{};
    usize owned_pages{};
    for (usize index = 0; index < extents.size(); ++index) {
        const MemoryExtent& extent = extents[index];
        const auto object_end = extent.object.end();
        if (!object_end
            || *object_end > logical_pages
            || extent.object.first < previous_end
            || !extent.physical.valid()
            || extent.physical.page_count() != extent.object.page_count) {
            return libk::unexpected(MemoryError::InvalidRange);
        }
        if (!valid_access(extent.access)) {
            return libk::unexpected(MemoryError::InvalidAccess);
        }
        if (extent.type == MemoryType::Device
            && extent.access.contains(Access::Execute)) {
            return libk::unexpected(MemoryError::InvalidMemoryType);
        }
        previous_end = *object_end;

        for (usize prior = 0; prior < index; ++prior) {
            if (!extent.physical.intersects(extents[prior].physical)) {
                continue;
            }
            if (extent.type != extents[prior].type) {
                return libk::unexpected(MemoryError::InvalidMemoryType);
            }
            return libk::unexpected(MemoryError::InvalidRange);
        }

        for (const Page page : extent.physical) {
            const auto state = pmm.state_of(page);
            if (kind == BackingKind::BootImage
                && ownership == BootOwnership::Owned) {
                if (!owned || !owned.contains(page)
                    || extent.type != MemoryType::Normal) {
                    return libk::unexpected(
                        MemoryError::OwnershipMismatch);
                }
                ++owned_pages;
                continue;
            }
            if (kind == BackingKind::BootImage) {
                if (!state || state.value() != PageState::Reserved
                    || extent.type != MemoryType::Normal) {
                    return libk::unexpected(
                        MemoryError::OwnershipMismatch);
                }
                continue;
            }
            // Borrowed RAM must remain outside the allocator's reusable
            // states. Device physical ranges are intentionally unmanaged.
            if (state) {
                if (state.value() != PageState::Reserved) {
                    return libk::unexpected(
                        MemoryError::OwnershipMismatch);
                }
                if (extent.type != MemoryType::Normal) {
                    return libk::unexpected(
                        MemoryError::InvalidMemoryType);
                }
            } else if (extent.type == MemoryType::Normal) {
                // Normal memory must be backed by RAM known to the PMM/direct
                // map inventory. Unmanaged physical ranges are MMIO-like and
                // require an explicit non-normal memory type.
                return libk::unexpected(MemoryError::InvalidMemoryType);
            }
        }
    }

    if (kind == BackingKind::BootImage) {
        if (ownership == BootOwnership::Owned
            && (!owned || owned.page_count() != owned_pages)) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        if (ownership == BootOwnership::Borrowed && owned) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
    } else if (owned) {
        return libk::unexpected(MemoryError::OwnershipMismatch);
    }
    return libk::expected();
}

class ExtentStore final : private libk::noncopyable_nonmovable {
    struct Block final {
        Block(
            OwnedPage&& page,
            kernel::resource::Reservation&& charge) noexcept
            : backing(libk::move(page)) {
            if (charge) {
                sponsorship.commit(libk::move(charge));
            }
        }

        OwnedPage backing{};
        kernel::resource::Sponsorship sponsorship{};
        Block* next{};
        usize count{};

        [[nodiscard]] auto extents() noexcept -> MemoryExtent* {
            return reinterpret_cast<MemoryExtent*>(
                reinterpret_cast<usize>(this) + extent_offset);
        }
        [[nodiscard]] auto extents() const noexcept -> const MemoryExtent* {
            return reinterpret_cast<const MemoryExtent*>(
                reinterpret_cast<usize>(this) + extent_offset);
        }
    };

    static constexpr usize extent_offset =
        (sizeof(Block) + alignof(MemoryExtent) - 1)
        & ~(alignof(MemoryExtent) - 1);
    static constexpr usize extent_capacity =
        (page_size - extent_offset) / sizeof(MemoryExtent);
    static_assert(extent_capacity != 0);

public:
    ExtentStore(
        Pmm& pmm,
        kernel::resource::Sponsorship* sponsor) noexcept
        : pmm_(&pmm), sponsor_(sponsor) {}

    ~ExtentStore() noexcept { reset(); }

    [[nodiscard]] auto initialize(
        libk::Span<const MemoryExtent> extents) noexcept
        -> libk::Expected<void, MemoryError> {
        KASSERT(head_ == nullptr);
        Block* tail{};
        usize copied{};
        while (copied < extents.size()) {
            auto charge = reserve_page();
            if (!charge) {
                reset();
                return libk::unexpected(charge.error());
            }
            auto allocated = pmm_->allocate_page();
            if (!allocated) {
                reset();
                return libk::unexpected(MemoryError::OutOfMemory);
            }
            OwnedPage page = libk::move(allocated).value();
            auto* const block = libk::construct_at(
                reinterpret_cast<Block*>(page.bytes()),
                libk::move(page),
                libk::move(charge).value());
            const usize remaining = extents.size() - copied;
            block->count = remaining < extent_capacity
                ? remaining
                : extent_capacity;
            for (usize index = 0; index < block->count; ++index) {
                libk::construct_at(
                    &block->extents()[index], extents[copied + index]);
            }
            copied += block->count;
            if (tail == nullptr) {
                head_ = block;
            } else {
                tail->next = block;
            }
            tail = block;
        }
        return libk::expected();
    }

    [[nodiscard]] auto find(usize page_index) const noexcept
        -> const MemoryExtent* {
        for (const Block* block = head_;
             block != nullptr;
             block = block->next) {
            for (usize index = 0; index < block->count; ++index) {
                const MemoryExtent& extent = block->extents()[index];
                const auto end = extent.object.end();
                KASSERT(end);
                if (page_index >= extent.object.first
                    && page_index < *end) {
                    return &extent;
                }
            }
        }
        return nullptr;
    }

    void reset() noexcept {
        while (head_ != nullptr) {
            Block* const block = head_;
            head_ = block->next;
            for (usize index = 0; index < block->count; ++index) {
                libk::destroy_at(&block->extents()[index]);
            }
            auto refund = block->sponsorship.detach();
            OwnedPage backing = libk::move(block->backing);
            libk::destroy_at(block);
            backing.reset();
            refund.complete();
        }
    }

private:
    [[nodiscard]] auto reserve_page() const noexcept
        -> libk::Expected<kernel::resource::Reservation, MemoryError> {
        if (sponsor_ == nullptr) {
            return libk::expected(kernel::resource::Reservation{});
        }
        auto reserved = sponsor_->reserve(kernel::resource::Budget{
            .memory = page_size,
        });
        if (!reserved) {
            return libk::unexpected(
                reserved.error() == kernel::resource::PoolError::Exhausted
                    ? MemoryError::ResourceExhausted
                    : MemoryError::InvalidState);
        }
        return libk::expected(libk::move(reserved).value());
    }

    Pmm* pmm_{};
    kernel::resource::Sponsorship* sponsor_{};
    Block* head_{};
};

class AnonymousBacking final : private libk::noncopyable_nonmovable {
    struct Slot;

    struct Node final {
        explicit Node(usize page_index) noexcept : index(page_index) {}

        usize index{};
        OwnedPage resident{};
        ContentState state{ContentState::Busy};
        libk::IntrusiveTreeHook tree_hook{};
        Slot* slot{};
        kernel::resource::Sponsorship resident_sponsorship{};
    };

    struct Compare final {
        [[nodiscard]] constexpr auto operator()(
            const Node& lhs,
            const Node& rhs) const noexcept -> bool {
            return lhs.index < rhs.index;
        }
        [[nodiscard]] constexpr auto operator()(
            usize lhs,
            const Node& rhs) const noexcept -> bool {
            return lhs < rhs.index;
        }
        [[nodiscard]] constexpr auto operator()(
            const Node& lhs,
            usize rhs) const noexcept -> bool {
            return lhs.index < rhs;
        }
    };

    using Tree = libk::IntrusiveTree<Node, &Node::tree_hook, Compare>;

    struct PageHeader;
    struct Slot final {
        PageHeader* page{};
        Slot* next_free{};
        bool occupied{};
        alignas(Node) byte storage[sizeof(Node)]{};

        [[nodiscard]] auto node() noexcept -> Node* {
            return reinterpret_cast<Node*>(storage);
        }
    };

    struct PageHeader final {
        PageHeader(
            OwnedPage&& page,
            kernel::resource::Reservation&& charge) noexcept
            : backing(libk::move(page)) {
            if (charge) {
                sponsorship.commit(libk::move(charge));
            }
        }

        OwnedPage backing{};
        kernel::resource::Sponsorship sponsorship{};
        PageHeader* next{};
        Slot* free_head{};
        usize live_count{};
    };

    static constexpr usize slot_offset =
        (sizeof(PageHeader) + alignof(Slot) - 1) & ~(alignof(Slot) - 1);
    static constexpr usize slots_per_page =
        (page_size - slot_offset) / sizeof(Slot);

public:
    AnonymousBacking(
        Pmm& pmm,
        AccessMask access,
        kernel::resource::Sponsorship* sponsor) noexcept
        : pmm_(&pmm), access_(access), sponsor_(sponsor) {
        static_assert(slots_per_page != 0);
    }

    ~AnonymousBacking() noexcept { reset(); }

    [[nodiscard]] auto query(usize page_index) const noexcept -> ContentState {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        const Node* const node = tree_.find(page_index);
        return node != nullptr ? node->state : ContentState::Zero;
    }

    [[nodiscard]] auto materialize(usize page_index) noexcept
        -> libk::Expected<MemoryPage, MemoryError> {
        for (;;) {
            {
                kernel::sync::IrqLockGuard guard{tree_lock_};
                const Node* const existing = tree_.find(page_index);
                if (existing != nullptr) {
                    return page_of(*existing);
                }
            }

            auto claimed = claim(page_index);
            if (!claimed) {
                return libk::unexpected(claimed.error());
            }
            Node* const candidate = claimed.value();
            bool inserted{};
            {
                kernel::sync::IrqLockGuard guard{tree_lock_};
                if (tree_.find(page_index) == nullptr) {
                    tree_.insert(*candidate);
                    inserted = true;
                }
            }
            if (!inserted) {
                release(*candidate);
                continue;
            }

            auto charge = reserve_page();
            if (!charge) {
                rollback(*candidate);
                return libk::unexpected(charge.error());
            }
            auto allocated = pmm_->allocate_page();
            if (!allocated) {
                rollback(*candidate);
                return libk::unexpected(MemoryError::OutOfMemory);
            }
            OwnedPage resident = libk::move(allocated).value();
            memset(resident.bytes(), 0, page_size);
            const Page page = resident.page();
            {
                kernel::sync::IrqLockGuard guard{tree_lock_};
                KASSERT(candidate->state == ContentState::Busy);
                candidate->resident = libk::move(resident);
                if (charge.value()) {
                    candidate->resident_sponsorship.commit(
                        libk::move(charge).value());
                }
                candidate->state = ContentState::Resident;
            }
            return libk::expected(MemoryPage{
                .page = page,
                .access = access_,
                .type = MemoryType::Normal,
            });
        }
    }

    [[nodiscard]] auto begin_transfer(usize page_index) noexcept
        -> libk::Expected<OwnedPage, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = tree_.find(page_index);
        if (node == nullptr || node->state != ContentState::Resident
            || !node->resident) {
            return libk::unexpected(
                node == nullptr ? MemoryError::NotBacked
                                : MemoryError::OwnershipMismatch);
        }
        node->state = ContentState::Busy;
        return libk::expected(libk::move(node->resident));
    }

    [[nodiscard]] auto restore_transfer(
        usize page_index,
        OwnedPage&& page) noexcept
        -> libk::Expected<void, MemoryError> {
        if (!page) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = tree_.find(page_index);
        if (node == nullptr || node->state != ContentState::Busy
            || node->resident) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        node->resident = libk::move(page);
        node->state = ContentState::Resident;
        return libk::expected();
    }

    [[nodiscard]] auto commit_transfer(usize page_index) noexcept
        -> libk::Expected<void, MemoryError> {
        Node* node{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            node = tree_.find(page_index);
            if (node == nullptr || node->state != ContentState::Busy
                || node->resident) {
                return libk::unexpected(MemoryError::OwnershipMismatch);
            }
            tree_.erase(*node);
        }
        auto refund = node->resident_sponsorship.detach();
        release(*node);
        refund.complete();
        return libk::expected();
    }

    void reset() noexcept {
        for (;;) {
            Node* node{};
            {
                kernel::sync::IrqLockGuard guard{tree_lock_};
                node = tree_.minimum();
                if (node != nullptr) {
                    tree_.erase(*node);
                }
            }
            if (node == nullptr) {
                break;
            }
            auto resident_refund = node->resident_sponsorship.detach();
            node->resident.reset();
            resident_refund.complete();
            release(*node);
        }

        KASSERT(!growing_);
        while (pages_ != nullptr) {
            PageHeader* const page = pages_;
            pages_ = page->next;
            KASSERT(page->live_count == 0);
            auto* const slots = reinterpret_cast<Slot*>(
                reinterpret_cast<usize>(page) + slot_offset);
            for (usize index = 0; index < slots_per_page; ++index) {
                KASSERT(!slots[index].occupied);
                libk::destroy_at(&slots[index]);
            }
            auto refund = page->sponsorship.detach();
            OwnedPage backing = libk::move(page->backing);
            libk::destroy_at(page);
            backing.reset();
            refund.complete();
        }
    }

private:
    [[nodiscard]] auto page_of(const Node& node) const noexcept
        -> libk::Expected<MemoryPage, MemoryError> {
        switch (node.state) {
        case ContentState::Resident:
            return libk::expected(MemoryPage{
                .page = node.resident.page(),
                .access = access_,
                .type = MemoryType::Normal,
            });
        case ContentState::Busy:
            return libk::unexpected(MemoryError::Busy);
        case ContentState::Failed:
            return libk::unexpected(MemoryError::BackingFailed);
        case ContentState::Zero:
            break;
        }
        return libk::unexpected(MemoryError::NotBacked);
    }

    [[nodiscard]] auto claim(usize page_index) noexcept
        -> libk::Expected<Node*, MemoryError> {
        for (;;) {
            {
                kernel::sync::IrqLockGuard guard{storage_lock_};
                for (PageHeader* page = pages_;
                     page != nullptr;
                     page = page->next) {
                    if (page->free_head == nullptr) {
                        continue;
                    }
                    Slot* const slot = page->free_head;
                    page->free_head = slot->next_free;
                    slot->next_free = nullptr;
                    slot->occupied = true;
                    ++page->live_count;
                    Node* const node = libk::construct_at(
                        slot->node(), page_index);
                    node->slot = slot;
                    return libk::expected(node);
                }
                if (growing_) {
                    return libk::unexpected(MemoryError::Busy);
                }
                growing_ = true;
            }

            auto charge = reserve_page();
            if (!charge) {
                kernel::sync::IrqLockGuard guard{storage_lock_};
                KASSERT(growing_);
                growing_ = false;
                return libk::unexpected(charge.error());
            }
            auto allocated = pmm_->allocate_page();
            if (!allocated) {
                kernel::sync::IrqLockGuard guard{storage_lock_};
                KASSERT(growing_);
                growing_ = false;
                return libk::unexpected(MemoryError::OutOfMemory);
            }
            OwnedPage backing = libk::move(allocated).value();
            auto* const page = libk::construct_at(
                reinterpret_cast<PageHeader*>(backing.bytes()),
                libk::move(backing),
                libk::move(charge).value());
            auto* const slots = reinterpret_cast<Slot*>(
                reinterpret_cast<usize>(page) + slot_offset);
            for (usize index = slots_per_page; index > 0; --index) {
                Slot* const slot = libk::construct_at(&slots[index - 1]);
                slot->page = page;
                slot->next_free = page->free_head;
                page->free_head = slot;
            }
            {
                kernel::sync::IrqLockGuard guard{storage_lock_};
                page->next = pages_;
                pages_ = page;
                KASSERT(growing_);
                growing_ = false;
            }
        }
    }

    void release(Node& node) noexcept {
        Slot* const slot = node.slot;
        KASSERT(slot != nullptr);
        libk::destroy_at(&node);
        kernel::sync::IrqLockGuard guard{storage_lock_};
        KASSERT(slot->occupied);
        slot->occupied = false;
        slot->next_free = slot->page->free_head;
        slot->page->free_head = slot;
        KASSERT(slot->page->live_count != 0);
        --slot->page->live_count;
    }

    void rollback(Node& node) noexcept {
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            KASSERT(tree_.find(node.index) == &node);
            tree_.erase(node);
        }
        release(node);
    }

    [[nodiscard]] auto reserve_page() const noexcept
        -> libk::Expected<kernel::resource::Reservation, MemoryError> {
        if (sponsor_ == nullptr) {
            return libk::expected(kernel::resource::Reservation{});
        }
        auto reserved = sponsor_->reserve(kernel::resource::Budget{
            .memory = page_size,
        });
        if (!reserved) {
            return libk::unexpected(
                reserved.error() == kernel::resource::PoolError::Exhausted
                    ? MemoryError::ResourceExhausted
                    : MemoryError::InvalidState);
        }
        return libk::expected(libk::move(reserved).value());
    }

    Pmm* pmm_{};
    AccessMask access_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::BackingTree>
        tree_lock_{};
    kernel::sync::SpinLock<kernel::sync::LockClass::BackingStorage>
        storage_lock_{};
    Tree tree_{};
    PageHeader* pages_{};
    bool growing_{};
    kernel::resource::Sponsorship* sponsor_{};
};

class PagerBacking final : private libk::noncopyable_nonmovable {
    struct Slot;
    struct PageHeader;

    struct Node final {
        explicit Node(usize page_index) noexcept : index(page_index) {}

        usize index{};
        PageSlot slot{};
        OwnedPage resident{};
        Slot* owner{};
        Node* next{};
        using MappingList = libk::IntrusiveList<
            PageMapping, &PageMapping::backing_hook_>;
        MappingList mappings{};
        usize leases{};
        u64 accessed_epoch{};
        u64 usage_epoch{};
        kernel::resource::Sponsorship resident_sponsorship{};
    };

    struct Slot final {
        PageHeader* page{};
        Slot* next_free{};
        bool occupied{};
        alignas(Node) byte storage[sizeof(Node)];

        [[nodiscard]] auto node() noexcept -> Node* {
            return reinterpret_cast<Node*>(storage);
        }
    };

    struct PageHeader final {
        PageHeader(
            OwnedPage&& page,
            kernel::resource::Reservation&& charge) noexcept
            : backing(libk::move(page)) {
            if (charge) {
                sponsorship.commit(libk::move(charge));
            }
        }

        OwnedPage backing{};
        kernel::resource::Sponsorship sponsorship{};
        PageHeader* next{};
        Slot* free_head{};
        usize live_count{};
    };

    static constexpr usize slot_offset =
        (sizeof(PageHeader) + alignof(Slot) - 1) & ~(alignof(Slot) - 1);
    static constexpr usize slots_per_page =
        (page_size - slot_offset) / sizeof(Slot);

public:
    PagerBacking(
        Pmm& pmm,
        kernel::pager::Pager& pager,
        AccessMask access,
        kernel::resource::Sponsorship* sponsor) noexcept
        : pmm_(&pmm), pager_(&pager), access_(access), sponsor_(sponsor) {
        static_assert(slots_per_page != 0);
    }

    ~PagerBacking() noexcept { reset(); }

    static void request_finished(
        void* context,
        PageKey key,
        bool failed) noexcept {
        auto& self = *static_cast<PagerBacking*>(context);
        kernel::sync::IrqLockGuard guard{self.tree_lock_};
        Node* const node = self.find_locked(key.index);
        if (node == nullptr || node->slot.generation != key.generation) {
            return;
        }
        // A Pager completion is only a transport edge.  A target page must
        // already be Resident before a successful completion can be
        // accepted; otherwise turn the protocol violation into the same
        // generation-checked failure as an explicit pager_fail.
        if (node->slot.state != PageSlotState::Requested
            && node->slot.state != PageSlotState::Filling) {
            return;
        }
        const u64 claim = node->slot.request.claim_generation;
        static_cast<void>(node->slot.fail(claim));
        (void)failed;
    }

    [[nodiscard]] auto query(usize page_index) const noexcept -> ContentState {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        const Node* node = nodes_;
        while (node != nullptr && node->index != page_index) {
            node = node->next;
        }
        if (node == nullptr) {
            return ContentState::Zero;
        }
        switch (node->slot.state) {
        case PageSlotState::ResidentClean:
        case PageSlotState::ResidentDirty:
            return ContentState::Resident;
        case PageSlotState::Failed:
            return ContentState::Failed;
        case PageSlotState::Missing:
            return ContentState::Zero;
        default:
            return ContentState::Busy;
        }
    }

    [[nodiscard]] auto materialize(usize page_index) noexcept
        -> libk::Expected<MemoryPage, MemoryError> {
        Node* node{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            node = find_locked(page_index);
            if (node != nullptr) {
                if (node->slot.state == PageSlotState::ResidentClean
                    || node->slot.state == PageSlotState::ResidentDirty) {
                    return libk::expected(MemoryPage{
                        .page = node->resident.page(),
                        .access = access_,
                        .type = MemoryType::Normal,
                    });
                }
                if (node->slot.state == PageSlotState::Failed) {
                    return libk::unexpected(MemoryError::BackingFailed);
                }
                return libk::unexpected(MemoryError::Pending);
            }
            node = claim();
            if (node == nullptr) {
                return libk::unexpected(MemoryError::OutOfMemory);
            }
            node->index = page_index;
            node->next = nodes_;
            nodes_ = node;
            const u64 generation = node->slot.generation ==
                    libk::numeric_limits<u64>::max()
                ? 0
                : node->slot.generation + 1;
            if (generation == 0
                || !node->slot.begin_request(
                    PageKey{generation, page_index}, page_index, 1)) {
                unlink_locked(*node);
                release(*node);
                return libk::unexpected(MemoryError::GenerationExhausted);
            }
        }

        const auto published = pager_->publish(
            node->slot.request.key,
            page_index,
            1,
            node->slot.request.key.generation,
            0,
            kernel::pager::RequestLink{
                .context = this,
                .finish = &PagerBacking::request_finished,
            });
        if (!published) {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            node->slot.cancel_request();
            unlink_locked(*node);
            release(*node);
            return libk::unexpected(published.error() == pager::Error::Full
                ? MemoryError::ResourceExhausted
                : published.error() == pager::Error::Closed
                    ? MemoryError::BackingFailed
                    : MemoryError::InvalidState);
        }
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            node->slot.request.transport_slot = published.value().key.slot;
        }
        return libk::unexpected(MemoryError::Pending);
    }

    [[nodiscard]] auto supply(
        usize page_index,
        u64 request_generation,
        u64 claim_generation,
        OwnedPage&& page,
        u64 content_epoch) noexcept
        -> libk::Expected<void, MemoryError> {
        if (!page || request_generation == 0 || claim_generation == 0
            || content_epoch == 0) {
            return libk::unexpected(MemoryError::InvalidRange);
        }
        // The service must have claimed the exact Pager record before it can
        // donate ownership.  This check is an authority edge, not page state.
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr
                || node->slot.request.key.generation != request_generation) {
                return libk::unexpected(MemoryError::OwnershipMismatch);
            }
        }
        if (!pager_->claimed(
                PageKey{request_generation, page_index}, claim_generation)) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        auto charge = reserve_page();
        if (!charge) {
            return libk::unexpected(charge.error());
        }
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr
                || node->slot.request.key.generation != request_generation) {
                return libk::unexpected(MemoryError::GenerationExhausted);
            }
            if (!node->slot.begin_fill(claim_generation)) {
                return libk::unexpected(MemoryError::Busy);
            }
            auto supplied = node->slot.supply(
                claim_generation, content_epoch);
            if (!supplied) {
                return libk::unexpected(MemoryError::InvalidState);
            }
            node->resident = libk::move(page);
            if (charge.value()) {
                node->resident_sponsorship.commit(libk::move(charge).value());
            }
        }
        // Supply and transport completion remain separate linearization
        // points. The service may still publish a completion/failure result
        // after this ownership commit, while a late reply is checked against
        // the same PageKey and cannot revive a newer request.
        return libk::expected();
    }

    [[nodiscard]] auto begin_transfer(usize) noexcept
        -> libk::Expected<OwnedPage, MemoryError> {
        return libk::unexpected(MemoryError::OwnershipMismatch);
    }

    [[nodiscard]] auto restore_transfer(usize, OwnedPage&&) noexcept
        -> libk::Expected<void, MemoryError> {
        return libk::unexpected(MemoryError::OwnershipMismatch);
    }

    [[nodiscard]] auto commit_transfer(usize) noexcept
        -> libk::Expected<void, MemoryError> {
        return libk::unexpected(MemoryError::OwnershipMismatch);
    }

    [[nodiscard]] auto fail(
        usize page_index,
        u64 request_generation,
        u64 claim_generation) noexcept
        -> libk::Expected<void, MemoryError> {
        if (request_generation == 0 || claim_generation == 0) {
            return libk::unexpected(MemoryError::InvalidRange);
        }
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr
                || node->slot.request.key.generation != request_generation
                || (node->slot.state != PageSlotState::Requested
                    && node->slot.state != PageSlotState::Filling)) {
                return libk::unexpected(MemoryError::OwnershipMismatch);
            }
        }

        // Finish the exact claimed transport record.  Pager owns the
        // completion linearization and invokes request_finished(), which
        // transitions this PageSlot under the backing lock.  Looking up by
        // PageKey avoids trusting the cached transport slot across reuse.
        const auto finished = pager_->fail(
            PageKey{request_generation, page_index}, claim_generation);
        if (!finished) {
            switch (finished.error()) {
            case kernel::pager::Error::Closed:
                return libk::unexpected(MemoryError::BackingFailed);
            case kernel::pager::Error::InvalidRange:
                return libk::unexpected(MemoryError::InvalidRange);
            case kernel::pager::Error::GenerationExhausted:
                return libk::unexpected(MemoryError::GenerationExhausted);
            case kernel::pager::Error::Full:
            case kernel::pager::Error::InvalidKey:
            case kernel::pager::Error::Busy:
            case kernel::pager::Error::Stale:
                return libk::unexpected(MemoryError::OwnershipMismatch);
            }
        }

        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(page_index);
        if (node == nullptr || node->slot.generation != request_generation) {
            return libk::unexpected(MemoryError::GenerationExhausted);
        }
        if (node->slot.state != PageSlotState::Failed) {
            // A successful transport failure must have a corresponding page
            // failure.  A concurrent supply/retry therefore reports a
            // checked ownership race instead of silently claiming success.
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        return libk::expected();
    }

    [[nodiscard]] auto bind_mapping(
        usize page_index,
        PageMapping& mapping) noexcept
        -> libk::Expected<void, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(page_index);
        if (node == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        if (node->slot.state != PageSlotState::ResidentClean
            && node->slot.state != PageSlotState::ResidentDirty) {
            return libk::unexpected(
                node->slot.state == PageSlotState::Failed
                    ? MemoryError::BackingFailed
                    : MemoryError::Pending);
        }
        if (mapping.attached() || mapping.backing_hook_.is_linked()) {
            return libk::unexpected(MemoryError::AttachmentState);
        }
        node->mappings.push_back(mapping);
        return libk::expected();
    }

    [[nodiscard]] auto unbind_mapping(
        PageMapping& mapping) noexcept
        -> libk::Expected<void, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(mapping.page_index());
        if (node == nullptr || !mapping.backing_hook_.is_linked()) {
            return libk::unexpected(MemoryError::AttachmentState);
        }
        node->mappings.erase(mapping);
        return libk::expected();
    }

    [[nodiscard]] auto lease_acquire(usize page_index) noexcept
        -> libk::Expected<void, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(page_index);
        if (node == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        if (node->slot.state != PageSlotState::ResidentClean
            && node->slot.state != PageSlotState::ResidentDirty) {
            return libk::unexpected(
                node->slot.state == PageSlotState::Failed
                    ? MemoryError::BackingFailed
                    : MemoryError::Pending);
        }
        KASSERT(node->leases != libk::numeric_limits<usize>::max());
        ++node->leases;
        return libk::expected();
    }

    void lease_release(Page page) noexcept {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* node{};
        for (Node* candidate = nodes_; candidate != nullptr;
             candidate = candidate->next) {
            if (candidate->resident.page() == page) {
                node = candidate;
                break;
            }
        }
        KASSERT(node != nullptr && node->leases != 0);
        --node->leases;
    }

    [[nodiscard]] auto page_state(usize page_index) const noexcept
        -> libk::Expected<PageSlotState, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        const Node* node = nodes_;
        while (node != nullptr && node->index != page_index) {
            node = node->next;
        }
        if (node == nullptr) {
            return libk::expected(PageSlotState::Missing);
        }
        return libk::expected(node->slot.state);
    }

    [[nodiscard]] auto observe_usage(
        usize page_index,
        bool accessed,
        bool dirty) noexcept -> libk::Expected<void, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(page_index);
        if (node == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        if (node->slot.state != PageSlotState::ResidentClean
            && node->slot.state != PageSlotState::ResidentDirty
            && node->slot.state != PageSlotState::WritebackQueued
            && node->slot.state != PageSlotState::WritebackActive) {
            return libk::unexpected(
                node->slot.state == PageSlotState::Failed
                    ? MemoryError::BackingFailed
                    : MemoryError::Pending);
        }
        if (accessed) {
            if (node->accessed_epoch
                == libk::numeric_limits<u64>::max()) {
                return libk::unexpected(MemoryError::GenerationExhausted);
            }
            ++node->accessed_epoch;
        }
        if (dirty) {
            if (node->usage_epoch == libk::numeric_limits<u64>::max()) {
                return libk::unexpected(MemoryError::GenerationExhausted);
            }
            const u64 epoch = ++node->usage_epoch;
            auto marked = node->slot.mark_dirty(epoch);
            if (!marked) {
                return libk::unexpected(
                    marked.error() == PageStateError::StaleGeneration
                        ? MemoryError::GenerationExhausted
                        : MemoryError::InvalidState);
            }
        }
        return libk::expected();
    }

    [[nodiscard]] auto queue_writeback(usize page_index) noexcept
        -> libk::Expected<u64, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(page_index);
        if (node == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        auto queued = node->slot.queue_writeback();
        if (!queued) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        return libk::expected(queued.value());
    }

    [[nodiscard]] auto begin_writeback(
        usize page_index,
        u64 epoch) noexcept -> libk::Expected<void, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(page_index);
        if (node == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        auto begun = node->slot.begin_writeback(epoch);
        return begun
            ? libk::Expected<void, MemoryError>{libk::expected()}
            : libk::Expected<void, MemoryError>{
                  libk::unexpected(MemoryError::OwnershipMismatch)};
    }

    [[nodiscard]] auto complete_writeback(
        usize page_index,
        u64 epoch,
        bool clean) noexcept -> libk::Expected<void, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(page_index);
        if (node == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        auto completed = node->slot.complete_writeback(epoch, clean);
        return completed
            ? libk::Expected<void, MemoryError>{libk::expected()}
            : libk::Expected<void, MemoryError>{
                  libk::unexpected(MemoryError::OwnershipMismatch)};
    }

    [[nodiscard]] auto evict_page(usize page_index) noexcept
        -> libk::Expected<void, MemoryError> {
        OwnedPage page{};
        kernel::resource::Refund refund{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr) {
                return libk::unexpected(MemoryError::NotBacked);
            }
            if (node->slot.state != PageSlotState::ResidentClean) {
                return libk::unexpected(MemoryError::InvalidState);
            }
            if (!node->mappings.empty() || node->leases != 0) {
                return libk::unexpected(MemoryError::Busy);
            }
            auto begun = node->slot.begin_evict();
            if (!begun) {
                return libk::unexpected(MemoryError::InvalidState);
            }
            page = libk::move(node->resident);
            refund = node->resident_sponsorship.detach();
            auto finished = node->slot.finish_evict();
            KASSERT(finished);
        }
        page.reset();
        refund.complete();
        return libk::expected();
    }

    void reset() noexcept {
        pager_->detach_links(this);
        while (nodes_ != nullptr) {
            Node* const node = nodes_;
            nodes_ = node->next;
            KASSERT(node->mappings.empty() && node->leases == 0);
            node->resident.reset();
            release(*node);
        }
        KASSERT(!growing_);
        while (pages_ != nullptr) {
            PageHeader* const page = pages_;
            pages_ = page->next;
            KASSERT(page->live_count == 0);
            auto* const slots = reinterpret_cast<Slot*>(
                reinterpret_cast<usize>(page) + slot_offset);
            for (usize index = 0; index < slots_per_page; ++index) {
                libk::destroy_at(&slots[index]);
            }
            auto refund = page->sponsorship.detach();
            OwnedPage backing = libk::move(page->backing);
            libk::destroy_at(page);
            backing.reset();
            refund.complete();
        }
    }

private:
    [[nodiscard]] auto find_locked(usize index) noexcept -> Node* {
        for (Node* node = nodes_; node != nullptr; node = node->next) {
            if (node->index == index) {
                return node;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto claim() noexcept -> Node* {
        for (;;) {
            {
                kernel::sync::IrqLockGuard guard{storage_lock_};
                if (pages_ != nullptr && pages_->free_head != nullptr) {
                    Slot* const slot = pages_->free_head;
                    pages_->free_head = slot->next_free;
                    slot->next_free = nullptr;
                    slot->occupied = true;
                    ++pages_->live_count;
                    Node* const node = libk::construct_at(slot->node(), 0);
                    node->owner = slot;
                    return node;
                }
                if (growing_) {
                    return nullptr;
                }
                growing_ = true;
            }
            auto charge = reserve_page();
            if (!charge) {
                kernel::sync::IrqLockGuard guard{storage_lock_};
                growing_ = false;
                return nullptr;
            }
            auto allocated = pmm_->allocate_page();
            if (!allocated) {
                kernel::sync::IrqLockGuard guard{storage_lock_};
                growing_ = false;
                return nullptr;
            }
            OwnedPage backing = libk::move(allocated).value();
            auto* const page = libk::construct_at(
                reinterpret_cast<PageHeader*>(backing.bytes()),
                libk::move(backing), libk::move(charge).value());
            auto* const slots = reinterpret_cast<Slot*>(
                reinterpret_cast<usize>(page) + slot_offset);
            for (usize index = slots_per_page; index > 0; --index) {
                Slot* const slot = libk::construct_at(&slots[index - 1]);
                slot->page = page;
                slot->next_free = page->free_head;
                page->free_head = slot;
            }
            kernel::sync::IrqLockGuard guard{storage_lock_};
            page->next = pages_;
            pages_ = page;
            growing_ = false;
        }
    }

    void unlink_locked(Node& target) noexcept {
        Node** link = &nodes_;
        while (*link != nullptr && *link != &target) {
            link = &(*link)->next;
        }
        if (*link == &target) {
            *link = target.next;
        }
    }

    void release(Node& node) noexcept {
        Slot* const slot = node.owner;
        KASSERT(slot != nullptr);
        libk::destroy_at(&node);
        kernel::sync::IrqLockGuard guard{storage_lock_};
        slot->occupied = false;
        slot->next_free = slot->page->free_head;
        slot->page->free_head = slot;
        KASSERT(slot->page->live_count != 0);
        --slot->page->live_count;
    }

    [[nodiscard]] auto reserve_page() const noexcept
        -> libk::Expected<kernel::resource::Reservation, MemoryError> {
        if (sponsor_ == nullptr) {
            return libk::expected(kernel::resource::Reservation{});
        }
        auto reserved = sponsor_->reserve(kernel::resource::Budget{
            .memory = page_size});
        if (!reserved) {
            return libk::unexpected(
                reserved.error() == kernel::resource::PoolError::Exhausted
                    ? MemoryError::ResourceExhausted
                    : MemoryError::InvalidState);
        }
        return libk::expected(libk::move(reserved).value());
    }

    Pmm* pmm_{};
    kernel::pager::Pager* pager_{};
    AccessMask access_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::BackingTree>
        tree_lock_{};
    kernel::sync::SpinLock<kernel::sync::LockClass::BackingStorage>
        storage_lock_{};
    Node* nodes_{};
    PageHeader* pages_{};
    bool growing_{};
    kernel::resource::Sponsorship* sponsor_{};
};

class ExtentBacking final : private libk::noncopyable_nonmovable {
public:
    ExtentBacking(
        Pmm& pmm,
        kernel::resource::Sponsorship* sponsor) noexcept
        : extents_(pmm, sponsor) {}

    ~ExtentBacking() noexcept { reset(); }

    [[nodiscard]] auto initialize(
        libk::Span<const MemoryExtent> extents) noexcept
        -> libk::Expected<void, MemoryError> {
        return extents_.initialize(extents);
    }

    [[nodiscard]] auto query(usize page_index) const noexcept -> ContentState {
        return extents_.find(page_index) != nullptr
            ? ContentState::Resident
            : ContentState::Failed;
    }

    [[nodiscard]] auto materialize(usize page_index) const noexcept
        -> libk::Expected<MemoryPage, MemoryError> {
        const MemoryExtent* const extent = extents_.find(page_index);
        if (extent == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        const usize offset = page_index - extent->object.first;
        const auto frame = extent->physical.first().frame().checked_add(offset);
        KASSERT(frame);
        return libk::expected(MemoryPage{
            .page = Page{*frame},
            .access = extent->access,
            .type = extent->type,
        });
    }

    void reset() noexcept { extents_.reset(); }

private:
    ExtentStore extents_;
};

class BootBacking final : private libk::noncopyable_nonmovable {
public:
    BootBacking(
        Pmm& pmm,
        kernel::resource::Sponsorship* sponsor) noexcept
        : extents_(pmm, sponsor) {}

    ~BootBacking() noexcept { reset(); }

    [[nodiscard]] auto initialize(
        libk::Span<const MemoryExtent> extents,
        BootOwnership ownership,
        OwnedPageGroup&& pages) noexcept
        -> libk::Expected<void, MemoryError> {
        auto initialized = extents_.initialize(extents);
        if (!initialized) {
            return initialized;
        }
        if (ownership == BootOwnership::Owned) {
            owned_ = libk::move(pages);
        }
        return libk::expected();
    }

    [[nodiscard]] auto query(usize page_index) const noexcept -> ContentState {
        return extents_.query(page_index);
    }

    [[nodiscard]] auto materialize(usize page_index) const noexcept
        -> libk::Expected<MemoryPage, MemoryError> {
        return extents_.materialize(page_index);
    }

    void reset() noexcept {
        // Owned image frames remain valid until every Mapping attachment has
        // detached; MemoryObject destroys the backing only after that point.
        owned_.reset();
        extents_.reset();
    }

private:
    ExtentBacking extents_;
    OwnedPageGroup owned_{};
};

static_assert(sizeof(AnonymousBacking) <= page_size);
static_assert(sizeof(PagerBacking) <= page_size);
static_assert(sizeof(ExtentBacking) <= page_size);
static_assert(sizeof(BootBacking) <= page_size);

} // namespace

MemoryWork::MemoryWork(MemoryWork&& other) noexcept
    : attachment_(libk::exchange(other.attachment_, nullptr)) {}

auto MemoryWork::operator=(MemoryWork&& other) noexcept -> MemoryWork& {
    if (this != &other) {
        reset();
        attachment_ = libk::exchange(other.attachment_, nullptr);
    }
    return *this;
}

MemoryWork::~MemoryWork() noexcept {
    reset();
}

void MemoryWork::reset() noexcept {
    MemoryAttachment* const attachment =
        libk::exchange(attachment_, nullptr);
    if (attachment != nullptr) {
        attachment->drop_work();
    }
}

PageMapping::~PageMapping() noexcept {
    KASSERT(owner_ == nullptr && !backing_hook_.is_linked());
}

MemoryAttachment::~MemoryAttachment() noexcept {
    const State current = static_cast<State>(
        state_.load<libk::MemoryOrder::Acquire>());
    KASSERT(current == State::Idle || current == State::Detached);
    KASSERT(owner_ == nullptr);
    KASSERT(work_.load<libk::MemoryOrder::Acquire>() == 0);
}

auto MemoryAttachment::attached() const noexcept -> bool {
    const State current = static_cast<State>(
        state_.load<libk::MemoryOrder::Acquire>());
    return current == State::Attached || current == State::Invalidating;
}

auto MemoryAttachment::busy() const noexcept -> bool {
    return work_.load<libk::MemoryOrder::Acquire>() != 0;
}

auto MemoryAttachment::detach() noexcept -> bool {
    MemoryObject* const owner = owner_;
    if (owner == nullptr) {
        return static_cast<State>(
            state_.load<libk::MemoryOrder::Acquire>()) == State::Detached
            && !busy();
    }
    return owner->detach(*this);
}

void MemoryAttachment::drop_work() noexcept {
    // detach() publishes Detached and then observes work_; this side removes
    // the last work pin and then observes Detached. Sequential consistency is
    // intentional: it forbids both sides from observing the other's old value
    // and thereby losing the final released() notification.
    const usize previous = work_.fetch_sub<libk::MemoryOrder::SeqCst>(1);
    KASSERT(previous != 0);
    if (previous == 1
        && static_cast<State>(state_.load<libk::MemoryOrder::SeqCst>())
            == State::Detached) {
        KASSERT(ops_ != nullptr && ops_->released != nullptr);
        ops_->released(context_);
    }
}

PageLease::PageLease(PageLease&& other) noexcept
    : owner_(libk::exchange(other.owner_, nullptr)),
      page_(other.page_) {}

auto PageLease::operator=(PageLease&& other) noexcept -> PageLease& {
    if (this != &other) {
        reset();
        owner_ = libk::exchange(other.owner_, nullptr);
        page_ = other.page_;
    }
    return *this;
}

PageLease::~PageLease() noexcept {
    reset();
}

void PageLease::reset() noexcept {
    MemoryObject* const owner = libk::exchange(owner_, nullptr);
    const Page page = page_.page;
    page_ = {};
    if (owner != nullptr) {
        owner->release_lease(page);
    }
}

PageTransfer::PageTransfer(PageTransfer&& other) noexcept
    : owner_(libk::exchange(other.owner_, nullptr)),
      index_(other.index_),
      page_(libk::move(other.page_)) {}

auto PageTransfer::operator=(PageTransfer&& other) noexcept -> PageTransfer& {
    if (this != &other) {
        abort();
        owner_ = libk::exchange(other.owner_, nullptr);
        index_ = other.index_;
        page_ = libk::move(other.page_);
    }
    return *this;
}

PageTransfer::~PageTransfer() noexcept {
    abort();
}

void PageTransfer::commit() noexcept {
    MemoryObject* const owner = libk::exchange(owner_, nullptr);
    if (owner != nullptr) {
        owner->finish_transfer(index_, {}, true);
    }
    page_.reset();
}

void PageTransfer::abort() noexcept {
    MemoryObject* const owner = libk::exchange(owner_, nullptr);
    if (owner != nullptr) {
        owner->finish_transfer(index_, libk::move(page_), false);
    }
    page_.reset();
}

MemoryObject::MemoryObject(Pmm& pmm, usize byte_size) noexcept
    : pmm_(&pmm) {
    if (byte_size != 0 && byte_size % page_size == 0) {
        logical_pages_ = byte_size / page_size;
    }
}

void MemoryObject::bind_sponsor(
    kernel::resource::Sponsorship& sponsor) noexcept {
    KASSERT(sponsor_ == nullptr && sponsor);
    KASSERT(state_ == MemoryState::Building && backing_ == nullptr);
    sponsor_ = &sponsor;
}

auto MemoryObject::reserve_dynamic(kernel::resource::Budget charge) noexcept
    -> libk::Expected<kernel::resource::Reservation, MemoryError> {
    if (sponsor_ == nullptr) {
        return libk::expected(kernel::resource::Reservation{});
    }
    auto reserved = sponsor_->reserve(charge);
    if (!reserved) {
        return libk::unexpected(
            reserved.error() == kernel::resource::PoolError::Exhausted
                ? MemoryError::ResourceExhausted
                : MemoryError::InvalidState);
    }
    return libk::expected(libk::move(reserved).value());
}

MemoryObject::~MemoryObject() noexcept {
    if (state_ == MemoryState::Building || state_ == MemoryState::Live) {
        retire();
    }
    KASSERT(state_ == MemoryState::Retired);
    KASSERT(!releasing_);
    KASSERT(backing_ == nullptr);
    KASSERT(backing_ops_ == nullptr);
    KASSERT(!backing_page_);
    KASSERT(!backing_sponsorship_);
    KASSERT(operations_ == 0);
    KASSERT(attachments_.empty());
}

auto MemoryObject::initialize_anonymous(AnonymousConfig config) noexcept
    -> libk::Expected<void, MemoryError> {
    return initialize_backing(
        BackingKind::Anonymous,
        {},
        config,
        BootOwnership::Borrowed,
        {},
        nullptr,
        {},
        {});
}

auto MemoryObject::initialize_physical(
    libk::Span<const MemoryExtent> extents) noexcept
    -> libk::Expected<void, MemoryError> {
    return initialize_backing(
        BackingKind::Physical,
        extents,
        {},
        BootOwnership::Borrowed,
        {},
        nullptr,
        {},
        {});
}

auto MemoryObject::initialize_boot_image(
    libk::Span<const MemoryExtent> extents,
    BootOwnership ownership,
    OwnedPageGroup&& owned) noexcept
    -> libk::Expected<void, MemoryError> {
    return initialize_backing(
        BackingKind::BootImage,
        extents,
        {},
        ownership,
        libk::move(owned),
        nullptr,
        {},
        {});
}

auto MemoryObject::initialize_pager(
    kernel::pager::Pager& pager,
    AccessMask access) noexcept
    -> libk::Expected<void, MemoryError> {
    return initialize_backing(
        BackingKind::Pager,
        {},
        {},
        BootOwnership::Borrowed,
        {},
        &pager,
        access,
        {});
}

auto MemoryObject::initialize_pager(
    object::ObjectRef&& pager,
    AccessMask access) noexcept
    -> libk::Expected<void, MemoryError> {
    auto pinned = pager.pin<kernel::pager::Pager>();
    if (!pinned) {
        return libk::unexpected(MemoryError::InvalidState);
    }
    return initialize_backing(
        BackingKind::Pager,
        {},
        {},
        BootOwnership::Borrowed,
        {},
        &pinned.value().get(),
        access,
        libk::move(pager));
}

auto MemoryObject::initialize_backing(
    BackingKind kind,
    libk::Span<const MemoryExtent> extents,
    AnonymousConfig anonymous,
    BootOwnership boot_ownership,
    OwnedPageGroup&& boot_pages,
    kernel::pager::Pager* pager,
    AccessMask pager_access,
    object::ObjectRef&& pager_ref) noexcept
    -> libk::Expected<void, MemoryError> {
    if (state_ != MemoryState::Building || logical_pages_ == 0) {
        fail_build();
        return libk::unexpected(MemoryError::InvalidSize);
    }
    if (kind == BackingKind::Anonymous) {
        if (!valid_access(anonymous.access)) {
            fail_build();
            return libk::unexpected(MemoryError::InvalidAccess);
        }
    } else if (kind == BackingKind::Pager) {
        if (pager == nullptr || !valid_access(pager_access)
            || (pager_ref && pager_ref.kind()
                != object::ObjectKind::Pager)) {
            fail_build();
            return libk::unexpected(MemoryError::InvalidState);
        }
    } else {
        auto validated = validate_extents(
            *pmm_,
            logical_pages_,
            extents,
            kind,
            boot_ownership,
            boot_pages);
        if (!validated) {
            fail_build();
            return validated;
        }
    }

    auto backing_charge = reserve_dynamic(kernel::resource::Budget{
        .memory = page_size,
    });
    if (!backing_charge) {
        fail_build();
        return libk::unexpected(backing_charge.error());
    }
    auto allocated = pmm_->allocate_page();
    if (!allocated) {
        fail_build();
        return libk::unexpected(MemoryError::OutOfMemory);
    }
    OwnedPage storage = libk::move(allocated).value();

    static const BackingOps anonymous_ops{
        .kind = BackingKind::Anonymous,
        .query = [](const void* backing, usize index) noexcept {
            return static_cast<const AnonymousBacking*>(backing)->query(index);
        },
        .materialize = [](void* backing, usize index) noexcept {
            return static_cast<AnonymousBacking*>(backing)->materialize(index);
        },
        .begin_transfer = [](void* backing, usize index) noexcept {
            return static_cast<AnonymousBacking*>(backing)->begin_transfer(index);
        },
        .restore_transfer = [](void* backing, usize index, OwnedPage&& page) noexcept {
            return static_cast<AnonymousBacking*>(backing)->restore_transfer(
                index, libk::move(page));
        },
        .commit_transfer = [](void* backing, usize index) noexcept {
            return static_cast<AnonymousBacking*>(backing)->commit_transfer(index);
        },
        .supply = nullptr,
        .fail = nullptr,
        .bind_mapping = nullptr,
        .unbind_mapping = nullptr,
        .lease_acquire = nullptr,
        .lease_release = nullptr,
        .page_state = nullptr,
        .observe_usage = nullptr,
        .queue_writeback = nullptr,
        .begin_writeback = nullptr,
        .complete_writeback = nullptr,
        .evict_page = nullptr,
        .destroy = [](void* backing) noexcept {
            libk::destroy_at(static_cast<AnonymousBacking*>(backing));
        },
    };
    static const BackingOps physical_ops{
        .kind = BackingKind::Physical,
        .query = [](const void* backing, usize index) noexcept {
            return static_cast<const ExtentBacking*>(backing)->query(index);
        },
        .materialize = [](void* backing, usize index) noexcept {
            return static_cast<ExtentBacking*>(backing)->materialize(index);
        },
        .begin_transfer = [](void*, usize) noexcept
            -> libk::Expected<OwnedPage, MemoryError> {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        },
        .restore_transfer = [](void*, usize, OwnedPage&&) noexcept
            -> libk::Expected<void, MemoryError> {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        },
        .commit_transfer = [](void*, usize) noexcept
            -> libk::Expected<void, MemoryError> {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        },
        .supply = nullptr,
        .fail = nullptr,
        .bind_mapping = nullptr,
        .unbind_mapping = nullptr,
        .lease_acquire = nullptr,
        .lease_release = nullptr,
        .page_state = nullptr,
        .observe_usage = nullptr,
        .queue_writeback = nullptr,
        .begin_writeback = nullptr,
        .complete_writeback = nullptr,
        .evict_page = nullptr,
        .destroy = [](void* backing) noexcept {
            libk::destroy_at(static_cast<ExtentBacking*>(backing));
        },
    };
    static const BackingOps boot_ops{
        .kind = BackingKind::BootImage,
        .query = [](const void* backing, usize index) noexcept {
            return static_cast<const BootBacking*>(backing)->query(index);
        },
        .materialize = [](void* backing, usize index) noexcept {
            return static_cast<BootBacking*>(backing)->materialize(index);
        },
        .begin_transfer = [](void*, usize) noexcept
            -> libk::Expected<OwnedPage, MemoryError> {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        },
        .restore_transfer = [](void*, usize, OwnedPage&&) noexcept
            -> libk::Expected<void, MemoryError> {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        },
        .commit_transfer = [](void*, usize) noexcept
            -> libk::Expected<void, MemoryError> {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        },
        .supply = nullptr,
        .fail = nullptr,
        .bind_mapping = nullptr,
        .unbind_mapping = nullptr,
        .lease_acquire = nullptr,
        .lease_release = nullptr,
        .page_state = nullptr,
        .observe_usage = nullptr,
        .queue_writeback = nullptr,
        .begin_writeback = nullptr,
        .complete_writeback = nullptr,
        .evict_page = nullptr,
        .destroy = [](void* backing) noexcept {
            libk::destroy_at(static_cast<BootBacking*>(backing));
        },
    };
    static const BackingOps pager_ops{
        .kind = BackingKind::Pager,
        .query = [](const void* backing, usize index) noexcept {
            return static_cast<const PagerBacking*>(backing)->query(index);
        },
        .materialize = [](void* backing, usize index) noexcept {
            return static_cast<PagerBacking*>(backing)->materialize(index);
        },
        .begin_transfer = [](void* backing, usize index) noexcept {
            return static_cast<PagerBacking*>(backing)->begin_transfer(index);
        },
        .restore_transfer = [](void* backing, usize index, OwnedPage&& page) noexcept {
            return static_cast<PagerBacking*>(backing)->restore_transfer(
                index, libk::move(page));
        },
        .commit_transfer = [](void* backing, usize index) noexcept {
            return static_cast<PagerBacking*>(backing)->commit_transfer(index);
        },
        .supply = [](void* backing, usize index, u64 request_generation,
                     u64 claim_generation, OwnedPage&& page,
                     u64 content_epoch) noexcept {
            return static_cast<PagerBacking*>(backing)->supply(
                index, request_generation, claim_generation,
                libk::move(page), content_epoch);
        },
        .fail = [](void* backing, usize index, u64 request_generation,
                   u64 claim_generation) noexcept {
            return static_cast<PagerBacking*>(backing)->fail(
                index, request_generation, claim_generation);
        },
        .bind_mapping = [](void* backing, usize index,
                           PageMapping& mapping) noexcept {
            return static_cast<PagerBacking*>(backing)->bind_mapping(
                index, mapping);
        },
        .unbind_mapping = [](void* backing, PageMapping& mapping) noexcept {
            return static_cast<PagerBacking*>(backing)->unbind_mapping(mapping);
        },
        .lease_acquire = [](void* backing, usize index) noexcept {
            return static_cast<PagerBacking*>(backing)->lease_acquire(index);
        },
        .lease_release = [](void* backing, Page page) noexcept {
            static_cast<PagerBacking*>(backing)->lease_release(page);
        },
        .page_state = [](const void* backing, usize index) noexcept {
            return static_cast<const PagerBacking*>(backing)->page_state(index);
        },
        .observe_usage = [](void* backing, usize index,
                            bool accessed, bool dirty) noexcept {
            return static_cast<PagerBacking*>(backing)->observe_usage(
                index, accessed, dirty);
        },
        .queue_writeback = [](void* backing, usize index) noexcept {
            return static_cast<PagerBacking*>(backing)->queue_writeback(index);
        },
        .begin_writeback = [](void* backing, usize index, u64 epoch) noexcept {
            return static_cast<PagerBacking*>(backing)->begin_writeback(
                index, epoch);
        },
        .complete_writeback = [](void* backing, usize index,
                                 u64 epoch, bool clean) noexcept {
            return static_cast<PagerBacking*>(backing)->complete_writeback(
                index, epoch, clean);
        },
        .evict_page = [](void* backing, usize index) noexcept {
            return static_cast<PagerBacking*>(backing)->evict_page(index);
        },
        .destroy = [](void* backing) noexcept {
            libk::destroy_at(static_cast<PagerBacking*>(backing));
        },
    };

    void* backend = storage.bytes();
    const BackingOps* ops{};
    libk::Expected<void, MemoryError> initialized = libk::expected();
    switch (kind) {
    case BackingKind::Anonymous: {
        auto* const backing = libk::construct_at(
            static_cast<AnonymousBacking*>(backend),
            *pmm_,
            anonymous.access,
            sponsor_);
        ops = &anonymous_ops;
        backend = backing;
        break;
    }
    case BackingKind::Physical: {
        auto* const backing = libk::construct_at(
            static_cast<ExtentBacking*>(backend), *pmm_, sponsor_);
        ops = &physical_ops;
        backend = backing;
        initialized = backing->initialize(extents);
        break;
    }
    case BackingKind::BootImage: {
        auto* const backing = libk::construct_at(
            static_cast<BootBacking*>(backend), *pmm_, sponsor_);
        ops = &boot_ops;
        backend = backing;
        initialized = backing->initialize(
            extents, boot_ownership, libk::move(boot_pages));
        break;
    }
    case BackingKind::Pager: {
        auto* const backing = libk::construct_at(
            static_cast<PagerBacking*>(backend),
            *pmm_, *pager, pager_access, sponsor_);
        ops = &pager_ops;
        backend = backing;
        break;
    }
    }

    if (!initialized) {
        ops->destroy(backend);
        storage.reset();
        const MemoryError error = initialized.error();
        fail_build();
        return libk::unexpected(error);
    }

    backing_ = backend;
    backing_ops_ = ops;
    backing_page_ = libk::move(storage);
    if (backing_charge.value()) {
        backing_sponsorship_.commit(
            libk::move(backing_charge).value());
    }
    state_ = MemoryState::Live;
    if (kind == BackingKind::Anonymous || kind == BackingKind::Pager) {
        access_ = kind == BackingKind::Anonymous ? anonymous.access : pager_access;
        if (kind == BackingKind::Pager) {
            pager_ref_ = libk::move(pager_ref);
        }
    } else {
        u8 access_bits{};
        for (const MemoryExtent& extent : extents) {
            access_bits |= extent.access.raw();
        }
        access_ = AccessMask::from_raw(access_bits);
    }
    if (kind != BackingKind::Anonymous && kind != BackingKind::Pager) {
        for (const MemoryExtent& extent : extents) {
            if (extent.access.contains(Access::Execute)) {
                seal_ = SealState::Executable;
                content_epoch_ = ContentEpoch{1};
                break;
            }
        }
    }

    if (kind == BackingKind::Anonymous && anonymous.eager) {
        for (usize index = 0; index < logical_pages_; ++index) {
            auto page = materialize(index);
            if (!page) {
                const MemoryError error = page.error();
                retire();
                return libk::unexpected(error);
            }
        }
    }
    return libk::expected();
}

auto MemoryObject::kind() const noexcept -> BackingKind {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(backing_ops_ != nullptr);
    return backing_ops_->kind;
}

auto MemoryObject::state() const noexcept -> MemoryState {
    kernel::sync::IrqLockGuard guard{lock_};
    return state_;
}

auto MemoryObject::seal_state() const noexcept -> SealState {
    kernel::sync::IrqLockGuard guard{lock_};
    return seal_;
}

auto MemoryObject::content_epoch() const noexcept -> ContentEpoch {
    kernel::sync::IrqLockGuard guard{lock_};
    return content_epoch_;
}

auto MemoryObject::seal() noexcept -> libk::Expected<void, MemoryError> {
    kernel::sync::IrqLockGuard guard{lock_};
    if (state_ != MemoryState::Live || seal_ != SealState::Loadable) {
        return libk::unexpected(MemoryError::InvalidState);
    }
    seal_ = SealState::Sealing;
    for (const MemoryAttachment& attachment : attachments_) {
        if (attachment.access_.contains(Access::Write)) {
            seal_ = SealState::Loadable;
            return libk::unexpected(MemoryError::Busy);
        }
    }
    KASSERT(content_epoch_.raw != libk::numeric_limits<u64>::max());
    content_epoch_ = ContentEpoch{content_epoch_.raw + 1};
    seal_ = SealState::Executable;
    return libk::expected();
}

auto MemoryObject::query(usize page_index) const noexcept
    -> libk::Expected<ContentState, MemoryError> {
    const BackingOps* ops{};
    const void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        if (page_index >= logical_pages_) {
            return libk::unexpected(MemoryError::InvalidRange);
        }
        KASSERT(operations_ != libk::numeric_limits<usize>::max());
        ++const_cast<MemoryObject*>(this)->operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    const ContentState result = ops->query(backing, page_index);
    const_cast<MemoryObject*>(this)->drop_page();
    return libk::expected(result);
}

auto MemoryObject::materialize(usize page_index) noexcept
    -> libk::Expected<PageLease, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        if (page_index >= logical_pages_) {
            return libk::unexpected(MemoryError::InvalidRange);
        }
        KASSERT(operations_ != libk::numeric_limits<usize>::max());
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }

    auto result = ops->materialize(backing, page_index);
    bool live{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        live = state_ == MemoryState::Live;
        if (!result || !live) {
            KASSERT(operations_ != 0);
            --operations_;
        }
    }
    if (!result || !live) {
        finish_retire();
        return !live
            ? libk::Expected<PageLease, MemoryError>{
                  libk::unexpected(MemoryError::InvalidState)}
            : libk::Expected<PageLease, MemoryError>{
                  libk::unexpected(result.error())};
    }
    if (ops->lease_acquire != nullptr) {
        auto acquired = ops->lease_acquire(backing, page_index);
        if (!acquired) {
            {
                kernel::sync::IrqLockGuard guard{lock_};
                KASSERT(operations_ != 0);
                --operations_;
            }
            finish_retire();
            return libk::unexpected(acquired.error());
        }
    }
    return libk::expected(PageLease{*this, result.value()});
}

auto MemoryObject::begin_transfer(usize page_index) noexcept
    -> libk::Expected<PageTransfer, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || !attachments_.empty()
            || page_index >= logical_pages_ || backing_ops_ == nullptr
            || backing_ops_->begin_transfer == nullptr) {
            return libk::unexpected(
                !attachments_.empty() ? MemoryError::Busy
                                       : MemoryError::InvalidState);
        }
        KASSERT(operations_ != libk::numeric_limits<usize>::max());
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto page = ops->begin_transfer(backing, page_index);
    if (!page) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(operations_ != 0);
            --operations_;
        }
        finish_retire();
        return libk::unexpected(page.error());
    }
    return libk::expected(PageTransfer{
        *this, page_index, libk::move(page).value()});
}

auto MemoryObject::pager_supply(
    usize page_index,
    u64 request_generation,
    u64 claim_generation,
    OwnedPage&& page,
    u64 content_epoch) noexcept
    -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || backing_ops_ == nullptr
            || backing_ops_->supply == nullptr
            || page_index >= logical_pages_) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->supply(
        backing, page_index, request_generation, claim_generation,
        libk::move(page), content_epoch);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0);
        --operations_;
    }
    finish_retire();
    return result;
}

auto MemoryObject::pager_supply_transfer(
    PageTransfer&& transfer,
    usize page_index,
    u64 request_generation,
    u64 claim_generation,
    u64 content_epoch) noexcept
    -> libk::Expected<void, MemoryError> {
    if (!transfer || !transfer.page().valid()) {
        return libk::unexpected(MemoryError::OwnershipMismatch);
    }
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || backing_ops_ == nullptr
            || backing_ops_->supply == nullptr
            || page_index >= logical_pages_) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->supply(
        backing, page_index, request_generation, claim_generation,
        libk::move(transfer.page_), content_epoch);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0);
        --operations_;
    }
    finish_retire();
    if (!result) {
        transfer.abort();
        return result;
    }
    transfer.commit();
    return result;
}

auto MemoryObject::pager_fail(
    usize page_index,
    u64 request_generation,
    u64 claim_generation) noexcept
    -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || backing_ops_ == nullptr
            || backing_ops_->fail == nullptr
            || page_index >= logical_pages_) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->fail(
        backing, page_index, request_generation, claim_generation);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0);
        --operations_;
    }
    finish_retire();
    return result;
}

auto MemoryObject::bind_mapping(
    PageMapping& mapping,
    usize page_index) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || mapping.owner_ != nullptr
            || backing_ops_ == nullptr) {
            return libk::unexpected(
                state_ != MemoryState::Live
                    ? MemoryError::InvalidState
                    : MemoryError::AttachmentState);
        }
        ++operations_;
        mapping.page_index_ = page_index;
        ops = backing_ops_;
        backing = backing_;
    }
    auto linked = ops->bind_mapping != nullptr
        ? ops->bind_mapping(backing, page_index, mapping)
        : libk::Expected<void, MemoryError>{libk::expected()};
    if (!linked) {
        mapping.page_index_ = 0;
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(operations_ != 0);
            --operations_;
        }
        finish_retire();
        return linked;
    }
    bool accepted{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        accepted = state_ == MemoryState::Live && mapping.owner_ == nullptr;
        if (accepted) {
            mapping.owner_ = this;
            KASSERT(mapping_count_ != libk::numeric_limits<usize>::max());
            ++mapping_count_;
        }
        KASSERT(operations_ != 0);
        --operations_;
    }
    if (!accepted) {
        if (ops->unbind_mapping != nullptr) {
            static_cast<void>(ops->unbind_mapping(backing, mapping));
        }
        mapping.page_index_ = 0;
        finish_retire();
        return libk::unexpected(MemoryError::InvalidState);
    }
    finish_retire();
    return libk::expected();
}

auto MemoryObject::unbind_mapping(
    PageMapping& mapping) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (mapping.owner_ != this || backing_ops_ == nullptr) {
            return libk::unexpected(MemoryError::AttachmentState);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto unlinked = ops->unbind_mapping != nullptr
        ? ops->unbind_mapping(backing, mapping)
        : libk::Expected<void, MemoryError>{libk::expected()};
    if (unlinked) {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(mapping.owner_ == this && mapping_count_ != 0);
        mapping.owner_ = nullptr;
        mapping.page_index_ = 0;
        --mapping_count_;
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0);
        --operations_;
    }
    finish_retire();
    return unlinked;
}

auto MemoryObject::page_state(usize page_index) const noexcept
    -> libk::Expected<PageSlotState, MemoryError> {
    const BackingOps* ops{};
    const void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || backing_ops_ == nullptr) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        ++const_cast<MemoryObject*>(this)->operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->page_state != nullptr
        ? ops->page_state(backing, page_index)
        : libk::Expected<PageSlotState, MemoryError>{
              libk::expected(PageSlotState::ResidentClean)};
    const_cast<MemoryObject*>(this)->drop_page();
    return result;
}

auto MemoryObject::observe_usage(
    usize page_index,
    bool accessed,
    bool dirty) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || backing_ops_ == nullptr) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->observe_usage != nullptr
        ? ops->observe_usage(backing, page_index, accessed, dirty)
        : libk::Expected<void, MemoryError>{libk::expected()};
    drop_page();
    return result;
}

auto MemoryObject::queue_writeback(usize page_index) noexcept
    -> libk::Expected<u64, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || backing_ops_ == nullptr
            || backing_ops_->queue_writeback == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->queue_writeback(backing, page_index);
    drop_page();
    return result;
}

auto MemoryObject::begin_writeback(
    usize page_index,
    u64 epoch) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || backing_ops_ == nullptr
            || backing_ops_->begin_writeback == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->begin_writeback(backing, page_index, epoch);
    drop_page();
    return result;
}

auto MemoryObject::complete_writeback(
    usize page_index,
    u64 epoch,
    bool clean) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || backing_ops_ == nullptr
            || backing_ops_->complete_writeback == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->complete_writeback(
        backing, page_index, epoch, clean);
    drop_page();
    return result;
}

auto MemoryObject::evict_page(usize page_index) noexcept
    -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || backing_ops_ == nullptr
            || backing_ops_->evict_page == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->evict_page(backing, page_index);
    drop_page();
    return result;
}

auto MemoryObject::read(usize offset, libk::Span<byte> output) noexcept
    -> libk::Expected<void, MemoryError> {
    const auto end = libk::checked_add(offset, output.size());
    if (!end || *end > size()) {
        return libk::unexpected(MemoryError::InvalidRange);
    }
    if (!access_.contains(Access::Read)) {
        return libk::unexpected(MemoryError::InvalidAccess);
    }

    usize copied{};
    while (copied < output.size()) {
        const usize position = offset + copied;
        const usize page_index = position / page_size;
        const usize page_offset = position & (page_size - 1);
        auto lease = materialize(page_index);
        if (!lease) {
            return libk::unexpected(lease.error());
        }
        const usize available = page_size - page_offset;
        const usize remaining = output.size() - copied;
        const usize amount = remaining < available ? remaining : available;
        const byte* const source = pmm_->bytes(lease.value().page().page)
            + page_offset;
        memcpy(output.data() + copied, source, amount);
        copied += amount;
    }
    return libk::expected();
}

auto MemoryObject::attach(
    MemoryAttachment& attachment,
    AccessMask access) noexcept
    -> libk::Expected<void, MemoryError> {
    kernel::sync::IrqLockGuard guard{lock_};
    if (state_ != MemoryState::Live || !valid_access(access)
        || !access_.contains(access)) {
        return libk::unexpected(MemoryError::InvalidState);
    }
    if ((access.contains(Access::Execute)
            && seal_ != SealState::Executable)
        || (access.contains(Access::Write)
            && seal_ != SealState::Loadable)) {
        return libk::unexpected(MemoryError::InvalidAccess);
    }
    KASSERT(!access.contains(Access::Execute) || content_epoch_.raw != 0);
    if (attachment.owner_ != nullptr
        || static_cast<MemoryAttachment::State>(
            attachment.state_.load<libk::MemoryOrder::Relaxed>())
            != MemoryAttachment::State::Idle
        || attachment.ops_ == nullptr
        || attachment.ops_->invalidate == nullptr
        || attachment.ops_->released == nullptr) {
        return libk::unexpected(MemoryError::AttachmentState);
    }
    attachment.owner_ = this;
    attachment.access_ = access;
    attachment.state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(MemoryAttachment::State::Attached));
    attachments_.push_back(attachment);
    return libk::expected();
}

auto MemoryObject::attachment_count() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    usize count{};
    for ([[maybe_unused]] const MemoryAttachment& attachment : attachments_) {
        ++count;
    }
    return count;
}

auto MemoryObject::detach(MemoryAttachment& attachment) noexcept -> bool {
    bool quiescent{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (attachment.owner_ != this) {
            return false;
        }
        const auto current = static_cast<MemoryAttachment::State>(
            attachment.state_.load<libk::MemoryOrder::Relaxed>());
        KASSERT(current == MemoryAttachment::State::Attached
            || current == MemoryAttachment::State::Invalidating);
        attachments_.erase(attachment);
        attachment.owner_ = nullptr;
        attachment.state_.store<libk::MemoryOrder::SeqCst>(
            static_cast<u8>(MemoryAttachment::State::Detached));
        quiescent = attachment.work_.load<libk::MemoryOrder::SeqCst>() == 0;
    }
    finish_retire();
    return quiescent;
}

void MemoryObject::retire() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == MemoryState::Retired
            || state_ == MemoryState::Stopping) {
            return;
        }
        if (state_ == MemoryState::Building) {
            state_ = MemoryState::Stopping;
        } else {
            KASSERT(state_ == MemoryState::Live);
            state_ = MemoryState::Stopping;
        }
    }

    for (;;) {
        MemoryAttachment* target{};
        MemoryWork work{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            for (MemoryAttachment& attachment : attachments_) {
                const auto current = static_cast<MemoryAttachment::State>(
                    attachment.state_.load<libk::MemoryOrder::Relaxed>());
                if (current != MemoryAttachment::State::Attached) {
                    continue;
                }
                KASSERT(attachment.work_.load<
                    libk::MemoryOrder::Relaxed>()
                    != libk::numeric_limits<usize>::max());
                [[maybe_unused]] const usize previous =
                    attachment.work_.fetch_add<
                        libk::MemoryOrder::Relaxed>(1);
                attachment.state_.store<libk::MemoryOrder::Release>(
                    static_cast<u8>(MemoryAttachment::State::Invalidating));
                target = &attachment;
                work = MemoryWork{attachment};
                break;
            }
        }
        if (target == nullptr) {
            break;
        }
        target->ops_->invalidate(
            target->context_, libk::move(work), MemoryInvalidation::Destroy);
    }
    finish_retire();
}

void MemoryObject::drop_page() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0);
        --operations_;
    }
    finish_retire();
}

void MemoryObject::release_lease(Page page) noexcept {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0 && backing_ops_ != nullptr);
        ops = backing_ops_;
        backing = backing_;
    }
    if (ops->lease_release != nullptr) {
        ops->lease_release(backing, page);
    }
    drop_page();
}

void MemoryObject::finish_transfer(
    usize page_index,
    OwnedPage&& page,
    bool commit) noexcept {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0 && backing_ops_ != nullptr);
        ops = backing_ops_;
        backing = backing_;
    }
    const auto result = commit
        ? ops->commit_transfer(backing, page_index)
        : ops->restore_transfer(backing, page_index, libk::move(page));
    KASSERT(result);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0);
        --operations_;
    }
    finish_retire();
}

void MemoryObject::finish_retire() noexcept {
    void* backing{};
    const BackingOps* ops{};
    OwnedPage storage{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Stopping
            || operations_ != 0
            || mapping_count_ != 0
            || !attachments_.empty()
            || releasing_) {
            return;
        }
        releasing_ = true;
        backing = backing_;
        ops = backing_ops_;
        backing_ = nullptr;
        backing_ops_ = nullptr;
        storage = libk::move(backing_page_);
    }

    if (backing != nullptr) {
        KASSERT(ops != nullptr);
        ops->destroy(backing);
    }
    auto backing_refund = backing_sponsorship_.detach();
    storage.reset();
    backing_refund.complete();

    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(releasing_);
        releasing_ = false;
        state_ = MemoryState::Retired;
    }
}

void MemoryObject::fail_build() noexcept {
    KASSERT(state_ == MemoryState::Building);
    KASSERT(backing_ == nullptr && backing_ops_ == nullptr);
    state_ = MemoryState::Retired;
}

} // namespace kernel::mm
