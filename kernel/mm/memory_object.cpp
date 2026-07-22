#include <mm/memory_object.hpp>

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
    page_ = {};
    if (owner != nullptr) {
        owner->drop_page();
    }
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
        libk::move(owned));
}

auto MemoryObject::initialize_backing(
    BackingKind kind,
    libk::Span<const MemoryExtent> extents,
    AnonymousConfig anonymous,
    BootOwnership boot_ownership,
    OwnedPageGroup&& boot_pages) noexcept
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
        .destroy = [](void* backing) noexcept {
            libk::destroy_at(static_cast<BootBacking*>(backing));
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
    if (kind == BackingKind::Anonymous) {
        access_ = anonymous.access;
    } else {
        u8 access_bits{};
        for (const MemoryExtent& extent : extents) {
            access_bits |= extent.access.raw();
        }
        access_ = AccessMask::from_raw(access_bits);
    }
    if (kind != BackingKind::Anonymous) {
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
    return libk::expected(PageLease{*this, result.value()});
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

void MemoryObject::finish_retire() noexcept {
    void* backing{};
    const BackingOps* ops{};
    OwnedPage storage{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != MemoryState::Stopping
            || operations_ != 0
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
