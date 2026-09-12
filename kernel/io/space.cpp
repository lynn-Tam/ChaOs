#include <io/space.hpp>

#include <object/device_pool.hpp>
#include <object/io_space_pool.hpp>
#include <object/memory_pool.hpp>
#include <object/object_store.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::io {
namespace {
constexpr auto DmaAccess = mm::AccessMask::of(mm::Access::Read, mm::Access::Write);
}

Space::Space(mm::Pmm& pmm, Executor& executor, object::ObjectStore& objects,
    cap::GrantGraph& grants) noexcept
    : pmm_(pmm), executor_(executor), objects_(objects), grants_(grants) {}

Space::~Space() noexcept {
    KASSERT(state_ == SpaceState::Empty || state_ == SpaceState::Closed);
    KASSERT(!self_ && !cleanup_ && !lease_ && !pins_ && !work_open_);
}

auto Space::state() const noexcept -> SpaceState {
    sync::IrqLockGuard guard{lock_};
    return state_;
}

auto Space::bar(usize index) noexcept -> libk::Expected<cap::GrantRef, SpaceError> {
    sync::IrqLockGuard guard{lock_};
    if (state_ != SpaceState::Active || closing_.load<libk::MemoryOrder::Acquire>())
        return libk::unexpected(SpaceError::InvalidState);
    if (index >= bars_.size() || !bars_[index].grant)
        return libk::unexpected(SpaceError::InvalidRange);
    auto grant = bars_[index].grant.clone();
    if (!grant) return libk::unexpected(SpaceError::InvalidAuthority);
    return libk::expected(libk::move(grant).value());
}

auto Space::interrupt() noexcept -> libk::Expected<cap::GrantRef, SpaceError> {
    sync::IrqLockGuard guard{lock_};
    if (state_ != SpaceState::Active || closing_.load<libk::MemoryOrder::Acquire>())
        return libk::unexpected(SpaceError::InvalidState);
    auto grant = interrupt_grant_.clone();
    if (!grant) return libk::unexpected(SpaceError::InvalidAuthority);
    return libk::expected(libk::move(grant).value());
}

auto Space::info() const noexcept -> libk::Expected<DeviceInfo, SpaceError> {
    sync::IrqLockGuard guard{lock_};
    if (state_ != SpaceState::Active || closing_.load<libk::MemoryOrder::Acquire>())
        return libk::unexpected(SpaceError::InvalidState);
    DeviceInfo info{.configuration = lease_->configuration()};
    for (usize index = 0; index < info.bar_sizes.size(); ++index)
        info.bar_sizes[index] = lease_->bars()[index].size;
    return libk::expected(info);
}

auto Space::bind(object::ObjectRef self, cap::Resolved<Device>& device,
    cap::Resolved<mm::MemoryObject>& memory, mm::ObjectRange range,
    usize first) noexcept -> libk::Expected<void, SpaceError> {
    auto pin = self.pin<Space>();
    if (!pin || &pin.value().get() != this)
        return libk::unexpected(SpaceError::InvalidAuthority);
    const auto effective = memory.authority();
    const auto* authority = libk::get_if<cap::MemoryAuthority>(&effective.data);
    if (!device.rights().contains(cap::Right::Connect)
        || !memory.rights().contains(cap::Right::Map) || authority == nullptr
        || !authority->range.contains(range) || !authority->access.contains(DmaAccess)
        || !authority->types.contains(mm::MemoryType::Normal))
        return libk::unexpected(SpaceError::InvalidAuthority);
    const auto tables = arch::IoRoot::required_pages(first, range.page_count);
    if (!tables || !range.within(memory->page_count()))
        return libk::unexpected(SpaceError::InvalidRange);
    if (memory->kind() != mm::BackingKind::Anonymous)
        return libk::unexpected(SpaceError::UnsupportedMemory);
    {
        sync::IrqLockGuard guard{lock_};
        if (state_ != SpaceState::Empty)
            return libk::unexpected(SpaceError::InvalidState);
        self_ = libk::move(self);
        state_ = SpaceState::Binding;
        executor_.open(*this);
    }
    // Binding owns these fields while allocation and page materialization run
    // with interrupts enabled. Invalidation can only request cancellation and
    // deposit its exact token; service leaves Binding to this invocation.
    libk::optional<arch::IoRoot> prepared{};
    libk::optional<SpaceError> error{};
    {
        auto root = prepare(device, memory, range, first, tables.value());
        if (root) prepared.emplace(libk::move(root).value());
        else error = root.error();
    }
    {
        sync::IrqLockGuard guard{lock_};
        if (prepared && !closing_.load<libk::MemoryOrder::Acquire>()) {
            lease_->open(libk::move(*prepared));
            state_ = SpaceState::Opening;
            executor_.submit(*this);
            return libk::expected();
        }
    }
    // Unpublished tables disappear before service can refund their charge.
    // Binding keeps the executor out while this potentially large tree frees.
    prepared.reset();
    {
        sync::IrqLockGuard guard{lock_};
        state_ = SpaceState::Closing;
        closing_.store<libk::MemoryOrder::Release>(true);
        executor_.submit(*this);
    }
    return libk::unexpected(error ? *error : SpaceError::Cancelled);
}

auto Space::prepare(cap::Resolved<Device>& device,
    cap::Resolved<mm::MemoryObject>& memory, mm::ObjectRange range,
    usize first, usize tables) noexcept -> libk::Expected<arch::IoRoot, SpaceError> {
    auto device_ref = device.reference();
    auto memory_ref = memory.reference();
    if (!device_ref || !memory_ref)
        return libk::unexpected(SpaceError::InvalidAuthority);
    auto device_hold = libk::move(device_ref).value().into_hold<Device>();
    auto memory_hold = libk::move(memory_ref).value().into_hold<mm::MemoryObject>();
    if (!device_hold || !memory_hold)
        return libk::unexpected(SpaceError::InvalidAuthority);
    device_ = libk::move(device_hold).value();
    memory_ = libk::move(memory_hold).value();
    if (!device.attach(device_grant_) || !memory.attach(memory_grant_)
        || !memory_->attach(memory_attachment_, DmaAccess))
        return libk::unexpected(SpaceError::InvalidAuthority);
    auto lease = device_->acquire(this, stop_device);
    if (!lease) return libk::unexpected(SpaceError::Busy);
    lease_.emplace(libk::move(*lease));
    auto bars = prepare_bars();
    if (!bars) return libk::unexpected(bars.error());
    auto interrupt = prepare_interrupt();
    if (!interrupt) return libk::unexpected(interrupt.error());
    const usize metadata = (range.page_count - 1) / Pins::Capacity + 1;
    if (sponsor_ != nullptr) {
        auto charge = sponsor_->acquire({.memory = (metadata + tables) * mm::page_size});
        if (!charge) return libk::unexpected(SpaceError::QuotaExceeded);
        charge_ = libk::move(charge).value();
    }
    metadata_ = pmm_.make_page_group();
    Pins** tail = &pins_;
    for (usize offset = 0; offset < range.page_count;) {
        if (closing_.load<libk::MemoryOrder::Acquire>())
            return libk::unexpected(SpaceError::Cancelled);
        auto extension = metadata_.extend();
        auto page = extension.allocate_page();
        if (!page) return libk::unexpected(SpaceError::OutOfMemory);
        auto* block = libk::construct_at(reinterpret_cast<Pins*>(extension.bytes(page.value())));
        extension.commit();
        *tail = block;
        tail = &block->next;
        for (; block->count < Pins::Capacity && offset < range.page_count; ++offset) {
            auto source = memory_->materialize(range.first + offset);
            if (!source) {
                const auto error = source.error();
                return libk::unexpected(error == mm::MemoryError::OutOfMemory
                    ? SpaceError::OutOfMemory
                    : error == mm::MemoryError::ResourceExhausted
                        ? SpaceError::QuotaExceeded : SpaceError::BackingUnavailable);
            }
            if (source.value().page().type != mm::MemoryType::Normal
                || !source.value().page().access.contains(DmaAccess))
                return libk::unexpected(SpaceError::UnsupportedMemory);
            block->pages[block->count++] = libk::move(source).value();
        }
    }
    Pins* block = pins_;
    usize index{};
    auto next = [&]() noexcept {
        KASSERT(block != nullptr && index < block->count);
        const auto page = block->pages[index++].page().page;
        if (index == block->count) { block = block->next; index = 0; }
        return page;
    };
    auto root = arch::IoRoot::create(pmm_, first, range.page_count,
        arch::IoRoot::PageSource::bind(next), true);
    if (!root) return libk::unexpected(SpaceError::OutOfMemory);
    KASSERT(root.value().page_count() == tables);
    return libk::expected(libk::move(root).value());
}

auto Space::prepare_bars() noexcept -> libk::Expected<void, SpaceError> {
    for (usize index = 0; index < bars_.size(); ++index) {
        const auto& physical = lease_->bars()[index];
        if (physical.size == 0) continue;
        if (closing_.load<libk::MemoryOrder::Acquire>())
            return libk::unexpected(SpaceError::Cancelled);
        const usize bytes = (physical.size + mm::page_size - 1) & ~(mm::page_size - 1);
        const auto range = mm::PageRange::from_aligned_bytes(mm::PhysAddr{physical.address}, bytes);
        if (!range) return libk::unexpected(SpaceError::InvalidRange);
        const mm::MemoryExtent extent{.object = {0, bytes / mm::page_size},
            .physical = *range, .access = DmaAccess, .type = mm::MemoryType::Device};
        resource::Reservation object_charge{};
        resource::Reservation grant_charge{};
        if (sponsor_ != nullptr) {
            auto memory = sponsor_->reserve(object::MemoryPool::slot_charge());
            auto grant = sponsor_->reserve(cap::GrantGraph::node_charge());
            if (!memory || !grant) return libk::unexpected(SpaceError::QuotaExceeded);
            object_charge = libk::move(memory).value();
            grant_charge = libk::move(grant).value();
        }
        auto pending = object_charge
            ? objects_.create_physical_sponsored(libk::move(object_charge), bytes, {&extent, 1})
            : objects_.create_physical(bytes, {&extent, 1});
        if (!pending) return libk::unexpected(pending.error() == mm::MemoryError::ResourceExhausted
            ? SpaceError::QuotaExceeded : SpaceError::OutOfMemory);
        auto& bar = bars_[index];
        bar.memory = libk::move(pending).value().publish();
        auto target = bar.memory.ref();
        KASSERT(target);
        // Space's allocation root owns these subordinate objects. Binding
        // excludes cleanup until every constructed object is recorded here.
        // Only Space invalidates this lineage; users may duplicate/delegate.
        const cap::GrantCeiling ceiling{
            cap::Rights::of(cap::Right::Map, cap::Right::Inspect,
                cap::Right::Duplicate, cap::Right::Delegate),
            cap::MemoryAuthority{{0, bytes / mm::page_size}, DmaAccess,
                mm::MemoryTypes::of(mm::MemoryType::Device)}};
        auto grant = grants_.create_root(libk::move(grant_charge),
            libk::move(target).value(), ceiling);
        if (!grant) return libk::unexpected(SpaceError::OutOfMemory);
        bar.grant = libk::move(grant).value();
    }
    return libk::expected();
}

auto Space::retire_bars() noexcept -> bool {
    bool complete = true;
    for (auto& bar : bars_) {
        if (!bar.memory) continue;
        if (bar.grant && !bar.revoke.initialized())
            KASSERT(grants_.invalidate(bar.grant.key(), bar.revoke));
        (void)bar.memory.retire();
        if ((bar.revoke.initialized() && !bar.revoke.complete())
            || bar.memory->state() != mm::MemoryState::Retired) complete = false;
    }
    return complete;
}

auto Space::prepare_interrupt() noexcept -> libk::Expected<void, SpaceError> {
    resource::Reservation object_charge{};
    resource::Reservation grant_charge{};
    if (sponsor_ != nullptr) {
        auto object = sponsor_->reserve(object::IrqPool::slot_charge());
        auto grant = sponsor_->reserve(cap::GrantGraph::node_charge());
        if (!object || !grant) return libk::unexpected(SpaceError::QuotaExceeded);
        object_charge = libk::move(object).value();
        grant_charge = libk::move(grant).value();
    }
    const auto source = irq::SourceToken::from_bootstrap(lease_->irq_source());
    auto pending = object_charge
        ? objects_.create_irq_sponsored(libk::move(object_charge), source)
        : objects_.create_irq(source);
    if (!pending) return libk::unexpected(SpaceError::OutOfMemory);
    interrupt_ = libk::move(pending).value().publish();
    auto target = interrupt_.ref();
    KASSERT(target);
    auto grant = grants_.create_root(libk::move(grant_charge), libk::move(target).value(),
        {cap::Rights::of(cap::Right::Inspect, cap::Right::Duplicate, cap::Right::Delegate,
            cap::Right::Route, cap::Right::Observe, cap::Right::Ack),
            cap::IrqAuthority{source.id(), true}});
    if (!grant) return libk::unexpected(SpaceError::OutOfMemory);
    interrupt_grant_ = libk::move(grant).value();
    return libk::expected();
}

auto Space::retire_interrupt() noexcept -> bool {
    if (!interrupt_) return true;
    if (interrupt_grant_ && !interrupt_revoke_.initialized())
        KASSERT(grants_.invalidate(interrupt_grant_.key(), interrupt_revoke_));
    if (interrupt_revoke_.initialized() && !interrupt_revoke_.complete()) return false;
    // Grant drain excludes a concurrent user bind/ack. close serializes with
    // dispatch, masks the source and detaches its Notification relation.
    (void)interrupt_->close();
    (void)interrupt_.retire();
    return interrupt_->state() == irq::State::Closed;
}

void Space::stop_device(void* context) noexcept {
    auto& space = *static_cast<Space*>(context);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.executor_.submit(space);
}

void Space::invalidate_memory(void* context, mm::MemoryWork&& work,
    mm::MemoryInvalidation) noexcept {
    auto& space = *static_cast<Space*>(context);
    sync::IrqLockGuard guard{space.lock_};
    KASSERT(!space.memory_work_);
    space.memory_work_ = libk::move(work);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.executor_.submit(space);
}

void Space::invalidate_device_grant(void* context, cap::GrantWork&& work,
    cap::GrantInvalidation) noexcept {
    auto& space = *static_cast<Space*>(context);
    sync::IrqLockGuard guard{space.lock_};
    KASSERT(!space.device_work_);
    space.device_work_ = libk::move(work);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.executor_.submit(space);
}

void Space::invalidate_memory_grant(void* context, cap::GrantWork&& work,
    cap::GrantInvalidation) noexcept {
    auto& space = *static_cast<Space*>(context);
    sync::IrqLockGuard guard{space.lock_};
    KASSERT(!space.grant_work_);
    space.grant_work_ = libk::move(work);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.executor_.submit(space);
}

void Space::close() noexcept {
    sync::IrqLockGuard guard{lock_};
    if (state_ == SpaceState::Empty) { state_ = SpaceState::Closed; return; }
    if (state_ == SpaceState::Closed || state_ == SpaceState::Failed) return;
    closing_.store<libk::MemoryOrder::Release>(true);
    executor_.submit(*this);
}

void Space::retire(object::ObjectCleanup&& cleanup) noexcept {
    {
        sync::IrqLockGuard guard{lock_};
        if (state_ != SpaceState::Empty && state_ != SpaceState::Closed) {
            cleanup_ = libk::move(cleanup);
            closing_.store<libk::MemoryOrder::Release>(true);
            executor_.submit(*this);
            return;
        }
        state_ = SpaceState::Closed;
    }
    cleanup.complete();
}

void Space::free_pages() noexcept {
    while (pins_ != nullptr) {
        auto* block = libk::exchange(pins_, pins_->next);
        libk::destroy_at(block);
    }
    metadata_.reset();
    charge_.reset();
}

auto Space::service() noexcept -> Completion {
    bool close_hardware{};
    {
        sync::IrqLockGuard guard{lock_};
        if (state_ == SpaceState::Binding || state_ == SpaceState::Failed)
            return {};
        close_hardware = closing_.load<libk::MemoryOrder::Acquire>();
    }
    // Only this executor mutates a published lease. poll may release a large
    // completed translation tree; callbacks need neither that tree nor a lock
    // around its destruction, and self_ retains the complete invocation.
    auto hardware = DeviceLease::State::Closed;
    if (close_hardware) {
        const bool bars_done = retire_bars();
        const bool interrupt_done = retire_interrupt();
        if (!bars_done || !interrupt_done) return {.more = true};
    }
    if (lease_) {
        if (close_hardware) lease_->close();
        hardware = lease_->poll();
    }
    {
        sync::IrqLockGuard guard{lock_};
        if (closing_.load<libk::MemoryOrder::Acquire>()) state_ = SpaceState::Closing;
        if (lease_) {
            if (hardware == DeviceLease::State::Failed) {
                state_ = SpaceState::Failed;
                executor_.withdraw(*this);
                return {}; // retain self, cleanup, source pins and table budget
            }
            if (state_ != SpaceState::Closing) {
                state_ = hardware == DeviceLease::State::Active
                    ? SpaceState::Active : SpaceState::Opening;
                return {.more = state_ == SpaceState::Opening};
            }
            if (hardware != DeviceLease::State::Closed) return {.more = true};
        }
        KASSERT(state_ == SpaceState::Closing);
    }
    // Detach and work release can complete a grant revoke, synchronously
    // advancing ResourcePool close back into Space::retire(). Keep those
    // callbacks outside our lock; self_ retains this executor invocation.
    if (memory_attachment_.attached()) (void)memory_attachment_.detach();
    if (device_grant_.attached()) (void)device_grant_.detach();
    if (memory_grant_.attached()) (void)memory_grant_.detach();
    mm::MemoryWork memory_work{};
    cap::GrantWork device_work{};
    cap::GrantWork grant_work{};
    {
        sync::IrqLockGuard guard{lock_};
        memory_work = libk::move(memory_work_);
        device_work = libk::move(device_work_);
        grant_work = libk::move(grant_work_);
    }
    memory_work.reset();
    device_work.reset();
    grant_work.reset();
    {
        sync::IrqLockGuard guard{lock_};
        if (memory_attachment_.busy() || device_grant_.busy() || memory_grant_.busy())
            return {.more = true};
        // release() serializes against Device's stop callback. All other
        // callbacks are drained above, so no source can enqueue after this.
        lease_.reset();
        executor_.withdraw(*this);
    }
    // Potentially many PageLeases: do not free an arena under an IRQ lock.
    free_pages();
    for (auto& bar : bars_) {
        bar.grant.reset();
        bar.memory.reset();
    }
    interrupt_grant_.reset();
    interrupt_.reset();
    memory_.reset();
    device_.reset();
    sync::IrqLockGuard guard{lock_};
    state_ = SpaceState::Closed;
    return {libk::move(self_), libk::move(cleanup_), false};
}

Executor::~Executor() noexcept { KASSERT(queue_.empty() && !notifier_); }
void Executor::open(Space& space) noexcept {
    sync::IrqLockGuard guard{lock_};
    KASSERT(!space.work_open_ && !space.work_hook_.is_linked());
    space.work_open_ = true;
}
void Executor::submit(Space& space) noexcept {
    Notifier notifier{};
    {
        sync::IrqLockGuard guard{lock_};
        if (!space.work_open_ || space.work_hook_.is_linked()) return;
        queue_.push_back(space);
        notifier = notifier_;
    }
    if (notifier) (void)notifier();
}
void Executor::withdraw(Space& space) noexcept {
    sync::IrqLockGuard guard{lock_};
    space.work_open_ = false;
    if (space.work_hook_.is_linked()) queue_.erase(space);
}
auto Executor::take() noexcept -> Space* {
    sync::IrqLockGuard guard{lock_};
    return queue_.empty() ? nullptr : &queue_.pop_front();
}
auto Executor::run(usize budget) noexcept -> bool {
    KASSERT(budget != 0);
    for (usize index = 0; index < budget; ++index) {
        auto* space = take();
        if (space == nullptr) break;
        auto result = space->service();
        if (result.more) submit(*space);
        if (result.cleanup) result.cleanup.complete();
        // Dropping self is the last operation. space may cease to exist here.
    }
    sync::IrqLockGuard guard{lock_};
    return !queue_.empty();
}
void Executor::bind_notifier(Notifier notifier) noexcept {
    sync::IrqLockGuard guard{lock_};
    KASSERT(notifier && !notifier_);
    notifier_ = notifier;
}
void Executor::unbind_notifier() noexcept {
    sync::IrqLockGuard guard{lock_};
    notifier_.reset();
}
} // namespace kernel::io
