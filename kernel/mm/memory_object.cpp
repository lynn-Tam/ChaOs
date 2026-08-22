#include <mm/memory_object.hpp>
#include <mm/memory_work.hpp>

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
                return libk::unexpected(MemoryError::Pressure);
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

    /*luna change: thread one optional reservation through anonymous growth,
      reason: PMM pressure can occur before any durable PageKey exists*/
    [[nodiscard]] auto materialize(
        usize page_index,
        FrameDemand* demand) noexcept
        -> libk::Expected<MemoryPage, MemoryError> {
        for (;;) {
            {
                kernel::sync::IrqLockGuard guard{tree_lock_};
                const Node* const existing = tree_.find(page_index);
                if (existing != nullptr) {
                    if (demand != nullptr) {
                        demand->reset();
                    }
                    return page_of(*existing);
                }
            }

            auto claimed = claim(page_index, demand);
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

            kernel::resource::Reservation charge{};
            if (demand != nullptr && *demand) {
                auto retained = demand->take();
                KASSERT(retained.has_value());
                charge = libk::move(retained).value();
            } else {
                auto reserved = reserve_page();
                if (!reserved) {
                    rollback(*candidate);
                    return libk::unexpected(reserved.error());
                }
                charge = libk::move(reserved).value();
            }
            auto allocated = pmm_->allocate_page();
            if (!allocated) {
                if (demand != nullptr) {
                    demand->emplace(libk::move(charge));
                }
                rollback(*candidate);
                return libk::unexpected(MemoryError::Pressure);
            }
            OwnedPage resident = libk::move(allocated).value();
            memset(resident.bytes(), 0, page_size);
            const Page page = resident.page();
            {
                kernel::sync::IrqLockGuard guard{tree_lock_};
                KASSERT(candidate->state == ContentState::Busy);
                candidate->resident = libk::move(resident);
                if (charge) {
                    candidate->resident_sponsorship.commit(
                        libk::move(charge));
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

    /*luna change: reuse the continuation reservation for metadata pages,
      reason: storage growth and resident allocation share the same PMM edge*/
    [[nodiscard]] auto claim(
        usize page_index,
        FrameDemand* demand) noexcept
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

            kernel::resource::Reservation charge{};
            if (demand != nullptr && *demand) {
                auto retained = demand->take();
                KASSERT(retained.has_value());
                charge = libk::move(retained).value();
            } else {
                auto reserved = reserve_page();
                if (!reserved) {
                    kernel::sync::IrqLockGuard guard{storage_lock_};
                    KASSERT(growing_);
                    growing_ = false;
                    return libk::unexpected(reserved.error());
                }
                charge = libk::move(reserved).value();
            }
            auto allocated = pmm_->allocate_page();
            if (!allocated) {
                if (demand != nullptr) {
                    demand->emplace(libk::move(charge));
                }
                kernel::sync::IrqLockGuard guard{storage_lock_};
                KASSERT(growing_);
                growing_ = false;
                return libk::unexpected(MemoryError::Pressure);
            }
            OwnedPage backing = libk::move(allocated).value();
            /*luna change: move the plain retained charge into metadata,
              reason: demand transfer already consumed the optional wrapper*/
            auto* const page = libk::construct_at(
                reinterpret_cast<PageHeader*>(backing.bytes()),
                libk::move(backing),
                libk::move(charge));
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

} // namespace

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
        /*luna change: replace hand-linked work and ready metadata with intrusive hooks, reason: libk owns membership and O(1) erase without parallel flags*/
        libk::IntrusiveListHook work_hook_{};
        libk::IntrusiveListHook ready_hook_{};
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
        MemoryObject& owner,
        Pmm& pmm,
        kernel::pager::Pager& pager,
        AccessMask access,
        kernel::resource::Sponsorship* sponsor) noexcept
        : owner_(&owner), pmm_(&pmm), pager_(&pager), access_(access),
          sponsor_(sponsor) {
        static_assert(slots_per_page != 0);
        attachment_ = kernel::pager::PagerAttachment{
            .context = this,
            .transition = &PagerBacking::request_transition,
            .drained = &PagerBacking::request_drained,
            .ready = &PagerBacking::capacity_ready,
        };
    }

    [[nodiscard]] auto init() noexcept -> libk::Expected<void, MemoryError> {
        return pager_->attach(attachment_)
            ? libk::expected()
            : libk::Expected<void, MemoryError>{
                  libk::unexpected(MemoryError::AttachmentState)};
    }

    ~PagerBacking() noexcept { reset(); }

    void stop() noexcept {
        if (attachment_.state != kernel::pager::PagerAttachment::State::Detached) {
            static_cast<void>(pager_->detach(attachment_));
        }
    }

    static auto request_transition(
        void* context,
        const kernel::pager::Request& request,
        kernel::pager::PagerAttachment::Event event) noexcept -> bool {
        auto& self = *static_cast<PagerBacking*>(context);
        kernel::sync::IrqLockGuard guard{self.tree_lock_};
        Node* const node = self.find_locked(request.page_key.index);
        if (node == nullptr || node->slot.request.key.generation
                != request.page_key.generation) {
            return false;
        }
        if (request.kind == kernel::pager::DeliveryKind::Writeback) {
            const WritebackKey key{
                request.page_key,
                request.writeback_generation,
                request.dirty_epoch};
            switch (event) {
            case kernel::pager::PagerAttachment::Event::Claim:
                return static_cast<bool>(node->slot.claim_writeback(
                    key, request.key.generation, request.claim.generation));
            case kernel::pager::PagerAttachment::Event::Requeue:
                return static_cast<bool>(node->slot.requeue_writeback(
                    key, request.key.generation, request.claim.generation));
            case kernel::pager::PagerAttachment::Event::Forced:
                return static_cast<bool>(node->slot.fail_writeback(
                    key, request.key.generation,
                    node->slot.state == PageSlotState::WritebackPublished
                        ? 0 : request.claim.generation,
                    WritebackFailure::BackingUnavailable));
            }
            return false;
        }
        // Page-in supply/fail owns the target semantic transition. Claim is
        // transport admission only; forced close invalidates the request.
        if (event == kernel::pager::PagerAttachment::Event::Forced) {
            const u64 claim = node->slot.request.claim_generation;
            const bool failed = static_cast<bool>(node->slot.fail(claim));
            if (!node->slot.request.waiters.empty()) {
                self.index_ready_locked(*node);
            }
            return failed;
        }
        return true;
    }

    static void request_drained(void* context) noexcept {
        auto& self = *static_cast<PagerBacking*>(context);
        self.owner_->release_backing_hold();
    }

    static void capacity_ready(void* context) noexcept {
        auto& self = *static_cast<PagerBacking*>(context);
        /*luna change: rearm the existing MemoryExecutor on Pager capacity,
          reason: the callback is only a producer wake and PageSlot retains
          the actual writeback/page obligation*/
        self.owner_->schedule_work();
    }


    [[nodiscard]] auto drain_page_waiters(
        usize capacity) noexcept -> usize {
        if (capacity == 0) {
            return 0;
        }
        constexpr usize budget = 8;
        const usize limit = capacity < budget ? capacity : budget;
        WaitClaim batch[budget]{};
        usize claimed_count{};
        usize inspected{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            for (auto it = ready_.begin();
                 it != ready_.end() && inspected < limit;) {
                Node& node = *it++;
                ++inspected;
                if ((node.slot.state == PageSlotState::ResidentClean
                        || node.slot.state == PageSlotState::ResidentDirty
                        || node.slot.state == PageSlotState::Failed)
                    && !node.slot.request.waiters.empty()) {
                    const PageWaitResult result =
                        node.slot.request.terminal_result();
                    claimed_count += node.slot.request.claim_waiters(
                        batch + claimed_count, limit - claimed_count, result);
                }
                if (node.slot.request.waiters.empty()) {
                    unindex_ready_locked(node);
                }
            }
            /*luna change: finalize waiter relations before callbacks, reason: PageRequest host ownership must close the reuse window before waking Thread/Vproc*/
            for (usize index = 0; index < claimed_count; ++index) {
                PageRequest* const request = batch[index].request();
                KASSERT(request != nullptr
                    && request->finish_claim(batch[index]));
            }
        }
        /*luna change: reset finalized waiter tokens directly after callback, reason: host ownership already updated the ready index and no PageRequest address scan is safe*/
        usize published{};
        for (usize index = 0; index < claimed_count; ++index) {
            KASSERT(batch[index].publish());
            KASSERT(batch[index].release());
            ++published;
            batch[index].reset();
        }
        return published;
    }

    /*luna change: publish retained requests through an explicit bounded transport phase, reason: Pager is only transport while PageRequest remains semantic truth and slot zero stays valid*/
    [[nodiscard]] auto service(usize capacity) noexcept -> MemoryServiceBatch {
        if (capacity == 0) {
            return {};
        }
        usize processed{};
        usize progressed{};
        bool capacity_wait{};
        while (processed < capacity) {
            Node* node{};
            PageKey page_key{};
            usize first{};
            usize count{};
            {
                kernel::sync::IrqLockGuard guard{tree_lock_};
                auto it = work_.begin();
                while (it != work_.end()) {
                    Node& candidate = *it++;
                    ++processed;
                    if (candidate.slot.state == PageSlotState::Requested
                        && candidate.slot.request.state
                            == PageRequestState::Queued) {
                        node = &candidate;
                        break;
                    }
                    unindex_work_locked(candidate);
                    if (processed == capacity) {
                        break;
                    }
                }
                if (node == nullptr) {
                    break;
                }
                if (!node->slot.request.begin_publish()) {
                    unindex_work_locked(*node);
                    continue;
                }
                page_key = node->slot.request.key;
                first = node->slot.request.first;
                count = node->slot.request.count;
            }

            const auto published = pager_->publish(
                attachment_, page_key, first, count, page_key.generation);
            if (!published) {
                bool terminal{};
                {
                    kernel::sync::IrqLockGuard guard{tree_lock_};
                    Node* const current = find_locked(page_key.index);
                    if (current != nullptr
                        && current->slot.request.key == page_key
                        && current->slot.request.state
                            == PageRequestState::Publishing) {
                        KASSERT(current->slot.request.abort_publish());
                        if (published.error() != kernel::pager::Error::Full) {
                            if (current->slot.fail(0)) {
                                unindex_work_locked(*current);
                                if (!current->slot.request.waiters.empty()) {
                                    index_ready_locked(*current);
                                }
                                terminal = true;
                            }
                        }
                    }
                }
                if (published.error() == kernel::pager::Error::Full) {
                    /*luna change: reindex the queued PageRequest on Pager
                      Full, reason: the attachment callback re-arms this
                      canonical work index after capacity returns*/
                    kernel::sync::IrqLockGuard guard{tree_lock_};
                    Node* const current = find_locked(page_key.index);
                    if (current != nullptr
                        && current->slot.request.key == page_key
                        && current->slot.request.state
                            == PageRequestState::Queued) {
                        index_work_locked(*current);
                    }
                    capacity_wait = true;
                    break;
                }
                if (terminal) {
                    ++progressed;
                }
                continue;
            }

            bool committed{};
            {
                kernel::sync::IrqLockGuard guard{tree_lock_};
                Node* const current = find_locked(page_key.index);
                if (current != nullptr
                    && current->slot.request.key == page_key
                        && current->slot.state == PageSlotState::Requested
                        && current->slot.request.publish()) {
                    unindex_work_locked(*current);
                    committed = true;
                }
            }
            if (!committed) {
                static_cast<void>(pager_->cancel(published.value().key));
                kernel::sync::IrqLockGuard guard{tree_lock_};
                Node* const current = find_locked(page_key.index);
                if (current != nullptr
                    && current->slot.request.key == page_key
                    && current->slot.request.state
                        == PageRequestState::Publishing) {
                    KASSERT(current->slot.request.abort_publish());
                }
                continue;
            }
            ++progressed;
        }

        const usize remaining = capacity - processed;
        progressed += drain_page_waiters(remaining);
        if (remaining != 0) {
            usize page_index{};
            WritebackKey key{};
            bool queued{};
            {
                kernel::sync::IrqLockGuard guard{tree_lock_};
                if (nodes_ != nullptr) {
                    if (reclaim_cursor_ == nullptr) {
                        reclaim_cursor_ = nodes_;
                    }
                    Node* node = reclaim_cursor_;
                    for (usize inspected = 0;
                         node != nullptr && inspected < remaining;
                         ++inspected) {
                        Node* const next = node->next != nullptr
                            ? node->next : nodes_;
                        reclaim_cursor_ = next;
                        if (node->slot.state == PageSlotState::WritebackQueued
                            && node->slot.writeback.retained) {
                            page_index = node->index;
                            key = WritebackKey{
                                .page = node->slot.request.key,
                                .generation =
                                    node->slot.writeback.generation,
                                .dirty_epoch = node->slot.writeback.dirty_epoch};
                            queued = true;
                            break;
                        }
                        node = next;
                    }
                }
            }
            if (queued) {
                /*luna change: publish retained writeback outside the tree
                  lock, reason: Pager transport is foreign and PageSlot keeps
                  the obligation through Full/requeue/failure*/
                auto published = publish_writeback(page_index, key);
                capacity_wait = !published
                    && published.error() == MemoryError::Pending;
                if (published) {
                    ++progressed;
                }
            }
        }
        bool more{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            more = !capacity_wait && (!work_.empty() || !ready_.empty());
        }
        return MemoryServiceBatch{
            .processed = processed,
            .progressed = progressed,
            .more = more};
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

    /*luna change: inspect a fixed Node window and admit one canonical action,
      reason: cold reclaim must be bounded while PageSlot owns intent and
      PageMapping owns exact invalidation publication*/
    [[nodiscard]] auto reclaim(usize capacity) noexcept -> ReclaimResult {
        if (capacity == 0) {
            return ReclaimResult::Idle;
        }
        PageMapping* mapping{};
        MemoryWork work{};
        usize evict_page_index{};
        ReclaimResult result = ReclaimResult::Idle;
        bool evict{};
        bool writeback{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            if (nodes_ == nullptr) {
                return ReclaimResult::Idle;
            } else {
                if (reclaim_cursor_ == nullptr) {
                    reclaim_cursor_ = nodes_;
                }
                Node* const start = reclaim_cursor_;
                Node* node = start;
                usize inspected{};
                bool complete{};
                for (; node != nullptr && inspected < capacity; ++inspected) {
                Node* const next = node->next != nullptr
                    ? node->next : nodes_;
                reclaim_cursor_ = next;
                complete = complete || next == start;
                if (node->accessed_epoch != 0) {
                    node->accessed_epoch = 0;
                    node = next;
                    continue;
                }
                switch (node->slot.state) {
                case PageSlotState::ResidentClean:
                case PageSlotState::ResidentDirty: {
                    auto retained = node->slot.retain_reclaim();
                    if (!retained) {
                        node = next;
                        continue;
                    }
                    if (!node->mappings.empty()) {
                        PageMapping& candidate = node->mappings.front();
                        work = candidate.claim();
                        if (work) {
                            mapping = &candidate;
                            result = ReclaimResult::Wait;
                        } else {
                            result = ReclaimResult::Wait;
                        }
                    } else if (node->leases != 0) {
                        result = ReclaimResult::Wait;
                    } else if (node->slot.state == PageSlotState::ResidentDirty) {
                        auto queued = node->slot.queue_writeback();
                        if (queued) {
                            writeback = true;
                            result = ReclaimResult::Wait;
                        } else {
                            result = ReclaimResult::More;
                        }
                    } else {
                        evict_page_index = node->index;
                        evict = true;
                        result = ReclaimResult::More;
                    }
                    node = nullptr;
                    break;
                }
                case PageSlotState::WritebackQueued:
                case PageSlotState::WritebackPublishing:
                case PageSlotState::WritebackPublished:
                case PageSlotState::WritebackActive:
                case PageSlotState::WritebackCompleting:
                    result = node->slot.reclaim_intent
                        ? ReclaimResult::Wait
                        : ReclaimResult::Idle;
                    node = nullptr;
                    break;
                case PageSlotState::WritebackFailed:
                    result = ReclaimResult::Idle;
                    node = nullptr;
                    break;
                default:
                    node = next;
                    break;
                }
                }
            /*luna change: anchor cycle completion to this pass cursor,
              reason: reaching the list head does not prove a full scan*/
            if (node != nullptr && inspected == capacity && !complete) {
                result = ReclaimResult::More;
            }
            }
        }
        ReclaimResult final = result;
        if (mapping != nullptr) {
            /*luna change: publish an exact mapping claim immediately after the
              tree lock, reason: no fallible policy may strand PageMapping*/
            mapping->publish(libk::move(work));
        } else if (writeback) {
            owner_->schedule_work();
        } else if (evict) {
            auto finished = evict_page(evict_page_index);
            if (finished) {
                final = ReclaimResult::Progress;
            } else {
                final = finished.error() == MemoryError::Busy
                    ? ReclaimResult::Wait : ReclaimResult::More;
            }
        }
        return final;
    }

    /*luna change: A -> A', reason: an evicted Missing Node retains metadata
      but must re-enter the same PageRequest lifecycle as a newly grown Node*/
    [[nodiscard]] auto materialize(
        usize page_index,
        FrameDemand* demand,
        WaitRelation* relation,
        void* owner,
        WaitRelation::Publish publish) noexcept
        -> libk::Expected<MemoryPage, MemoryError> {
        Node* node{};
        bool fresh{};
        bool queued{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            node = find_locked(page_index);
            if (node != nullptr) {
                if (demand != nullptr) {
                    demand->reset();
                }
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
                if (node->slot.state != PageSlotState::Missing) {
                    if (relation != nullptr
                        && !node->slot.request.attach(
                            *relation, owner, publish)) {
                        return libk::unexpected(MemoryError::Busy);
                    }
                    return libk::unexpected(MemoryError::Pending);
                }
            } else {
                auto claimed = claim(demand);
                if (!claimed) {
                    /*luna change: reject pre-attach storage contention for relation callers, reason: transient growth has no durable waiter or wakeup to receive the operations pin*/
                    if (relation != nullptr
                        && claimed.error() == MemoryError::Pending) {
                        return libk::unexpected(MemoryError::Busy);
                    }
                    return libk::unexpected(claimed.error());
                }
                node = claimed.value();
                fresh = true;
                if (demand != nullptr) {
                    demand->reset();
                }
                node->index = page_index;
                node->next = nodes_;
                nodes_ = node;
            }
            const u64 generation = node->slot.generation ==
                    libk::numeric_limits<u64>::max()
                ? 0
                : node->slot.generation + 1;
            if (generation == 0
                || !node->slot.begin_request(
                    PageKey{generation, page_index}, page_index, 1)) {
                if (fresh) {
                    unlink_locked(*node);
                    release(*node);
                }
                return libk::unexpected(MemoryError::GenerationExhausted);
            }
            if (relation != nullptr
                && !node->slot.request.attach(*relation, owner, publish)) {
                node->slot.cancel_request();
                if (fresh) {
                    unlink_locked(*node);
                    release(*node);
                }
                return libk::unexpected(MemoryError::Busy);
            }
            index_work_locked(*node);
            queued = true;
        }
        if (queued) {
            /*luna change: retain queued page work for MemoryExecutor service, reason: Pager admission must occur outside the PageRequest owner lock and survive queue pressure*/
            owner_->schedule_work();
        }
        return libk::unexpected(MemoryError::Pending);
    }

    [[nodiscard]] auto cancel_fault(
        WaitRelation& relation,
        u64 generation) noexcept -> bool {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        PageRequest* const request = relation.request;
        if (request == nullptr || !request->detach(relation, generation)) {
            return false;
        }
        return true;
    }

    [[nodiscard]] auto supply(
        kernel::pager::Pager& pager,
        usize page_index,
        PageKey page_key,
        kernel::pager::ClaimKey claim,
        OwnedPage&& page,
        u64 content_epoch) noexcept
        -> libk::Expected<void, MemoryError> {
        if (&pager != pager_) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        if (!page || !page_key || !claim || content_epoch == 0) {
            return libk::unexpected(MemoryError::InvalidRange);
        }
        // The service must have claimed the exact Pager record before it can
        // donate ownership.  This check is an authority edge, not page state.
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr || node->slot.request.key != page_key) {
                return libk::unexpected(MemoryError::OwnershipMismatch);
            }
        }
        auto reply = pager_->begin_reply(claim);
        if (!reply) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        const auto& claimed = reply.value().request();
        if (reply.value().attachment() != &attachment_
            || claimed.kind != kernel::pager::DeliveryKind::PageIn
            || claimed.key != claim.delivery
            || claimed.claim != claim
            || claimed.page_key != page_key
            || page_index < claimed.first
            || page_index - claimed.first >= claimed.count) {
            static_cast<void>(reply.value().abort());
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        auto charge = reserve_page();
        if (!charge) {
            static_cast<void>(reply.value().abort());
            return libk::unexpected(charge.error());
        }
        MemoryError commit_error = MemoryError::InvalidState;
        bool committed{};
        bool notify{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr || node->slot.request.key != page_key) {
                commit_error = MemoryError::GenerationExhausted;
            } else if (!node->slot.begin_fill(claim.generation)) {
                commit_error = MemoryError::Busy;
            } else {
                auto supplied = node->slot.supply(
                    claim.generation, content_epoch);
                if (!supplied) {
                    commit_error = MemoryError::InvalidState;
                } else {
                    node->resident = libk::move(page);
                    if (charge.value()) {
                        node->resident_sponsorship.commit(
                            libk::move(charge).value());
                    }
                    if (!node->slot.request.waiters.empty()) {
                        index_ready_locked(*node);
                        notify = true;
                    }
                    committed = true;
                }
            }
        }
        /*luna change: keep the transfer payload for pre-commit abort and make Completing commit terminal, reason: PageTransfer must own rollback until PageSlot and resident ownership commit*/
        if (!committed) {
            static_cast<void>(reply.value().abort());
            return libk::unexpected(commit_error);
        }
        const auto finished = reply.value().commit();
        KASSERT(finished);
        if (notify) {
            owner_->schedule_work();
        }
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
        kernel::pager::Pager& pager,
        usize page_index,
        PageKey page_key,
        kernel::pager::ClaimKey claim) noexcept
        -> libk::Expected<void, MemoryError> {
        if (&pager != pager_) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        if (!page_key || !claim) {
            return libk::unexpected(MemoryError::InvalidRange);
        }
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr || node->slot.request.key != page_key
                || (node->slot.state != PageSlotState::Requested
                    && node->slot.state != PageSlotState::Filling)) {
                return libk::unexpected(MemoryError::OwnershipMismatch);
            }
        }

        auto reply = pager_->begin_reply(claim);
        if (!reply) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        const auto& claimed = reply.value().request();
        if (reply.value().attachment() != &attachment_
            || claimed.kind != kernel::pager::DeliveryKind::PageIn
            || claimed.key != claim.delivery
            || claimed.claim != claim
            || claimed.page_key != page_key) {
            static_cast<void>(reply.value().abort());
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        MemoryError error = MemoryError::OwnershipMismatch;
        bool committed{};
        bool notify{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr || node->slot.request.key != page_key) {
                error = MemoryError::GenerationExhausted;
            } else if (node->slot.fail(claim.generation)) {
                if (!node->slot.request.waiters.empty()) {
                    index_ready_locked(*node);
                    notify = true;
                }
                committed = true;
            }
        }
        if (!committed) {
            static_cast<void>(reply.value().abort());
            return libk::unexpected(error);
        }
        /*luna change: make the Completing reply commit an invariant after page failure, reason: PageSlot::fail is terminal before Pager transport can release its exact ticket*/
        const auto finished = reply.value().commit();
        KASSERT(finished);
        if (notify) {
            owner_->schedule_work();
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
        if (mapping.invalidating()) {
            return libk::unexpected(MemoryError::Busy);
        }
        node->mappings.erase(mapping);
        /*luna change: detach the relation after backing unlink, reason: the
          atomic relation state must outlive the intrusive hook removal*/
        mapping.mark_detached();
        return libk::expected();
    }

    /*luna change: unlink only with the exact claimed token, reason: normal
      teardown must leave an unpublished PageMapping alive*/
    [[nodiscard]] auto unbind_claimed_mapping(
        PageMapping& mapping,
        MemoryWork& work) noexcept
        -> libk::Expected<void, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(mapping.page_index());
        if (node == nullptr || !mapping.backing_hook_.is_linked()) {
            return libk::unexpected(MemoryError::AttachmentState);
        }
        if (!mapping.owns(work)) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        node->mappings.erase(mapping);
        return libk::expected();
    }

    /*luna change: claim one Pager mapping under its owner tree lock, reason:
      exact invalidation must release the backing lock before VSpace work*/
    [[nodiscard]] auto claim_mapping(
        PageMapping& mapping) noexcept
        -> libk::Expected<MemoryWork, MemoryError> {
        kernel::sync::IrqLockGuard guard{tree_lock_};
        Node* const node = find_locked(mapping.page_index());
        if (node == nullptr || !mapping.backing_hook_.is_linked()) {
            return libk::unexpected(MemoryError::AttachmentState);
        }
        auto work = mapping.claim();
        if (!work) {
            return libk::unexpected(MemoryError::Busy);
        }
        return libk::expected(libk::move(work));
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
        /*luna change: fold usage through terminal writeback failure,
          reason: PageSlot remains the sole failure and dirty truth while
          later A/D observations still age the resident candidate*/
        if (node->slot.state != PageSlotState::ResidentClean
            && node->slot.state != PageSlotState::ResidentDirty
            && node->slot.state != PageSlotState::WritebackQueued
            && node->slot.state != PageSlotState::WritebackPublishing
            && node->slot.state != PageSlotState::WritebackPublished
            && node->slot.state != PageSlotState::WritebackActive
            && node->slot.state != PageSlotState::WritebackCompleting
            && node->slot.state != PageSlotState::WritebackFailed) {
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
        -> libk::Expected<WritebackKey, MemoryError> {
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

    [[nodiscard]] auto publish_writeback(
        usize page_index,
        WritebackKey key) noexcept
        -> libk::Expected<void, MemoryError> {
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr) {
                return libk::unexpected(MemoryError::NotBacked);
            }
            if (!node->slot.begin_writeback_publish(key)) {
                return libk::unexpected(MemoryError::OwnershipMismatch);
            }
        }
        const auto published = pager_->publish_writeback(
            attachment_,
            key.page, key.generation, key.dirty_epoch);
        if (!published) {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node != nullptr && node->slot.request.key == key.page
                && node->slot.state == PageSlotState::WritebackPublishing) {
                if (published.error() == pager::Error::Full) {
                    static_cast<void>(node->slot.abort_writeback_publish(key));
                } else {
                    /*luna change: terminalize non-Full writeback transport
                      errors at the exact PageSlot key, reason: a dead Pager
                      must not leave a retryable queued obligation spinning*/
                    auto failed = node->slot.fail_writeback(
                        key, 0, 0,
                        WritebackFailure::BackingUnavailable);
                    KASSERT(failed);
                }
            }
            return libk::unexpected(
                published.error() == pager::Error::Full
                    ? MemoryError::Pending
                    : published.error() == pager::Error::Closed
                        ? MemoryError::BackingFailed
                        : MemoryError::OwnershipMismatch);
        }
        bool committed{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node != nullptr && node->slot.request.key == key.page
                && node->slot.publish_writeback(
                    key, published.value().key.generation)) {
                node->slot.writeback.transport_slot = published.value().key.slot;
                committed = true;
            }
        }
        if (!committed) {
            static_cast<void>(pager_->cancel(published.value().key));
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node != nullptr) {
                static_cast<void>(node->slot.abort_writeback_publish(key));
            }
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        return libk::expected();
    }

    [[nodiscard]] auto writeback(
        kernel::pager::Pager* pager,
        usize page_index,
        WritebackTxn txn) noexcept -> libk::Expected<void, MemoryError> {
        switch (txn.action) {
        case WritebackAction::Publish:
            return publish_writeback(page_index, txn.key);
        case WritebackAction::Complete:
            return pager == nullptr
                ? libk::Expected<void, MemoryError>{
                      libk::unexpected(MemoryError::OwnershipMismatch)}
                : complete_writeback(
                    *pager,
                page_index, txn.key, txn.delivery_generation,
                txn.claim_generation);
        case WritebackAction::Fail:
            return pager == nullptr
                ? libk::Expected<void, MemoryError>{
                      libk::unexpected(MemoryError::OwnershipMismatch)}
                : fail_writeback(
                    *pager, page_index, txn.key, txn.delivery_generation,
                    txn.claim_generation, txn.failure);
        }
        return libk::unexpected(MemoryError::InvalidState);
    }

    [[nodiscard]] auto complete_writeback(
        kernel::pager::Pager& pager,
        usize page_index,
        WritebackKey key,
        u64 delivery_generation,
        u64 claim_generation) noexcept -> libk::Expected<void, MemoryError> {
        if (&pager != pager_) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        u16 transport_slot{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr) {
                return libk::unexpected(MemoryError::NotBacked);
            }
            transport_slot = node->slot.writeback.transport_slot;
        }
        auto reply = pager_->begin_reply(kernel::pager::ClaimKey{
            kernel::pager::RequestKey{
                transport_slot, delivery_generation}, claim_generation});
        if (!reply) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        const auto claim = reply.value().key();
        const auto& claimed = reply.value().request();
        if (reply.value().attachment() != &attachment_
            || claimed.kind != kernel::pager::DeliveryKind::Writeback
            || claimed.key != claim.delivery
            || claimed.claim != claim
            || claimed.page_key != key.page
            || claimed.writeback_generation != key.generation
            || claimed.dirty_epoch != key.dirty_epoch) {
            static_cast<void>(reply.value().abort());
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        bool committed{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node != nullptr) {
                auto begun = node->slot.begin_writeback_complete(
                    key, delivery_generation, claim_generation);
                if (begun) {
                    committed = static_cast<bool>(node->slot.complete_writeback(
                        key, delivery_generation, claim_generation));
                }
            }
        }
        if (!committed) {
            static_cast<void>(reply.value().abort());
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        /*luna change: make writeback completion's Pager commit terminal, reason: the PageSlot writeback state is already committed before the exact Completing ticket is released*/
        const auto finished = reply.value().commit();
        KASSERT(finished);
        /*luna change: wake retained reclaim after writeback completion,
          reason: MemoryExecutor is the existing kernel retry edge after both
          PageSlot and Pager ownership have committed*/
        owner_->schedule_work();
        return libk::expected();
    }

    [[nodiscard]] auto fail_writeback(
        kernel::pager::Pager& pager,
        usize page_index,
        WritebackKey key,
        u64 delivery_generation,
        u64 claim_generation,
        WritebackFailure failure) noexcept -> libk::Expected<void, MemoryError> {
        if (&pager != pager_) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        u16 transport_slot{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node == nullptr) {
                return libk::unexpected(MemoryError::NotBacked);
            }
            transport_slot = node->slot.writeback.transport_slot;
        }
        auto reply = pager_->begin_reply(kernel::pager::ClaimKey{
            kernel::pager::RequestKey{
                transport_slot, delivery_generation}, claim_generation});
        if (!reply) {
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        const auto& claimed = reply.value().request();
        if (reply.value().attachment() != &attachment_
            || claimed.kind != kernel::pager::DeliveryKind::Writeback
            || claimed.page_key != key.page
            || claimed.writeback_generation != key.generation
            || claimed.dirty_epoch != key.dirty_epoch) {
            static_cast<void>(reply.value().abort());
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        bool committed{};
        {
            kernel::sync::IrqLockGuard guard{tree_lock_};
            Node* const node = find_locked(page_index);
            if (node != nullptr
                && node->slot.begin_writeback_complete(
                    key, delivery_generation, claim_generation)) {
                committed = static_cast<bool>(node->slot.fail_writeback(
                    key, delivery_generation, claim_generation, failure));
            }
        }
        if (!committed) {
            static_cast<void>(reply.value().abort());
            return libk::unexpected(MemoryError::OwnershipMismatch);
        }
        /*luna change: make writeback failure's Pager commit terminal, reason: the PageSlot failure state is already committed before the exact Completing ticket is released*/
        const auto finished = reply.value().commit();
        KASSERT(finished);
        /*luna change: wake terminal writeback observation, reason: failure is
          not runnable More but the owner must revisit retained reclaim intent*/
        owner_->schedule_work();
        return libk::expected();
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
            /*luna change: admit direct clean eviction through PageSlot intent,
              reason: begin_evict requires an owned loss obligation even when
              no candidate queue supplied this page*/
            auto retained = node->slot.retain_reclaim();
            KASSERT(retained);
            auto begun = node->slot.begin_evict();
            KASSERT(begun);
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
        KASSERT(attachment_.state
            == kernel::pager::PagerAttachment::State::Detached);
        work_.clear();
        ready_.clear();
        while (nodes_ != nullptr) {
            Node* const node = nodes_;
            nodes_ = node->next;
            reclaim_cursor_ = nullptr;
            KASSERT(node->mappings.empty() && node->leases == 0);
            /*luna change: settle each resident sponsorship before releasing its Node, reason: successful supply commits the charge to the resident owner*/
            auto refund = node->resident_sponsorship.detach();
            node->resident.reset();
            refund.complete();
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
    using WorkList = libk::IntrusiveList<Node, &Node::work_hook_>;
    using ReadyList = libk::IntrusiveList<Node, &Node::ready_hook_>;

    /*luna change: use libk intrusive indexes for retained requests and ready waiters, reason: queue membership must be O(1) and cannot duplicate list links with business flags*/
    void index_work_locked(Node& node) noexcept {
        if (node.work_hook_.is_linked()) {
            return;
        }
        work_.push_back(node);
    }

    void unindex_work_locked(Node& target) noexcept {
        if (!target.work_hook_.is_linked()) {
            return;
        }
        work_.erase(target);
    }

    void index_ready_locked(Node& node) noexcept {
        if (node.ready_hook_.is_linked()) {
            return;
        }
        ready_.push_back(node);
    }

    void unindex_ready_locked(Node& target) noexcept {
        if (!target.ready_hook_.is_linked()) {
            return;
        }
        ready_.erase(target);
    }

    [[nodiscard]] auto find_locked(usize index) noexcept -> Node* {
        for (Node* node = nodes_; node != nullptr; node = node->next) {
            if (node->index == index) {
                return node;
            }
        }
        return nullptr;
    }

    /*luna change: consume and restore one exact reservation at Pager growth,
      reason: PMM pressure precedes PageSlot/PageKey publication*/
    [[nodiscard]] auto claim(FrameDemand* demand) noexcept
        -> libk::Expected<Node*, MemoryError> {
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
                    return libk::expected(node);
                }
                if (growing_) {
                    return libk::unexpected(MemoryError::Pending);
                }
                growing_ = true;
            }
            kernel::resource::Reservation charge{};
            if (demand != nullptr && *demand) {
                auto retained = demand->take();
                KASSERT(retained.has_value());
                charge = libk::move(retained).value();
            } else {
                auto reserved = reserve_page();
                if (!reserved) {
                    kernel::sync::IrqLockGuard guard{storage_lock_};
                    growing_ = false;
                    return libk::unexpected(reserved.error());
                }
                charge = libk::move(reserved).value();
            }
            auto allocated = pmm_->allocate_page();
            if (!allocated) {
                if (demand != nullptr) {
                    demand->emplace(libk::move(charge));
                }
                kernel::sync::IrqLockGuard guard{storage_lock_};
                growing_ = false;
                return libk::unexpected(MemoryError::Pressure);
            }
            OwnedPage backing = libk::move(allocated).value();
            auto* const page = libk::construct_at(
                reinterpret_cast<PageHeader*>(backing.bytes()),
                libk::move(backing), libk::move(charge));
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
        if (reclaim_cursor_ == &target) {
            reclaim_cursor_ = target.next;
        }
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

    MemoryObject* owner_{};
    Pmm* pmm_{};
    kernel::pager::Pager* pager_{};
    AccessMask access_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::BackingTree>
        tree_lock_{};
    kernel::sync::SpinLock<kernel::sync::LockClass::BackingStorage>
        storage_lock_{};
    Node* nodes_{};
    /*luna change: retain one owner cursor for bounded candidate inspection,
      reason: policy traversal must not create a per-page work queue*/
    Node* reclaim_cursor_{};
    WorkList work_{};
    ReadyList ready_{};
    PageHeader* pages_{};
    bool growing_{};
    kernel::resource::Sponsorship* sponsor_{};
    kernel::pager::PagerAttachment attachment_{};
};

namespace {

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

/*luna change: move only the release token pair, reason: publication belongs
  to each relation owner and must not become MemoryWork state*/
MemoryWork::MemoryWork(MemoryWork&& other) noexcept
    : context_(libk::exchange(other.context_, nullptr)),
      release_(libk::exchange(other.release_, nullptr)) {}

auto MemoryWork::operator=(MemoryWork&& other) noexcept -> MemoryWork& {
    if (this != &other) {
        reset();
        context_ = libk::exchange(other.context_, nullptr);
        release_ = libk::exchange(other.release_, nullptr);
    }
    return *this;
}

MemoryWork::~MemoryWork() noexcept {
    reset();
}

void MemoryWork::reset() noexcept {
    void* const context = libk::exchange(context_, nullptr);
    Release const release = libk::exchange(release_, nullptr);
    if (context != nullptr) {
        KASSERT(release != nullptr);
        release(context);
    }
}

MemoryWork::MemoryWork(MemoryAttachment& attachment) noexcept
    : context_(&attachment),
      release_([](void* context) noexcept {
          static_cast<MemoryAttachment*>(context)->drop_work();
      }) {}

void PageMapping::arm(void* context, Publish publish) noexcept {
    KASSERT(static_cast<State>(state_.load<libk::MemoryOrder::Acquire>())
        == State::Detached);
    KASSERT(context != nullptr && publish != nullptr);
    context_ = context;
    publish_ = publish;
}

/*luna change: validate the token's exact relation callback and context,
  reason: a non-empty MemoryWork cannot prove PageMapping ownership*/
auto PageMapping::owns(const MemoryWork& work) const noexcept -> bool {
    return static_cast<State>(state_.load<libk::MemoryOrder::Acquire>())
            == State::Invalidating
        && work.context_ == this
        && work.release_ == &PageMapping::release_work;
}

/*luna change: transition the exact relation once before foreign publication,
  reason: backing membership and VSpace detach share one winner state*/
auto PageMapping::claim() noexcept -> MemoryWork {
    u8 attached = static_cast<u8>(State::Attached);
    if (!state_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(
            attached,
            static_cast<u8>(State::Invalidating))) {
        return {};
    }
    return MemoryWork{this, &PageMapping::release_work};
}

/*luna change: publish the claimed relation through PageMapping-owned context,
  reason: the release token remains independent of publication protocol*/
void PageMapping::publish(MemoryWork&& work) noexcept {
    KASSERT(context_ != nullptr && publish_ != nullptr && owns(work));
    publish_(context_, libk::move(work));
}

void PageMapping::mark_attached() noexcept {
    KASSERT(owner_ != nullptr);
    state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(State::Attached));
}

void PageMapping::mark_detached() noexcept {
    const State current = static_cast<State>(
        state_.load<libk::MemoryOrder::Acquire>());
    if (current == State::Attached) {
        state_.store<libk::MemoryOrder::Release>(
            static_cast<u8>(State::Detached));
    }
}

void PageMapping::release_work(void* context) noexcept {
    static_cast<PageMapping*>(context)->finish_work();
}

void PageMapping::finish_work() noexcept {
    KASSERT(static_cast<State>(state_.load<libk::MemoryOrder::Acquire>())
        == State::Invalidating);
    KASSERT(owner_ == nullptr && !backing_hook_.is_linked());
    state_.store<libk::MemoryOrder::Release>(
        static_cast<u8>(State::Detached));
}

PageMapping::~PageMapping() noexcept {
    KASSERT(owner_ == nullptr && !backing_hook_.is_linked());
    KASSERT(static_cast<State>(state_.load<libk::MemoryOrder::Acquire>())
        == State::Detached);
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

MemoryObject::MemoryObject(
    Pmm& pmm,
    usize byte_size,
    MemoryExecutor& work,
    PageReclaimer& reclaimer) noexcept
    : pmm_(&pmm), work_(&work), reclaimer_(reclaimer) {
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
    KASSERT(!reclaim_entry_.hook.is_linked() && reclaim_entry_.object == nullptr);
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
        return libk::unexpected(MemoryError::Pressure);
    }
    OwnedPage storage = libk::move(allocated).value();

    /*luna change: route the optional demand through backing adapters,
      reason: only relation-aware callers may retain Pressure pins*/
    static const BackingOps anonymous_ops{
        .kind = BackingKind::Anonymous,
        .query = [](const void* backing, usize index) noexcept {
            return static_cast<const AnonymousBacking*>(backing)->query(index);
        },
        .materialize = [](void* backing, usize index, FrameDemand* demand, WaitRelation*, void*,
                          WaitRelation::Publish) noexcept {
            return static_cast<AnonymousBacking*>(backing)->materialize(index, demand);
        },
        .cancel_fault = nullptr,
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
        .unbind_claimed_mapping = nullptr,
        .claim_mapping = nullptr,
        .lease_acquire = nullptr,
        .lease_release = nullptr,
        .page_state = nullptr,
        .observe_usage = nullptr,
        .drain_page_waiters = nullptr,
        .service = nullptr,
        .reclaim = nullptr,
        .queue_writeback = nullptr,
        .writeback = nullptr,
        .evict_page = nullptr,
        .destroy = [](void* backing) noexcept {
            libk::destroy_at(static_cast<AnonymousBacking*>(backing));
        },
        .stop = nullptr,
    };
    static const BackingOps physical_ops{
        .kind = BackingKind::Physical,
        .query = [](const void* backing, usize index) noexcept {
            return static_cast<const ExtentBacking*>(backing)->query(index);
        },
        .materialize = [](void* backing, usize index, FrameDemand*, WaitRelation*, void*,
                          WaitRelation::Publish) noexcept {
            return static_cast<ExtentBacking*>(backing)->materialize(index);
        },
        .cancel_fault = nullptr,
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
        .unbind_claimed_mapping = nullptr,
        .claim_mapping = nullptr,
        .lease_acquire = nullptr,
        .lease_release = nullptr,
        .page_state = nullptr,
        .observe_usage = nullptr,
        .drain_page_waiters = nullptr,
        .service = nullptr,
        .reclaim = nullptr,
        .queue_writeback = nullptr,
        .writeback = nullptr,
        .evict_page = nullptr,
        .destroy = [](void* backing) noexcept {
            libk::destroy_at(static_cast<ExtentBacking*>(backing));
        },
        .stop = nullptr,
    };
    static const BackingOps boot_ops{
        .kind = BackingKind::BootImage,
        .query = [](const void* backing, usize index) noexcept {
            return static_cast<const BootBacking*>(backing)->query(index);
        },
        .materialize = [](void* backing, usize index, FrameDemand*, WaitRelation*, void*,
                          WaitRelation::Publish) noexcept {
            return static_cast<BootBacking*>(backing)->materialize(index);
        },
        .cancel_fault = nullptr,
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
        .unbind_claimed_mapping = nullptr,
        .claim_mapping = nullptr,
        .lease_acquire = nullptr,
        .lease_release = nullptr,
        .page_state = nullptr,
        .observe_usage = nullptr,
        .drain_page_waiters = nullptr,
        .service = nullptr,
        .reclaim = nullptr,
        .queue_writeback = nullptr,
        .writeback = nullptr,
        .evict_page = nullptr,
        .destroy = [](void* backing) noexcept {
            libk::destroy_at(static_cast<BootBacking*>(backing));
        },
        .stop = nullptr,
    };
    static const BackingOps pager_ops{
        .kind = BackingKind::Pager,
        .query = [](const void* backing, usize index) noexcept {
            return static_cast<const PagerBacking*>(backing)->query(index);
        },
        .materialize = [](void* backing, usize index, FrameDemand* demand,
                          WaitRelation* relation,
                          void* owner,
                          WaitRelation::Publish publish) noexcept {
            return static_cast<PagerBacking*>(backing)->materialize(
                index, demand, relation, owner, publish);
        },
        .cancel_fault = [](void* backing, WaitRelation& relation,
                           u64 generation) noexcept {
            return static_cast<PagerBacking*>(backing)->cancel_fault(
                relation, generation);
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
        .supply = [](void* backing, kernel::pager::Pager& pager,
                     usize index, PageKey page_key,
                     kernel::pager::ClaimKey claim, OwnedPage&& page,
                     u64 content_epoch) noexcept {
            return static_cast<PagerBacking*>(backing)->supply(
                pager, index, page_key, claim,
                libk::move(page), content_epoch);
        },
        .fail = [](void* backing, kernel::pager::Pager& pager,
                   usize index, PageKey page_key,
                   kernel::pager::ClaimKey claim) noexcept {
            return static_cast<PagerBacking*>(backing)->fail(
                pager, index, page_key, claim);
        },
        .bind_mapping = [](void* backing, usize index,
                           PageMapping& mapping) noexcept {
            return static_cast<PagerBacking*>(backing)->bind_mapping(
                index, mapping);
        },
        .unbind_mapping = [](void* backing, PageMapping& mapping) noexcept {
            return static_cast<PagerBacking*>(backing)->unbind_mapping(mapping);
        },
        .unbind_claimed_mapping = [](
            void* backing,
            PageMapping& mapping,
            MemoryWork& work) noexcept {
            return static_cast<PagerBacking*>(backing)->unbind_claimed_mapping(
                mapping, work);
        },
        .claim_mapping = [](void* backing, PageMapping& mapping) noexcept {
            return static_cast<PagerBacking*>(backing)->claim_mapping(mapping);
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
        .drain_page_waiters = [](void* backing, usize capacity) noexcept {
            return static_cast<PagerBacking*>(backing)->drain_page_waiters(
                capacity);
        },
        .service = [](void* backing, usize capacity) noexcept {
            return static_cast<PagerBacking*>(backing)->service(capacity);
        },
        .reclaim = [](void* backing, usize capacity) noexcept {
            return static_cast<PagerBacking*>(backing)->reclaim(capacity);
        },
        .queue_writeback = [](void* backing, usize index) noexcept {
            return static_cast<PagerBacking*>(backing)->queue_writeback(index);
        },
        .writeback = [](void* backing, kernel::pager::Pager* pager,
                        usize index,
                        WritebackTxn txn) noexcept {
            return static_cast<PagerBacking*>(backing)->writeback(
                pager, index, txn);
        },
        .evict_page = [](void* backing, usize index) noexcept {
            return static_cast<PagerBacking*>(backing)->evict_page(index);
        },
        .destroy = [](void* backing) noexcept {
            libk::destroy_at(static_cast<PagerBacking*>(backing));
        },
        .stop = [](void* backing) noexcept {
            static_cast<PagerBacking*>(backing)->stop();
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
        KASSERT(operations_ != libk::numeric_limits<usize>::max());
        ++operations_;
        auto* const backing = libk::construct_at(
            static_cast<PagerBacking*>(backend),
            *this, *pmm_, *pager, pager_access, sponsor_);
        ops = &pager_ops;
        backend = backing;
        initialized = backing->init();
        break;
    }
    }

    if (!initialized) {
        if (kind == BackingKind::Pager) {
            KASSERT(operations_ != 0);
            --operations_;
        }
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
    if (kind == BackingKind::Pager) {
        /*luna change: publish Pager membership only after Live state, reason:
          the derived index must never admit a building object*/
        KASSERT(reclaimer_.register_memory(*this));
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
    return materialize_impl(page_index, nullptr, nullptr, nullptr, nullptr);
}

auto MemoryObject::materialize(
    usize page_index,
    WaitRelation* relation,
    void* owner,
    WaitRelation::Publish publish,
    FrameDemand* demand) noexcept
    -> libk::Expected<PageLease, MemoryError> {
    return materialize_impl(page_index, demand, relation, owner, publish);
}

/*luna change: admit pressure before returning a durable fault, reason:
  MemoryObject owns the operation pin, demand and PageReclaimer handoff*/
auto MemoryObject::materialize_impl(
    usize page_index,
    FrameDemand* demand,
    WaitRelation* relation,
    void* owner,
    WaitRelation::Publish publish) noexcept
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

    /*luna change: sample frame progress before backing allocation,
      reason: a frame returned after Pressure must remain observable to the relation*/
    const u64 progress = pmm_->frame_progress_generation();
    auto result = ops->materialize(
        backing, page_index, demand, relation, owner, publish);
    const bool pressure = relation != nullptr && demand != nullptr
        && !result && result.error() == MemoryError::Pressure && *demand;
    if (pressure && !reclaimer_.retain(
            *relation,
            progress,
            owner,
            publish)) {
        /*luna change: settle an unadmitted pressure pin at its owner,
          reason: a Pressure result without an attached relation is
          non-durable and cannot reach a continuation*/
        demand->reset();
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(operations_ != 0);
            --operations_;
        }
        finish_retire();
        return libk::unexpected(MemoryError::Busy);
    }
    const bool retained = relation != nullptr && !result
        && (result.error() == MemoryError::Pending || pressure);
    if (!retained && demand != nullptr && *demand) {
        demand->reset();
    }
    bool live{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        live = state_ == MemoryState::Live;
        if ((!result && !retained) || (live == false && !retained)) {
            KASSERT(operations_ != 0);
            --operations_;
        }
    }
    /*luna change: preserve the pressure class across a retained pin, reason:
      PageReclaimer admission is distinct from PageRequest transport*/
    if (retained) {
        return libk::unexpected(
            pressure ? MemoryError::Pressure : MemoryError::Pending);
    }
    if (!result || !live) {
        finish_retire();
        if (!live) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        return libk::unexpected(result.error());
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
    pager::Pager& pager,
    usize page_index,
    PageKey page_key,
    pager::ClaimKey claim,
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
        backing, pager, page_index, page_key, claim,
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
    pager::Pager& pager,
    PageTransfer&& transfer,
    usize page_index,
    PageKey page_key,
    pager::ClaimKey claim,
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
        backing, pager, page_index, page_key, claim,
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
    pager::Pager& pager,
    usize page_index,
    PageKey page_key,
    pager::ClaimKey claim) noexcept
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
        backing, pager, page_index, page_key, claim);
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
            /*luna change: publish attachment state after backing admission,
              reason: PageMapping has one exact attached winner*/
            mapping.mark_attached();
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
        /*luna change: complete normal unbind through the same relation state,
          reason: reclaim and teardown cannot retain parallel attachment truth*/
        mapping.mark_detached();
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0);
        --operations_;
    }
    finish_retire();
    return unlinked;
}

/*luna change: complete claimed mapping unlink only with matching token,
  reason: PageMapping storage stays live until VSpace finishes its PTE*/
auto MemoryObject::unbind_mapping(
    PageMapping& mapping,
    MemoryWork& work) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (mapping.owner_ != this || backing_ops_ == nullptr
            || backing_ops_->unbind_claimed_mapping == nullptr) {
            return libk::unexpected(MemoryError::AttachmentState);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto unlinked = ops->unbind_claimed_mapping(backing, mapping, work);
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

/*luna change: claim one reverse mapping through the existing operation pin,
  reason: page invalidation must cross the backing lock without extending it*/
auto MemoryObject::claim_mapping(PageMapping& mapping) noexcept
    -> libk::Expected<MemoryWork, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        if (mapping.owner_ != this || backing_ops_ == nullptr
            || backing_ops_->claim_mapping == nullptr) {
            return libk::unexpected(MemoryError::AttachmentState);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->claim_mapping(backing, mapping);
    drop_page();
    return result;
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

/*luna change: retain a stopping-safe exact mapping operation pin, reason:
  VSpace teardown must fold A/D state before the relation is unbound*/
auto MemoryObject::observe_usage(
    PageMapping& mapping,
    bool accessed,
    bool dirty) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    usize page_index{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if ((state_ != MemoryState::Live && state_ != MemoryState::Stopping)
            || backing_ops_ == nullptr || backing_ == nullptr) {
            return libk::unexpected(MemoryError::InvalidState);
        }
        if (mapping.owner_ != this || mapping.page_index_ >= logical_pages_) {
            return libk::unexpected(MemoryError::AttachmentState);
        }
        KASSERT(operations_ != libk::numeric_limits<usize>::max());
        ++operations_;
        page_index = mapping.page_index_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->observe_usage != nullptr
        ? ops->observe_usage(backing, page_index, accessed, dirty)
        : libk::Expected<void, MemoryError>{libk::expected()};
    drop_page();
    return result;
}

auto MemoryObject::drain_page_waiters(
    usize capacity) noexcept -> usize {
    if (capacity == 0) {
        return 0;
    }
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || backing_ops_ == nullptr
            || backing_ops_->drain_page_waiters == nullptr) {
            return 0;
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    const usize result = ops->drain_page_waiters(backing, capacity);
    drop_page();
    return result;
}

/*luna change: reserve and drop a pin only for fresh admission, reason: an already-active runner owns settle and a racing kick must not release it*/
void MemoryObject::schedule_work() noexcept {
    KASSERT(work_ != nullptr);
    bool fresh{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!work_open_.load<libk::MemoryOrder::Acquire>()
            || state_ != MemoryState::Live) {
            return;
        }
        if (!work_active_.load<libk::MemoryOrder::Acquire>()) {
            KASSERT(operations_ != libk::numeric_limits<usize>::max());
            ++operations_;
            work_active_.store<libk::MemoryOrder::Release>(true);
            fresh = true;
        }
    }
    ensure_work_observation();
    auto observation = diag::concurrency::ObservationLease::borrow(
        diag::concurrency::ObservationKey{
            observation_key_.load<libk::MemoryOrder::Acquire>()});
    observation.watch(true);
    observation.touch();
    if (!work_->submit(*this) && fresh) {
        finish_work();
    }
}

/*luna change: settle the single work pin after executor handoff, reason: MemoryExecutor owns queue rearm while MemoryObject only releases operations lifetime*/
void MemoryObject::finish_work() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!work_active_.load<libk::MemoryOrder::Acquire>()) {
            return;
        }
        work_active_.store<libk::MemoryOrder::Release>(false);
        KASSERT(operations_ != 0);
        --operations_;
    }
    finish_retire();
}

/*luna change: dispatch bounded backing service through the canonical object, reason: MemoryExecutor transports work while PageSlot/PageRequest own semantic state*/
auto MemoryObject::service(usize capacity) noexcept -> MemoryServiceBatch {
    if (capacity == 0) {
        return {};
    }
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!work_active_.load<libk::MemoryOrder::Acquire>()
            || state_ != MemoryState::Live
            || backing_ops_ == nullptr || backing_ops_->service == nullptr) {
            return {};
        }
        ops = backing_ops_;
        backing = backing_;
    }
    return ops->service(backing, capacity);
}

/*luna change: project MemoryExecutor progress through the existing observation lease, reason: diagnostics must remain a one-way view of canonical work state*/
void MemoryObject::ensure_work_observation() noexcept {
    bool expected = false;
    if (!observation_reserved_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, true)) {
        return;
    }
    observation_key_.store<libk::MemoryOrder::Relaxed>(0);
    auto observation = diag::concurrency::ObservationLease::reserve(
        diag::concurrency::RecordKind::MemoryWork,
        reinterpret_cast<u64>(this),
        1,
        diag::concurrency::Expectation::InternalFinite);
    if (!observation) {
        observation_reserved_.store<libk::MemoryOrder::Release>(false);
        return;
    }
    observation.watch(true);
    observation_key_.store<libk::MemoryOrder::Release>(
        observation.detach_key().raw);
}

void MemoryObject::publish_work_observation(
    const MemoryServiceBatch& batch) noexcept {
    const auto key = diag::concurrency::ObservationKey{
        observation_key_.load<libk::MemoryOrder::Acquire>()};
    auto observation = diag::concurrency::ObservationLease::borrow(key);
    if (!observation) {
        return;
    }
    MemoryState state{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        state = state_;
    }
    u64 stamp = static_cast<u64>(state);
    stamp = stamp * 17 + batch.processed;
    stamp = stamp * 17 + batch.progressed;
    stamp = stamp * 17 + batch.more;
    diag::concurrency::ObservationBatch update{
        .phase = static_cast<u32>(batch.more
            ? diag::concurrency::ServicePhase::Running
            : diag::concurrency::ServicePhase::Completed),
        .semantic_stamp = stamp,
        .wait = batch.more
            ? diag::concurrency::WaitKind::MemoryWork
            : diag::concurrency::WaitKind::None,
        .driver = diag::concurrency::NodeRef::external(
            reinterpret_cast<u64>(this)),
        .blocker = {},
        .site = diag::concurrency::SourceSite::current(),
        .detail_mask = 0xfU,
        .update_progress = batch.progressed != 0,
        .update_watched = true,
        .watched = batch.more};
    update.detail[0] = batch.processed;
    update.detail[1] = batch.progressed;
    update.detail[2] = batch.more;
    update.detail[3] = static_cast<u64>(state);
    observation.publish(update);
}

auto MemoryObject::queue_writeback(usize page_index) noexcept
    -> libk::Expected<WritebackKey, MemoryError> {
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

auto MemoryObject::publish_writeback(
    usize page_index,
    WritebackKey key) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || backing_ops_ == nullptr
            || backing_ops_->writeback == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->writeback(
        backing, nullptr, page_index, WritebackTxn{
            .action = WritebackAction::Publish,
            .key = key});
    drop_page();
    return result;
}

auto MemoryObject::complete_writeback(
    pager::Pager& pager,
    usize page_index,
    WritebackKey key,
    u64 delivery_generation,
    u64 claim_generation) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || backing_ops_ == nullptr
            || backing_ops_->writeback == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->writeback(
        backing, &pager, page_index, WritebackTxn{
            .action = WritebackAction::Complete,
            .key = key,
            .delivery_generation = delivery_generation,
            .claim_generation = claim_generation});
    drop_page();
    return result;
}

auto MemoryObject::fail_writeback(
    pager::Pager& pager,
    usize page_index,
    WritebackKey key,
    u64 delivery_generation,
    u64 claim_generation,
    WritebackFailure failure) noexcept -> libk::Expected<void, MemoryError> {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || page_index >= logical_pages_
            || backing_ops_ == nullptr
            || backing_ops_->writeback == nullptr) {
            return libk::unexpected(MemoryError::NotBacked);
        }
        ++operations_;
        ops = backing_ops_;
        backing = backing_;
    }
    auto result = ops->writeback(
        backing, &pager, page_index, WritebackTxn{
            .action = WritebackAction::Fail,
            .key = key,
            .delivery_generation = delivery_generation,
            .claim_generation = claim_generation,
            .failure = failure});
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

/*luna change: close work admission before backing stop and withdraw queued work, reason: retire must preserve one operations pin for any in-flight service without publishing new work*/
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
        work_open_.store<libk::MemoryOrder::Release>(false);
    }

    static_cast<void>(reclaimer_.withdraw(*this));

    if (work_->withdraw(*this)) {
        finish_work();
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
    void* stopped_backing{};
    const BackingOps* stopped_ops{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        stopped_backing = backing_;
        stopped_ops = backing_ops_;
    }
    if (stopped_ops != nullptr && stopped_ops->stop != nullptr) {
        stopped_ops->stop(stopped_backing);
    }
    finish_retire();
}

/*luna change: take a bounded reclaim operation pin through the MemoryObject
  owner, reason: the derived index must not call backing code after retire*/
auto MemoryObject::try_reclaim_pin() noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (!reclaim_entry_.hook.is_linked() || state_ != MemoryState::Live
        || backing_ops_ == nullptr) {
        return false;
    }
    KASSERT(operations_ != libk::numeric_limits<usize>::max());
    ++operations_;
    return true;
}

void MemoryObject::finish_reclaim_pin() noexcept {
    drop_page();
}

/*luna change: run one pinned backing candidate pass, reason: PageReclaimer
  holds the existing operations pin across the lock-free foreign call*/
auto MemoryObject::reclaim(usize capacity) noexcept -> ReclaimResult {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Live || backing_ops_ == nullptr
            || backing_ops_->reclaim == nullptr) {
            return ReclaimResult::Idle;
        }
        ops = backing_ops_;
        backing = backing_;
    }
    return ops->reclaim(backing, capacity);
}

void MemoryObject::drop_page() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(operations_ != 0);
        --operations_;
    }
    finish_retire();
}

/*luna change: settle the retained fault pin through the MemoryObject owner, reason: one operation count must cover foreign Pager detach and terminal release*/
void MemoryObject::release_fault() noexcept {
    drop_page();
}

/*luna change: release only the exact pressure relation, reason: its caller
  settles the separate MemoryObject operation pin exactly once*/
auto MemoryObject::release_pressure(
    WaitRelation& relation,
    u64 generation) noexcept -> bool {
    return reclaimer_.release(relation, generation);
}

auto MemoryObject::cancel_fault(
    WaitRelation& relation,
    u64 generation) noexcept -> bool {
    const BackingOps* ops{};
    void* backing{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (backing_ops_ == nullptr || backing_ == nullptr
            || state_ == MemoryState::Retired
            || backing_ops_->cancel_fault == nullptr) {
            return false;
        }
        ops = backing_ops_;
        backing = backing_;
    }
    if (!ops->cancel_fault(backing, relation, generation)) {
        return false;
    }
    drop_page();
    return true;
}

void MemoryObject::release_backing_hold() noexcept {
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

/*luna change: keep work observations through idle and finish only at object retirement, reason: diagnostics must span service cycles without steering executor control*/
void MemoryObject::finish_retire() noexcept {
    void* backing{};
    const BackingOps* ops{};
    OwnedPage storage{};
    u64 terminal_key{};
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
        terminal_key = observation_key_.exchange<
            libk::MemoryOrder::AcqRel>(0);
        observation_reserved_.store<libk::MemoryOrder::Release>(false);
    }
    if (terminal_key != 0) {
        auto observation = diag::concurrency::ObservationLease::borrow(
            diag::concurrency::ObservationKey{terminal_key});
        observation.finish(static_cast<u32>(
            diag::concurrency::ServicePhase::Completed));
    }
}

/*luna change: seal failed construction against executor admission, reason: a non-live object has no backing service to drain*/
void MemoryObject::fail_build() noexcept {
    KASSERT(state_ == MemoryState::Building);
    KASSERT(backing_ == nullptr && backing_ops_ == nullptr);
    work_open_.store<libk::MemoryOrder::Release>(false);
    state_ = MemoryState::Retired;
}

} // namespace kernel::mm
