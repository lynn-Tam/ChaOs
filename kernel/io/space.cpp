#include <expected>
#include <optional>
#include <utility>
#include <object/pool.hpp>
#include <io/space.hpp>
#include <ipc/notification.hpp>

#include <io/device.hpp>
#include <cap/graph.hpp>
#include <object/ref.hpp>
#include <mm/mem.hpp>
#include <sync.hpp>

namespace io {
static constexpr auto DmaAccess = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);

Space::Space(mm::Pmm& pmm, Executor& executor, object::pool<irq::Irq>& irqs, object::pool<mm::Mem>& memory,
             cap::GrantGraph& grants) noexcept
    : pmm_(pmm), executor_(executor), irqs_(irqs), mems_(memory), grants_(grants) {}

Space::~Space() noexcept {
    libk_assert(state_ == SpaceState::Empty || state_ == SpaceState::Closed || state_ == SpaceState::Faulted);
    libk_assert(!self_ && !cleanup_ && !lease_ && !pins_ && !work_open_ && !fault_source_.attached());
}

auto Space::watch(ipc::Notification& notification, u64 badge) noexcept -> std::expected<void, SpaceError> {
    if (badge == 0) return std::unexpected(SpaceError::InvalidRange);
    sync::Lock guard{lock_};
    if (state_ != SpaceState::Empty || fault_source_.attached())
        return std::unexpected(SpaceError::InvalidState);
    if (!notification.bind(fault_source_, badge)) return std::unexpected(SpaceError::InvalidState);
    return {};
}

auto Space::state() const noexcept -> SpaceState {
    sync::Lock guard{lock_};
    return state_;
}

auto Space::bar(usize index) noexcept -> std::expected<cap::GrantRef, SpaceError> {
    sync::Lock guard{lock_};
    if (state_ != SpaceState::Active || closing_.load<libk::MemoryOrder::Acquire>())
        return std::unexpected(SpaceError::InvalidState);
    if (index >= bars_.size() || !bars_[index].grant) return std::unexpected(SpaceError::InvalidRange);
    auto grant = bars_[index].grant.clone();
    if (!grant) return std::unexpected(SpaceError::Denied);
    return (std::move(grant).value());
}

auto Space::interrupt() noexcept -> std::expected<cap::GrantRef, SpaceError> {
    sync::Lock guard{lock_};
    if (state_ != SpaceState::Active || closing_.load<libk::MemoryOrder::Acquire>())
        return std::unexpected(SpaceError::InvalidState);
    auto grant = interrupt_grant_.clone();
    if (!grant) return std::unexpected(SpaceError::Denied);
    return (std::move(grant).value());
}

auto Space::info() const noexcept -> std::expected<DeviceInfo, SpaceError> {
    sync::Lock guard{lock_};
    if (state_ != SpaceState::Active || closing_.load<libk::MemoryOrder::Acquire>())
        return std::unexpected(SpaceError::InvalidState);
    return (device_->info());
}

auto Space::bind(object::ref<> self, cap::Resolved<Device>& device, cap::Resolved<mm::Mem>& memory,
                 mm::ObjectRange range, usize first) noexcept -> std::expected<void, SpaceError> {
    auto pin = self.as<Space>();
    if (!pin || &pin.value().get() != this) return std::unexpected(SpaceError::Denied);
    const auto effective = memory.view();
    const auto* limit = std::get_if<cap::MemLimit>(&effective.data);
    if (!device.rights().contains(cap::Right::Connect) || !memory.rights().contains(cap::Right::Map) ||
        limit == nullptr || !limit->range.contains(range) || !limit->access.contains(DmaAccess) ||
        !limit->types.contains(mm::MemoryType::Normal))
        return std::unexpected(SpaceError::Denied);
    const auto tables = mm::PageTable::dma_pages(first, range.size());
    if (!tables || !range.within(memory->page_count())) return std::unexpected(SpaceError::InvalidRange);
    if (memory->kind() != mm::BackingKind::Anonymous) return std::unexpected(SpaceError::UnsupportedMemory);
    {
        sync::Lock guard{lock_};
        if (state_ != SpaceState::Empty) return std::unexpected(SpaceError::InvalidState);
        self_ = std::move(self);
        state_ = SpaceState::Binding;
        executor_.open(*this);
    }
    // Binding owns these fields while allocation and page materialization run
    // with interrupts enabled. Invalidation can only request cancellation and
    // deposit its exact token; service leaves Binding to this invocation.
    std::optional<mm::PageTable> prepared{};
    std::optional<SpaceError> error{};
    {
        auto root = prepare(device, memory, range, first, tables.value());
        if (root)
            prepared.emplace(std::move(root).value());
        else
            error = root.error();
    }
    {
        sync::Lock guard{lock_};
        if (prepared && !closing_.load<libk::MemoryOrder::Acquire>()) {
            lease_->open(std::move(*prepared));
            state_ = SpaceState::Opening;
            executor_.submit(*this);
            return {};
        }
    }
    // Unpublished tables disappear before service can refund their charge.
    // Binding keeps the executor out while this potentially large tree frees.
    prepared.reset();
    {
        sync::Lock guard{lock_};
        state_ = SpaceState::Closing;
        closing_.store<libk::MemoryOrder::Release>(true);
        executor_.submit(*this);
    }
    return std::unexpected(error ? *error : SpaceError::Cancelled);
}

auto Space::prepare(cap::Resolved<Device>& device, cap::Resolved<mm::Mem>& memory, mm::ObjectRange range,
                    usize first, usize tables) noexcept -> std::expected<mm::PageTable, SpaceError> {
    auto device_ref = device.reference();
    auto memory_ref = memory.reference();
    if (!device_ref || !memory_ref) return std::unexpected(SpaceError::Denied);
    auto device_hold = std::move(device_ref).value().as<Device>();
    auto memory_hold = std::move(memory_ref).value().as<mm::Mem>();
    if (!device_hold || !memory_hold) return std::unexpected(SpaceError::Denied);
    device_ = std::move(device_hold).value();
    memory_ = std::move(memory_hold).value();
    if (!device.attach(device_grant_) || !memory.attach(memory_grant_) ||
        !memory_->attach(memory_attachment_, DmaAccess))
        return std::unexpected(SpaceError::Denied);
    auto lease = device_->acquire(Device::Stop::bind<&Space::stop_device>(*this));
    if (!lease) return std::unexpected(SpaceError::Busy);
    lease_.emplace(std::move(*lease));
    auto bars = prepare_bars();
    if (!bars) return std::unexpected(bars.error());
    auto interrupt = prepare_interrupt();
    if (!interrupt) return std::unexpected(interrupt.error());
    const usize metadata = (range.size() - 1) / Pins::Capacity + 1;
    if (sponsor_ != nullptr) {
        auto charge = sponsor_->acquire({.memory = (metadata + tables) * mm::page_size});
        if (!charge) return std::unexpected(SpaceError::QuotaExceeded);
        charge_ = std::move(charge).value();
    }
    metadata_ = pmm_.group();
    Pins** tail = &pins_;
    for (usize offset = 0; offset < range.size();) {
        if (closing_.load<libk::MemoryOrder::Acquire>()) return std::unexpected(SpaceError::Cancelled);
        auto page = metadata_.allocate();
        if (!page) return std::unexpected(SpaceError::OutOfMemory);
        auto* block = libk::construct_at(reinterpret_cast<Pins*>(metadata_.bytes(page.value())));
        *tail = block;
        tail = &block->next;
        for (; block->count < Pins::Capacity && offset < range.size(); ++offset) {
            auto source = memory_->materialize(range.base() + offset);
            if (!source) {
                const auto error = source.error();
                return std::unexpected(error == mm::MemErr::OutOfMemory ? SpaceError::OutOfMemory
                                       : error == mm::MemErr::ResourceExhausted
                                           ? SpaceError::QuotaExceeded
                                           : SpaceError::BackingUnavailable);
            }
            if (source.value().page().type != mm::MemoryType::Normal ||
                !source.value().page().access.contains(DmaAccess))
                return std::unexpected(SpaceError::UnsupportedMemory);
            block->pages[block->count++] = std::move(source).value();
        }
    }
    Pins* block = pins_;
    usize index{};
    auto next = [&]() noexcept {
        libk_assert(block != nullptr && index < block->count);
        const auto page = block->pages[index++].page().page;
        if (index == block->count) {
            block = block->next;
            index = 0;
        }
        return page;
    };
    auto root = mm::PageTable::dma(pmm_, first, range.size(), next, true);
    if (!root) return std::unexpected(SpaceError::OutOfMemory);
    libk_assert(root.value().page_count() == tables);
    return (std::move(root).value());
}

auto Space::prepare_bars() noexcept -> std::expected<void, SpaceError> {
    for (usize index = 0; index < bars_.size(); ++index) {
        const auto& physical = lease_->bars()[index];
        if (physical.size == 0) continue;
        if (closing_.load<libk::MemoryOrder::Acquire>()) return std::unexpected(SpaceError::Cancelled);
        const usize bytes = (physical.size + mm::page_size - 1) & ~(mm::page_size - 1);
        const auto range = mm::Pages::from_aligned_bytes(mm::Phys{physical.address}, bytes);
        if (!range) return std::unexpected(SpaceError::InvalidRange);
        const mm::Extent extent{.object = {0, bytes / mm::page_size},
                                      .physical = *range,
                                      .access = DmaAccess,};
        resource::Reservation object_charge{};
        resource::Reservation grant_charge{};
        if (sponsor_ != nullptr) {
            auto memory = sponsor_->reserve(object::pool<mm::Mem>::slot_charge());
            auto grant = sponsor_->reserve(cap::GrantGraph::node_charge());
            if (!memory || !grant) return std::unexpected(SpaceError::QuotaExceeded);
            object_charge = std::move(memory).value();
            grant_charge = std::move(grant).value();
        }
        auto pending = mems_.make(
            std::move(object_charge), [&](auto& m) { return m.init_phys({&extent, 1}); }, pmm_,
            bytes);
        if (!pending)
            return std::unexpected(pending.error() == mm::MemErr::ResourceExhausted
                                       ? SpaceError::QuotaExceeded
                                       : SpaceError::OutOfMemory);
        auto& bar = bars_[index];
        bar.memory = std::move(pending).value().publish();
        auto target = bar.memory.erase();
        libk_assert(target);
        // Space's allocation root owns these subordinate objects. Binding
        // excludes cleanup until every constructed object is recorded here.
        // Only Space invalidates this lineage; users may duplicate/delegate.
        const cap::View ceiling{cap::Rights::of(cap::Right::Map, cap::Right::Inspect, cap::Right::Duplicate,
                                                cap::Right::Delegate),
                                cap::MemLimit{{0, bytes / mm::page_size},
                                              DmaAccess,
                                              mm::MemoryTypes::of(mm::MemoryType::Device)}};
        auto grant = grants_.create_root(std::move(grant_charge), std::move(target).value(), ceiling);
        if (!grant) return std::unexpected(SpaceError::OutOfMemory);
        bar.grant = std::move(grant).value();
    }
    return {};
}

auto Space::retire_bars() noexcept -> bool {
    bool complete = true;
    for (auto& bar : bars_) {
        if (!bar.memory) continue;
        if (bar.grant && !bar.revoke.initialized())
            libk_assert(grants_.invalidate(bar.grant.key(), bar.revoke));
        (void)bar.memory.retire();
        if ((bar.revoke.initialized() && !bar.revoke.complete()) ||
            bar.memory->state() != mm::MemState::Retired)
            complete = false;
    }
    return complete;
}

auto Space::prepare_interrupt() noexcept -> std::expected<void, SpaceError> {
    resource::Reservation object_charge{};
    resource::Reservation grant_charge{};
    if (sponsor_ != nullptr) {
        auto object = sponsor_->reserve(object::pool<irq::Irq>::slot_charge());
        auto grant = sponsor_->reserve(cap::GrantGraph::node_charge());
        if (!object || !grant) return std::unexpected(SpaceError::QuotaExceeded);
        object_charge = std::move(object).value();
        grant_charge = std::move(grant).value();
    }
    const auto source = lease_->irq_source();
    auto pending =
        object_charge ? irqs_.create(std::move(object_charge), source) : irqs_.create(source);
    if (!pending) return std::unexpected(SpaceError::OutOfMemory);
    interrupt_ = std::move(pending).value().publish();
    auto target = interrupt_.erase();
    libk_assert(target);
    auto grant =
        grants_.create_root(std::move(grant_charge), std::move(target).value(),
                            {cap::Rights::of(cap::Right::Inspect, cap::Right::Duplicate, cap::Right::Delegate,
                                             cap::Right::Route, cap::Right::Observe, cap::Right::Ack),
                             cap::IrqRoute{source.id, source.level}});
    if (!grant) return std::unexpected(SpaceError::OutOfMemory);
    interrupt_grant_ = std::move(grant).value();
    return {};
}

auto Space::retire_interrupt() noexcept -> bool {
    if (!interrupt_) return true;
    if (interrupt_grant_ && !interrupt_revoke_.initialized())
        libk_assert(grants_.invalidate(interrupt_grant_.key(), interrupt_revoke_));
    if (interrupt_revoke_.initialized() && !interrupt_revoke_.complete()) return false;
    // Grant drain excludes a concurrent user bind/ack. close serializes with
    // dispatch, masks the source and detaches its Notification relation.
    (void)interrupt_->close();
    (void)interrupt_.retire();
    return interrupt_->closed();
}

void Space::stop_device(bool fault) noexcept {
    auto& space = *this;
    if (fault) space.fault_signal_.store<libk::MemoryOrder::Release>(true);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.executor_.submit(space);
}

void Space::invalidate_memory(void* ctx, mm::MemWork&& work) noexcept {
    auto& space = *static_cast<Space*>(ctx);
    sync::Lock guard{space.lock_};
    libk_assert(!space.memory_work_);
    space.memory_work_ = std::move(work);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.executor_.submit(space);
}

void Space::invalidate_device_grant(void* ctx, cap::GrantWork&& work, cap::GrantInvalidation) noexcept {
    auto& space = *static_cast<Space*>(ctx);
    sync::Lock guard{space.lock_};
    libk_assert(!space.device_work_);
    space.device_work_ = std::move(work);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.executor_.submit(space);
}

void Space::invalidate_memory_grant(void* ctx, cap::GrantWork&& work, cap::GrantInvalidation) noexcept {
    auto& space = *static_cast<Space*>(ctx);
    sync::Lock guard{space.lock_};
    libk_assert(!space.grant_work_);
    space.grant_work_ = std::move(work);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.executor_.submit(space);
}

void Space::close() noexcept {
    bool empty{};
    {
        sync::Lock guard{lock_};
        if (state_ == SpaceState::Empty) {
            state_ = SpaceState::Closed;
            empty = true;
        } else if (state_ == SpaceState::Closed || state_ == SpaceState::Faulted ||
                   state_ == SpaceState::Failed)
            return;
        else {
            closing_.store<libk::MemoryOrder::Release>(true);
            executor_.submit(*this);
        }
    }
    if (empty) fault_source_.reset();
}

void Space::retire(object::cleanup&& cleanup) noexcept {
    {
        sync::Lock guard{lock_};
        if (state_ != SpaceState::Empty && state_ != SpaceState::Closed && state_ != SpaceState::Faulted) {
            cleanup_ = std::move(cleanup);
            closing_.store<libk::MemoryOrder::Release>(true);
            executor_.submit(*this);
            return;
        }
        state_ = SpaceState::Closed;
    }
    fault_source_.reset();
    cleanup.complete();
}

void Space::free_pages() noexcept {
    while (pins_ != nullptr) {
        auto* block = std::exchange(pins_, pins_->next);
        libk::destroy_at(block);
    }
    metadata_.reset();
    charge_.reset();
}

auto Space::service() noexcept -> Completion {
    bool close_hardware{};
    bool notify_fault{};
    {
        sync::Lock guard{lock_};
        if (state_ == SpaceState::Binding || state_ == SpaceState::Failed) return {};
        if (fault_signal_.exchange<libk::MemoryOrder::AcqRel>(false)) {
            faulted_ = true;
            state_ = SpaceState::Closing;
            notify_fault = true;
        }
        close_hardware = closing_.load<libk::MemoryOrder::Acquire>();
    }
    if (notify_fault) (void)fault_source_.signal();
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
    if (hardware == DeviceLease::State::Failed) {
        closing_.store<libk::MemoryOrder::Release>(true);
        if (!close_hardware) {
            const bool bars_done = retire_bars();
            const bool interrupt_done = retire_interrupt();
            if (!bars_done || !interrupt_done) return {.more = true};
        }
        {
            sync::Lock guard{lock_};
            state_ = SpaceState::Failed;
            executor_.withdraw(*this);
        }
        // Hardware drain is unproved: retain the lease, pins and structural
        // self-reference, but wake the owner so it cannot wait for an IRQ.
        (void)fault_source_.signal();
        return {};
    }
    {
        sync::Lock guard{lock_};
        if (closing_.load<libk::MemoryOrder::Acquire>()) state_ = SpaceState::Closing;
        if (lease_) {
            if (state_ != SpaceState::Closing) {
                state_ = hardware == DeviceLease::State::Active ? SpaceState::Active : SpaceState::Opening;
                return {.more = state_ == SpaceState::Opening};
            }
            if (hardware != DeviceLease::State::Closed) return {.more = true};
        }
        libk_assert(state_ == SpaceState::Closing);
    }
    // Detach and work release can complete a grant revoke, synchronously
    // advancing ResourcePool close back into Space::retire(). Keep those
    // callbacks outside our lock; self_ retains this executor invocation.
    if (memory_attachment_.attached()) (void)memory_attachment_.detach();
    if (device_grant_.attached()) (void)device_grant_.detach();
    if (memory_grant_.attached()) (void)memory_grant_.detach();
    mm::MemWork memory_work{};
    cap::GrantWork device_work{};
    cap::GrantWork grant_work{};
    {
        sync::Lock guard{lock_};
        memory_work = std::move(memory_work_);
        device_work = std::move(device_work_);
        grant_work = std::move(grant_work_);
    }
    memory_work.reset();
    device_work.reset();
    grant_work.reset();
    {
        sync::Lock guard{lock_};
        if (memory_attachment_.busy() || device_grant_.busy() || memory_grant_.busy()) return {.more = true};
        // release() serializes against Device's stop callback. All other
        // callbacks are drained above, so no source can enqueue after this.
        lease_.reset();
        executor_.withdraw(*this);
    }
    // Potentially many PageHolds: do not free an arena under an IRQ lock.
    free_pages();
    for (auto& bar : bars_) {
        bar.grant.reset();
        bar.memory.reset();
    }
    interrupt_grant_.reset();
    interrupt_.reset();
    memory_.reset();
    device_.reset();
    if (fault_signal_.exchange<libk::MemoryOrder::AcqRel>(false) && !faulted_) {
        faulted_ = true;
        (void)fault_source_.signal();
    }
    fault_source_.reset();
    sync::Lock guard{lock_};
    state_ = faulted_ ? SpaceState::Faulted : SpaceState::Closed;
    return {std::move(self_), std::move(cleanup_), false};
}

Executor::~Executor() noexcept { libk_assert(queue_.empty() && !notifier_); }
void Executor::open(Space& space) noexcept {
    sync::Lock guard{lock_};
    libk_assert(!space.work_open_ && !space.work_hook_.is_linked());
    space.work_open_ = true;
}
void Executor::submit(Space& space) noexcept {
    Notifier notifier{};
    {
        sync::Lock guard{lock_};
        if (!space.work_open_ || space.work_hook_.is_linked()) return;
        queue_.push_back(space);
        notifier = notifier_;
    }
    if (notifier) (void)notifier();
}
void Executor::withdraw(Space& space) noexcept {
    sync::Lock guard{lock_};
    space.work_open_ = false;
    if (space.work_hook_.is_linked()) queue_.erase(space);
}
auto Executor::take() noexcept -> Space* {
    sync::Lock guard{lock_};
    return queue_.empty() ? nullptr : &queue_.pop_front();
}
auto Executor::run(usize budget) noexcept -> bool {
    libk_assert(budget != 0);
    for (usize index = 0; index < budget; ++index) {
        auto* space = take();
        if (space == nullptr) break;
        auto result = space->service();
        if (result.more) submit(*space);
        if (result.cleanup) result.cleanup.complete();
        // Dropping self is the last operation. space may cease to exist here.
    }
    sync::Lock guard{lock_};
    return !queue_.empty();
}
void Executor::bind_notifier(Notifier notifier) noexcept {
    sync::Lock guard{lock_};
    libk_assert(notifier && !notifier_);
    notifier_ = notifier;
}
void Executor::unbind_notifier() noexcept {
    sync::Lock guard{lock_};
    notifier_.reset();
}
} // namespace io
