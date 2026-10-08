#include <expected>
#include <optional>
#include <utility>
#include <object/pool.hpp>
#include <io/space.hpp>
#include <ipc/notification.hpp>

#include <io/host.hpp>
#include <cap/grant.hpp>
#include <object/ref.hpp>
#include <mm/mem.hpp>
#include <sync.hpp>

namespace io {
static constexpr auto DmaAccess = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);

Space::Space(mm::Pmm& pmm, WorkQueue& work, object::pool<irq::Irq>& irqs, object::pool<mm::Mem>& memory,
             cap::Graph& grants) noexcept
    : pmm_(pmm), work_(work), irqs_(irqs), mems_(memory), grants_(grants), reg_store_(pmm) {}

Space::~Space() noexcept {
    libk_assert(state_ == SpaceState::Empty || state_ == SpaceState::Closed || state_ == SpaceState::Faulted);
    libk_assert(!self_ && !cleanup_ && !hw_ && !pins_ && !fault_source_.attached());
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

auto Space::reg(usize index) noexcept -> std::expected<std::pair<cap::GrantRef, usize>, SpaceError> {
    sync::Lock guard{lock_};
    if (state_ != SpaceState::Active || closing_.load<libk::MemoryOrder::Acquire>())
        return std::unexpected(SpaceError::InvalidState);
    const auto sources = hw_.get()->regs();
    if (index >= sources.size()) return std::unexpected(SpaceError::InvalidRange);
    for (const auto& reg : regs_) {
        if (reg.index != index) continue;
        auto grant = reg.grant.clone();
        if (!grant) return std::unexpected(SpaceError::Denied);
        return std::pair{std::move(*grant), sources[index].size};
    }
    return std::unexpected(SpaceError::Absent);
}

auto Space::interrupt() noexcept -> std::expected<cap::GrantRef, SpaceError> {
    sync::Lock guard{lock_};
    if (state_ != SpaceState::Active || closing_.load<libk::MemoryOrder::Acquire>())
        return std::unexpected(SpaceError::InvalidState);
    auto grant = interrupt_grant_.clone();
    if (!grant) return std::unexpected(SpaceError::Denied);
    return (std::move(grant).value());
}

auto Space::bind(object::ref<> self, cap::Resolved<Host>& host, cap::Resolved<mm::Mem>& memory,
                 mm::ObjectRange range, usize first) noexcept -> std::expected<void, SpaceError> {
    auto pin = self.as<Space>();
    if (!pin || &pin.value().get() != this) return std::unexpected(SpaceError::Denied);
    const auto effective = memory.view();
    const auto* limit = std::get_if<cap::MemLimit>(&effective.data);
    if (!host.rights().contains(cap::Right::Connect) || !memory.rights().contains(cap::Right::Map) ||
        limit == nullptr || !limit->range.contains(range) || !limit->perms.contains(DmaAccess))
        return std::unexpected(SpaceError::Denied);
    const auto view = host.view();
    const auto* requesters = std::get_if<cap::HostLimit>(&view.data);
    if (!requesters || requesters->count != 1) return std::unexpected(SpaceError::Denied);
    const auto tables = mm::PageTable::dma_pages(first, range.size());
    if (!tables || !range.within(memory->page_count())) return std::unexpected(SpaceError::InvalidRange);
    if (memory->kind() != mm::BackingKind::Anonymous) return std::unexpected(SpaceError::UnsupportedMemory);
    {
        sync::Lock guard{lock_};
        if (state_ != SpaceState::Empty) return std::unexpected(SpaceError::InvalidState);
        self_ = std::move(self);
        state_ = SpaceState::Binding;
        work_.open(job_, Work::Fn::bind<&Space::run_work>(*this));
    }
    // Binding owns these fields while allocation and page materialization run
    // with interrupts enabled. Invalidation can only request cancellation and
    // deposit its exact token; service leaves Binding to this invocation.
    std::optional<mm::PageTable> prepared{};
    std::optional<SpaceError> error{};
    {
        auto root = prepare(host, requesters->first, memory, range, first, tables.value());
        if (root)
            prepared.emplace(std::move(root).value());
        else
            error = root.error();
    }
    {
        sync::Lock guard{lock_};
        if (prepared && !closing_.load<libk::MemoryOrder::Acquire>()) {
            hw_.get()->open(std::move(*prepared));
            state_ = SpaceState::Opening;
            work_.post(job_);
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
        work_.post(job_);
    }
    return std::unexpected(error ? *error : SpaceError::Cancelled);
}

auto Space::prepare(cap::Resolved<Host>& host, usize requester, cap::Resolved<mm::Mem>& memory, mm::ObjectRange range,
                    usize first, usize tables) noexcept -> std::expected<mm::PageTable, SpaceError> {
    auto host_ref = host.reference();
    auto memory_ref = memory.reference();
    if (!host_ref || !memory_ref) return std::unexpected(SpaceError::Denied);
    auto host_hold = std::move(host_ref).value().as<Host>();
    auto memory_hold = std::move(memory_ref).value().as<mm::Mem>();
    if (!host_hold || !memory_hold) return std::unexpected(SpaceError::Denied);
    host_ = std::move(host_hold).value();
    memory_ = std::move(memory_hold).value();
    if (!host.attach(host_grant_) || !memory.attach(memory_grant_) ||
        !memory_->attach(memory_attachment_, DmaAccess))
        return std::unexpected(SpaceError::Denied);
    auto lease = host_->acquire(requester, Hw::Stop::bind<&Space::stop_host>(*this));
    if (!lease) return std::unexpected(lease.error() == BindErr::Busy ? SpaceError::Busy
        : lease.error() == BindErr::NoMemory ? SpaceError::OutOfMemory : SpaceError::BackingUnavailable);
    hw_.reset(*lease);
    auto regs = prepare_regs();
    if (!regs) return std::unexpected(regs.error());
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
            if (!pmm_.is_ram(source.value().page()) ||
                !source.value().perms().contains(DmaAccess))
                return std::unexpected(SpaceError::UnsupportedMemory);
            block->pages[block->count++] = std::move(source).value();
        }
    }
    Pins* block = pins_;
    usize index{};
    auto next = [&]() noexcept {
        libk_assert(block != nullptr && index < block->count);
        const auto page = block->pages[index++].page();
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

auto Space::prepare_regs() noexcept -> std::expected<void, SpaceError> {
    const auto sources = hw_.get()->regs();
    const object::ref<> unpaid{};
    const auto& payer = sponsor_ ? sponsor_->payer() : unpaid;
    for (usize index = 0; index < sources.size(); ++index) {
        const auto physical = sources[index];
        const auto perms = physical.perms;
        if (physical.size == 0) continue;
        if (closing_.load<libk::MemoryOrder::Acquire>()) return std::unexpected(SpaceError::Cancelled);
        const usize bytes = (physical.size + mm::page_size - 1) & ~(mm::page_size - 1);
        const auto range = mm::Pages::from_aligned_bytes(mm::Phys{physical.pa}, bytes);
        if (!range) return std::unexpected(SpaceError::InvalidRange);
        const mm::Extent extent{.object = {0, bytes / mm::page_size},
                                      .physical = *range,
                                      .perms = perms};
        resource::Reservation object_charge{};
        resource::Reservation grant_charge{};
        if (sponsor_ != nullptr) {
            auto memory = sponsor_->reserve(object::pool<mm::Mem>::slot_charge());
            auto grant = sponsor_->reserve(cap::Graph::node_charge());
            if (!memory || !grant) return std::unexpected(SpaceError::QuotaExceeded);
            object_charge = std::move(memory).value();
            grant_charge = std::move(grant).value();
        }
        auto pending = mems_.create(std::move(object_charge), pmm_, bytes, mm::PhysCfg{{&extent, 1}, {}});
        if (!pending)
            return std::unexpected(pending.error() == mm::MemErr::ResourceExhausted
                                       ? SpaceError::QuotaExceeded
                                       : SpaceError::OutOfMemory);
        auto node = reg_store_.create(payer, index);
        if (!node) return std::unexpected(node.error() == mm::SlabErr::ResourceExhausted
            ? SpaceError::QuotaExceeded : SpaceError::OutOfMemory);
        auto& reg = **node;
        regs_.push_back(reg);
        reg.memory = std::move(pending).value().publish();
        auto target = reg.memory.erase();
        libk_assert(target);
        // Space's allocation root owns these subordinate objects. Binding
        // excludes cleanup until every constructed object is recorded here.
        // Only Space invalidates this lineage; users may duplicate/delegate.
        const cap::View ceiling{cap::Rights::of(cap::Right::Map, cap::Right::Inspect, cap::Right::Duplicate,
                                                cap::Right::Delegate),
                                cap::MemLimit{{0, bytes / mm::page_size},
                                              perms}};
        auto grant = grants_.create_root(std::move(grant_charge), std::move(target).value(), ceiling);
        if (!grant) return std::unexpected(SpaceError::OutOfMemory);
        reg.grant = std::move(grant).value();
    }
    return {};
}

auto Space::retire_regs() noexcept -> bool {
    bool complete = true;
    for (auto& reg : regs_) {
        if (!reg.memory) continue;
        if (reg.grant && !reg.revoke.initialized())
            libk_assert(grants_.invalidate(reg.grant.key(), reg.revoke));
        (void)reg.memory.retire();
        if ((reg.revoke.initialized() && !reg.revoke.complete()) ||
            reg.memory->state() != mm::MemState::Retired)
            complete = false;
    }
    return complete;
}

auto Space::prepare_interrupt() noexcept -> std::expected<void, SpaceError> {
    resource::Reservation object_charge{};
    resource::Reservation grant_charge{};
    if (sponsor_ != nullptr) {
        auto object = sponsor_->reserve(object::pool<irq::Irq>::slot_charge());
        auto grant = sponsor_->reserve(cap::Graph::node_charge());
        if (!object || !grant) return std::unexpected(SpaceError::QuotaExceeded);
        object_charge = std::move(object).value();
        grant_charge = std::move(grant).value();
    }
    const auto source = hw_.get()->irq();
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

void Space::stop_host(bool fault) noexcept {
    auto& space = *this;
    if (fault) space.fault_signal_.store<libk::MemoryOrder::Release>(true);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.work_.post(space.job_);
}

void Space::invalidate_memory(void* ctx, mm::MemWork&& work) noexcept {
    auto& space = *static_cast<Space*>(ctx);
    sync::Lock guard{space.lock_};
    libk_assert(!space.memory_work_);
    space.memory_work_ = std::move(work);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.work_.post(space.job_);
}

void Space::invalidate_host_grant(void* ctx, cap::GrantWork&& work) noexcept {
    auto& space = *static_cast<Space*>(ctx);
    sync::Lock guard{space.lock_};
    libk_assert(!space.host_work_);
    space.host_work_ = std::move(work);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.work_.post(space.job_);
}

void Space::invalidate_memory_grant(void* ctx, cap::GrantWork&& work) noexcept {
    auto& space = *static_cast<Space*>(ctx);
    sync::Lock guard{space.lock_};
    libk_assert(!space.grant_work_);
    space.grant_work_ = std::move(work);
    space.closing_.store<libk::MemoryOrder::Release>(true);
    space.work_.post(space.job_);
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
            work_.post(job_);
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
            work_.post(job_);
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
    auto hardware = Hw::State::Closed;
    if (close_hardware) {
        const bool regs_done = retire_regs();
        const bool interrupt_done = retire_interrupt();
        if (!regs_done || !interrupt_done) return {.more = true};
    }
    if (hw_) {
        if (close_hardware) hw_.get()->close();
        hardware = hw_.get()->poll();
    }
    if (hardware == Hw::State::Failed) {
        closing_.store<libk::MemoryOrder::Release>(true);
        if (!close_hardware) {
            const bool regs_done = retire_regs();
            const bool interrupt_done = retire_interrupt();
            if (!regs_done || !interrupt_done) return {.more = true};
        }
        {
            sync::Lock guard{lock_};
            state_ = SpaceState::Failed;
            work_.close(job_);
        }
        // Hardware drain is unproved: retain the lease, pins and structural
        // self-reference, but wake the owner so it cannot wait for an IRQ.
        (void)fault_source_.signal();
        return {};
    }
    {
        sync::Lock guard{lock_};
        if (closing_.load<libk::MemoryOrder::Acquire>()) state_ = SpaceState::Closing;
        if (hw_) {
            if (state_ != SpaceState::Closing) {
                state_ = hardware == Hw::State::Active ? SpaceState::Active : SpaceState::Opening;
                return {.more = state_ == SpaceState::Opening};
            }
            if (hardware != Hw::State::Closed) return {.more = true};
        }
        libk_assert(state_ == SpaceState::Closing);
    }
    // Detach and work release can complete a grant revoke, synchronously
    // advancing ResourcePool close back into Space::retire(). Keep those
    // callbacks outside our lock; self_ retains this executor invocation.
    if (memory_attachment_.attached()) (void)memory_attachment_.detach();
    if (host_grant_.attached()) (void)host_grant_.detach();
    if (memory_grant_.attached()) (void)memory_grant_.detach();
    mm::MemWork memory_work{};
    cap::GrantWork host_work{};
    cap::GrantWork grant_work{};
    {
        sync::Lock guard{lock_};
        memory_work = std::move(memory_work_);
        host_work = std::move(host_work_);
        grant_work = std::move(grant_work_);
    }
    memory_work.reset();
    host_work.reset();
    grant_work.reset();
    {
        sync::Lock guard{lock_};
        if (memory_attachment_.busy() || host_grant_.busy() || memory_grant_.busy()) return {.more = true};
        // release() drains hardware fault delivery. All other
        // callbacks are drained above, so no source can enqueue after this.
        hw_.reset();
        work_.close(job_);
    }
    // Potentially many PageHolds: do not free an arena under an IRQ lock.
    free_pages();
    while (!regs_.empty()) reg_store_.destroy(regs_.pop_front());
    interrupt_grant_.reset();
    interrupt_.reset();
    memory_.reset();
    host_.reset();
    if (fault_signal_.exchange<libk::MemoryOrder::AcqRel>(false) && !faulted_) {
        faulted_ = true;
        (void)fault_source_.signal();
    }
    fault_source_.reset();
    sync::Lock guard{lock_};
    state_ = faulted_ ? SpaceState::Faulted : SpaceState::Closed;
    return {std::move(self_), std::move(cleanup_), false};
}

void Space::run_work() noexcept {
    auto result = service();
    if (result.more) work_.post(job_);
    if (result.cleanup) result.cleanup.complete();
    // result.self is released last, after the queue can no longer touch us.
}

} // namespace io
