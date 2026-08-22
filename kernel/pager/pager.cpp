#include <pager/pager.hpp>

#include <libk/limits.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::pager {

Reply::Reply(Reply&& other) noexcept
    : pager_(libk::exchange(other.pager_, nullptr)),
      key_(other.key_), request_(other.request_),
      attachment_(other.attachment_) {
    other.key_ = {};
    other.request_ = {};
    other.attachment_ = nullptr;
}

auto Reply::operator=(Reply&& other) noexcept -> Reply& {
    if (this != &other) {
        KASSERT(pager_ == nullptr);
        pager_ = libk::exchange(other.pager_, nullptr);
        key_ = other.key_;
        request_ = other.request_;
        attachment_ = other.attachment_;
        other.key_ = {};
        other.request_ = {};
        other.attachment_ = nullptr;
    }
    return *this;
}

Reply::~Reply() noexcept {
    KASSERT(pager_ == nullptr);
}

auto Reply::commit() noexcept -> libk::Expected<void, Error> {
    Pager* const pager = libk::exchange(pager_, nullptr);
    if (pager == nullptr) {
        return libk::unexpected(Error::Stale);
    }
    return pager->finish_reply(*this);
}

auto Reply::abort() noexcept -> libk::Expected<void, Error> {
    Pager* const pager = libk::exchange(pager_, nullptr);
    if (pager == nullptr) {
        return libk::unexpected(Error::Stale);
    }
    return pager->abort_reply(*this);
}

Pager::Pager() noexcept
    : source_link_(ipc::NotificationSource::bind<
          Pager,
          &Pager::notification_closed>(*this)) {}

Pager::~Pager() noexcept {
    /*luna change: include producer waiter unlink in Pager teardown,
      reason: attachment capacity hooks must be empty before storage dies*/
    KASSERT(state_ == State::Open || state_ == State::Closed);
    KASSERT(claimed_ == 0 && !cleanup_);
    KASSERT(notification_ == nullptr && attachments_.empty()
        && capacity_waiters_.empty() && !source_link_.attached());
}

auto Pager::state() const noexcept -> State {
    kernel::sync::IrqLockGuard guard{lock_};
    return state_;
}

auto Pager::pending() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return ready_.size();
}

auto Pager::view(const Slot& slot, u16 index) const noexcept -> Request {
    const RequestKey delivery{
        .slot = index,
        .generation = slot.generation,
    };
    Request result{
        .key = delivery,
        .claim = (slot.state == TransportState::Claimed
                || slot.state == TransportState::Completing)
            ? ClaimKey{delivery, slot.claim_generation}
            : ClaimKey{},
        .kind = slot.kind,
        .page_key = mm::PageKey{slot.page_generation, slot.page_index},
        .urgency = slot.urgency,
    };
    if (slot.kind == DeliveryKind::PageIn) {
        result.first = slot.payload.page_in.first;
        result.count = slot.payload.page_in.count;
        result.backing_epoch = slot.payload.page_in.backing_epoch;
    } else {
        result.first = slot.page_index;
        result.count = 1;
        result.backing_epoch = slot.payload.writeback.dirty_epoch;
        result.writeback_generation = slot.payload.writeback.generation;
        result.dirty_epoch = slot.payload.writeback.dirty_epoch;
    }
    return result;
}

auto Pager::find_locked(RequestKey key) noexcept -> Slot* {
    if (!key || key.slot >= max_requests) {
        return nullptr;
    }
    Slot& slot = slots_[key.slot];
    return slot.state != TransportState::Free
        && slot.generation == key.generation
        ? &slot : nullptr;
}

auto Pager::find_claim_locked(ClaimKey key) noexcept -> Slot* {
    if (!key) {
        return nullptr;
    }
    Slot* const slot = find_locked(key.delivery);
    if (slot == nullptr || slot->state != TransportState::Claimed
        || slot->claim_generation != key.generation) {
        return nullptr;
    }
    // A record published through an attachment becomes stale as soon as the
    // reverse relation detaches; no new owner transition may commit into it.
    if (slot->attachment != nullptr
        && (slot->attachment->state != PagerAttachment::State::Attached
            || slot->attachment_generation != slot->attachment->generation)) {
        return nullptr;
    }
    return slot;
}

auto Pager::find_free_locked() noexcept -> Slot* {
    for (Slot& slot : slots_) {
        if (slot.state == TransportState::Free) {
            return &slot;
        }
    }
    return nullptr;
}

/*luna change: deliver one producer-ready callback under an attachment lease,
  reason: Pager owns capacity while MemoryObject owns the retained obligation
  and callbacks must run without the Pager lock*/
void Pager::wake_capacity() noexcept {
    PagerAttachment* attachment{};
    PagerAttachment::Ready ready{};
    void* context{};
    u64 generation{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open || capacity_waiters_.empty()
            || find_free_locked() == nullptr || ready_.full()) {
            return;
        }
        auto it = capacity_waiters_.begin();
        attachment = &*it;
        KASSERT(attachment->state == PagerAttachment::State::Attached);
        KASSERT(attachment->hook_.is_linked()
            && attachment->capacity_hook_.is_linked()
            && attachment->ready != nullptr);
        KASSERT(attachment->leases
            != libk::numeric_limits<u32>::max());
        capacity_waiters_.erase(*attachment);
        ++attachment->leases;
        generation = attachment->generation;
        ready = attachment->ready;
        context = attachment->context;
    }

    ready(context);

    bool drained{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(attachment->leases != 0);
        --attachment->leases;
        KASSERT(attachment->state == PagerAttachment::State::Attached
            || attachment->state == PagerAttachment::State::Retiring);
        KASSERT(attachment->state == PagerAttachment::State::Retiring
            || attachment->generation == generation);
        if (attachment->state == PagerAttachment::State::Retiring
            && attachment->leases == 0) {
            attachment->state = PagerAttachment::State::Detached;
            drained = true;
        }
    }
    if (drained && attachment->drained != nullptr) {
        attachment->drained(context);
    }
}

auto Pager::attach(PagerAttachment& attachment) noexcept -> bool {
    if (!attachment) {
        return false;
    }
    kernel::sync::IrqLockGuard guard{lock_};
    /*luna change: reject stale derived capacity membership on attach,
      reason: a reused attachment must begin with both hooks detached*/
    if (state_ != State::Open || attachment.hook_.is_linked()
        || attachment.capacity_hook_.is_linked()
        || attachment.state != PagerAttachment::State::Detached
        || attachment.generation == libk::numeric_limits<u64>::max()) {
        return false;
    }
    ++attachment.generation;
    attachment.leases = 0;
    attachment.state = PagerAttachment::State::Attached;
    attachments_.push_back(attachment);
    return true;
}

/*luna change: run forced attachment teardown as a constant-space pass, reason: bounded transport records must not inflate the kernel stack and one pass lease keeps callbacks and in-flight completions safe*/
auto Pager::detach(PagerAttachment& attachment) noexcept -> bool {
    bool close{};
    bool drained{};
    u64 old_generation{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (attachment.state != PagerAttachment::State::Attached
            || !attachment.hook_.is_linked()
            || attachment.leases == libk::numeric_limits<u32>::max()) {
            return false;
        }
        attachment.state = PagerAttachment::State::Retiring;
        attachments_.erase(attachment);
        /*luna change: unlink capacity admission at retirement, reason:
          detached attachment storage cannot remain in the derived waiter
          index while existing leases drain*/
        if (attachment.capacity_hook_.is_linked()) {
            capacity_waiters_.erase(attachment);
        }
        old_generation = attachment.generation;
        if (attachment.generation != libk::numeric_limits<u64>::max()) {
            ++attachment.generation;
        }
        ++attachment.leases;
    }
    for (;;) {
        Request forced{};
        bool removed{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            for (auto it = ready_.begin(); it != ready_.end(); ++it) {
                const u16 index = *it;
                Slot& slot = slots_[index];
                if (slot.state != TransportState::Queued
                    || slot.attachment != &attachment
                    || slot.attachment_generation != old_generation) {
                    continue;
                }
                KASSERT(slot.leases == 0);
                forced = view(slot, index);
                ready_.erase(it);
                slot.state = TransportState::Free;
                slot.attachment = nullptr;
                slot.attachment_generation = 0;
                removed = true;
                break;
            }
            if (!removed) {
                for (Slot& slot : slots_) {
                    if ((slot.state != TransportState::Claimed
                            && slot.state != TransportState::Completing)
                        || slot.attachment != &attachment
                        || slot.attachment_generation != old_generation
                        || slot.leases != 0) {
                        continue;
                    }
                    forced = view(
                        slot, static_cast<u16>(&slot - slots_));
                    KASSERT(claimed_ != 0);
                    --claimed_;
                    release_claim_locked(slot);
                    slot.state = TransportState::Free;
                    slot.attachment = nullptr;
                    slot.attachment_generation = 0;
                    removed = true;
                    break;
                }
            }
            if (!removed) {
                KASSERT(attachment.leases != 0);
                --attachment.leases;
                if (attachment.state == PagerAttachment::State::Retiring
                    && attachment.leases == 0) {
                    attachment.state = PagerAttachment::State::Detached;
                    drained = true;
                }
                close = state_ == State::Closing
                    && ready_.empty() && claimed_ == 0;
                if (close) {
                    state_ = State::Closed;
                }
            }
        }
        if (!removed) {
            break;
        }
        static_cast<void>(attachment.transition(
            attachment.context, forced, PagerAttachment::Event::Forced));
        wake_capacity();
    }
    if (drained && attachment.drained != nullptr) {
        attachment.drained(attachment.context);
    }
    if (close && cleanup_) {
        cleanup_.complete();
    }
    return true;
}

auto Pager::publish(
    PagerAttachment* attachment,
    DeliveryKind kind,
    mm::PageKey page_key,
    usize first,
    usize count,
    u64 backing_epoch,
    u64 writeback_generation,
    u64 dirty_epoch,
    u8 urgency) noexcept -> libk::Expected<Request, Error> {
    if (!page_key || count == 0 || count > max_request_pages
        || first > libk::numeric_limits<usize>::max() - count
        || backing_epoch == 0
        || (kind == DeliveryKind::Writeback
            && (writeback_generation == 0 || dirty_epoch == 0))) {
        return libk::unexpected(Error::InvalidRange);
    }
    Request result{};
    bool signal{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open) {
            return libk::unexpected(Error::Closed);
        }
        if (attachment != nullptr
            && (attachment->state != PagerAttachment::State::Attached
                || !attachment->hook_.is_linked())) {
            return libk::unexpected(Error::Stale);
        }
        Slot* const slot = find_free_locked();
        if (slot == nullptr || ready_.full()) {
            /*luna change: retain one derived capacity waiter on Full,
              reason: the PageSlot obligation waits for an exact Pager slot
              release instead of requeueing runnable work*/
            if (attachment != nullptr && attachment->ready != nullptr
                && !attachment->capacity_hook_.is_linked()) {
                capacity_waiters_.push_back(*attachment);
            }
            return libk::unexpected(Error::Full);
        }
        if (attachment != nullptr && attachment->capacity_hook_.is_linked()) {
            /*luna change: consume a satisfied capacity waiter on publish,
              reason: one successful producer admission needs no stale wake*/
            capacity_waiters_.erase(*attachment);
        }
        if (slot->generation == libk::numeric_limits<u64>::max()) {
            return libk::unexpected(Error::GenerationExhausted);
        }
        ++slot->generation;
        slot->claim_generation = 0;
        slot->state = TransportState::Queued;
        slot->attachment = attachment;
        slot->attachment_generation = attachment != nullptr
            ? attachment->generation : 0;
        slot->page_index = page_key.index;
        slot->page_generation = page_key.generation;
        slot->kind = kind;
        slot->urgency = urgency;
        if (kind == DeliveryKind::PageIn) {
            slot->payload.page_in = Pager::Slot::Payload::PageIn{
                .first = first,
                .count = count,
                .backing_epoch = backing_epoch,
            };
        } else {
            slot->payload.writeback = Pager::Slot::Payload::Writeback{
                .generation = writeback_generation,
                .dirty_epoch = dirty_epoch,
            };
        }
        const u16 index = static_cast<u16>(slot - slots_);
        if (!ready_.try_push_back(index)) {
            slot->state = TransportState::Free;
            return libk::unexpected(Error::Full);
        }
        result = view(*slot, index);
        signal = notification_ != nullptr;
    }
    if (signal) {
        static_cast<void>(source_link_.signal());
    }
    return libk::expected(result);
}

auto Pager::try_claim() noexcept -> libk::Expected<Request, Error> {
    Request request{};
    PagerAttachment* attachment{};
    u64 attachment_generation{};
    bool owner{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open) {
            return libk::unexpected(Error::Closed);
        }
        if (ready_.empty()) {
            return libk::unexpected(Error::Busy);
        }
        const u16 index = ready_.front();
        Slot& slot = slots_[index];
        KASSERT(slot.state == TransportState::Queued);
        if (slot.claim_generation == libk::numeric_limits<u64>::max()
            || slot.leases == libk::numeric_limits<u32>::max()) {
            return libk::unexpected(Error::GenerationExhausted);
        }
        attachment = slot.attachment;
        if (attachment != nullptr
            && attachment->leases == libk::numeric_limits<u32>::max()) {
            return libk::unexpected(Error::GenerationExhausted);
        }
        ready_.pop_front();
        slot.state = TransportState::Claimed;
        ++slot.claim_generation;
        ++claimed_;
        ++slot.leases;
        request = view(slot, index);
        attachment_generation = slot.attachment_generation;
        if (attachment != nullptr) {
            ++attachment->leases;
        }
    }
    bool drained{};
    bool accepted{};
    bool compensate{};
    bool drain{};
    bool signal{};
    if (attachment != nullptr) {
        owner = attachment->transition(
            attachment->context, request, PagerAttachment::Event::Claim);
    } else {
        owner = true;
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_locked(request.key);
        const bool current = slot != nullptr
            && slot->state == TransportState::Claimed
            && slot->claim_generation == request.claim.generation
            && slot->attachment == attachment;
        const bool live = current
            && (state_ == State::Open || state_ == State::Closing)
            && (attachment == nullptr
                || slot->attachment_generation == attachment_generation)
            && (attachment == nullptr || (attachment->state
                    == PagerAttachment::State::Attached
                && attachment->generation == attachment_generation));
        if (owner && live) {
            KASSERT(slot->leases != 0);
            --slot->leases;
            if (attachment != nullptr) {
                KASSERT(attachment->leases != 0);
                --attachment->leases;
            }
            accepted = true;
        } else if (!owner && live) {
            KASSERT(slot->leases != 0);
            --slot->leases;
            if (attachment != nullptr) {
                KASSERT(attachment->leases != 0);
                --attachment->leases;
            }
            KASSERT(claimed_ != 0);
            --claimed_;
            slot->state = TransportState::Queued;
            KASSERT(ready_.try_push_back(request.key.slot));
            signal = notification_ != nullptr;
            if (attachment != nullptr
                && attachment->state == PagerAttachment::State::Retiring
                && attachment->leases == 0) {
                attachment->state = PagerAttachment::State::Detached;
                drained = true;
            }
        } else if (current) {
            compensate = true;
        }
    }
    if (compensate && attachment != nullptr) {
        static_cast<void>(attachment->transition(
            attachment->context, request, PagerAttachment::Event::Forced));
    }
    if (compensate) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            Slot* const slot = find_locked(request.key);
        if (slot != nullptr && slot->state == TransportState::Claimed
            && slot->claim_generation == request.claim.generation
            && slot->attachment == attachment) {
            KASSERT(slot->leases != 0 && claimed_ != 0);
            --slot->leases;
            --claimed_;
            release_claim_locked(*slot);
            slot->state = TransportState::Free;
            slot->attachment = nullptr;
            slot->attachment_generation = 0;
        }
            if (attachment != nullptr) {
                KASSERT(attachment->leases != 0);
                --attachment->leases;
                if (attachment->state == PagerAttachment::State::Retiring
                    && attachment->leases == 0) {
                    attachment->state = PagerAttachment::State::Detached;
                    drained = true;
                }
            }
            drain = state_ == State::Forced;
        }
    }
    if (drained && attachment != nullptr && attachment->drained != nullptr) {
        attachment->drained(attachment->context);
    }
    if (drain) {
        static_cast<void>(close(true));
    }
    if (signal) {
        static_cast<void>(source_link_.signal());
    }
    /*luna change: wake one producer after ready-ring consumption,
      reason: claim frees queue admission only when a transport slot also
      remains available*/
    wake_capacity();
    if (accepted) {
        return libk::expected(request);
    }
    return libk::unexpected(Error::Stale);
}

void Pager::release_claim_locked(Slot& slot) noexcept {
    if (slot.claim_index != nullptr) {
        slot.claim_index->clear(slot.claim_ticket);
        slot.claim_index = nullptr;
        slot.claim_ticket = 0;
    }
}

auto Pager::register_claim(
    ClaimKey key,
    ClaimIndex& index) noexcept -> libk::Expected<void, Error> {
    const usize ticket = index.free_ticket();
    if (ticket == claims_per_execution) {
        return libk::unexpected(Error::Full);
    }
    kernel::sync::IrqLockGuard guard{lock_};
    Slot* const slot = find_claim_locked(key);
    if (slot == nullptr || slot->leases != 0) {
        return libk::unexpected(Error::Stale);
    }
    /*luna change: publish the binding under the Pager lock, reason: a forced
      close in the claim window must never leave a registered stale entry*/
    slot->claim_index = &index;
    slot->claim_ticket = static_cast<u16>(ticket);
    index.entries[ticket] = ClaimIndex::Entry{
        this,
        static_cast<u16>(slot - slots_),
        slot->generation,
        slot->claim_generation};
    return libk::expected();
}

auto Pager::invalidate_claim(
    const ClaimIndex::Entry& entry) noexcept -> bool {
    if (entry.pager != this || entry.slot >= max_requests
        || entry.delivery_generation == 0 || entry.claim_generation == 0) {
        return false;
    }
    /*luna change: reuse the exact requeue owner edge from the terminal walk,
      reason: a writeback obligation must return to Published through the
      attachment event and the next claim must advance the generation*/
    const ClaimKey key{
        .delivery = RequestKey{entry.slot, entry.delivery_generation},
        .generation = entry.claim_generation};
    const auto released = requeue(key, nullptr, nullptr);
    KASSERT(released || released.error() != Error::Busy);
    return static_cast<bool>(released);
}

auto Pager::begin_reply(ClaimKey key) noexcept
    -> libk::Expected<Reply, Error> {
    kernel::sync::IrqLockGuard guard{lock_};
    Slot* const slot = find_claim_locked(key);
    if (slot == nullptr || slot->leases != 0) {
        return libk::unexpected(Error::Stale);
    }
    if (slot->leases == libk::numeric_limits<u32>::max()) {
        return libk::unexpected(Error::GenerationExhausted);
    }
    slot->state = TransportState::Completing;
    ++slot->leases;
    if (slot->attachment != nullptr) {
        if (slot->attachment->leases == libk::numeric_limits<u32>::max()) {
            --slot->leases;
            slot->state = TransportState::Claimed;
            return libk::unexpected(Error::GenerationExhausted);
        }
        ++slot->attachment->leases;
    }
    return libk::expected(Reply{
        *this, view(*slot, key.delivery.slot), slot->attachment});
}

auto Pager::bind(
    ipc::Notification& notification,
    u64 badge) noexcept -> libk::Expected<void, Error> {
    if (badge == 0) {
        return libk::unexpected(Error::InvalidRange);
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open || notification_ != nullptr) {
            return libk::unexpected(Error::Busy);
        }
    }
    if (!notification.bind(source_link_, badge)) {
        return libk::unexpected(Error::Busy);
    }
    bool publish_ready{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open || notification_ != nullptr) {
            source_link_.reset();
            return libk::unexpected(Error::Busy);
        }
        notification_ = &notification;
        badge_ = badge;
        publish_ready = !ready_.empty();
    }
    if (publish_ready) {
        static_cast<void>(source_link_.signal());
    }
    return libk::expected();
}

auto Pager::unbind() noexcept -> bool {
    bool detached{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        detached = notification_ != nullptr;
        notification_ = nullptr;
        badge_ = 0;
    }
    if (detached) {
        source_link_.reset();
    }
    return detached;
}

void Pager::notification_closed() noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    notification_ = nullptr;
    badge_ = 0;
}

auto Pager::cancel(RequestKey key) noexcept
    -> libk::Expected<void, Error> {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_locked(key);
        if (slot == nullptr) {
            return libk::unexpected(Error::Stale);
        }
        if (slot->state != TransportState::Queued) {
            return libk::unexpected(Error::Busy);
        }
        bool found{};
        for (auto it = ready_.begin(); it != ready_.end(); ++it) {
            if (*it == key.slot) {
                ready_.erase(it);
                found = true;
                break;
            }
        }
        if (!found) {
            return libk::unexpected(Error::Busy);
        }
        slot->state = TransportState::Free;
        slot->attachment = nullptr;
        slot->attachment_generation = 0;
    }
    /*luna change: wake a producer after queued cancellation frees a slot,
      reason: capacity release must rearm retained owner work exactly once*/
    wake_capacity();
    return libk::expected();
}

auto Pager::requeue(
    ClaimKey key,
    const Request& request,
    PagerAttachment* expected_attachment) noexcept
    -> libk::Expected<void, Error> {
    return requeue(key, &request, expected_attachment);
}

auto Pager::requeue(
    ClaimKey key,
    const Request* expected,
    PagerAttachment* expected_attachment) noexcept
    -> libk::Expected<void, Error> {
    PagerAttachment* attachment{};
    Request request{};
    bool signal{};
    bool drained{};
    u64 attachment_generation{};
    bool owner{};
    bool accepted{};
    bool compensate{};
    bool drain{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_claim_locked(key);
        if (slot == nullptr) {
            return libk::unexpected(Error::Stale);
        }
        if (ready_.full()) {
            return libk::unexpected(Error::Full);
        }
        attachment = slot->attachment;
        attachment_generation = slot->attachment_generation;
        request = view(*slot, key.delivery.slot);
        if ((expected != nullptr
                && (request != *expected
                    || (expected_attachment != nullptr
                        && slot->attachment != expected_attachment)))
            || (expected == nullptr && expected_attachment != nullptr)) {
            return libk::unexpected(Error::Stale);
        }
        if (slot->leases != 0) {
            return libk::unexpected(Error::Busy);
        }
        KASSERT(slot->leases != libk::numeric_limits<u32>::max());
        ++slot->leases;
        if (attachment != nullptr) {
            if (attachment->leases == libk::numeric_limits<u32>::max()) {
                --slot->leases;
                return libk::unexpected(Error::GenerationExhausted);
            }
            ++attachment->leases;
        }
    }
    if (attachment != nullptr) {
        owner = attachment->transition(
            attachment->context, request, PagerAttachment::Event::Requeue);
    } else {
        owner = true;
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_locked(key.delivery);
        const bool current = slot != nullptr
            && slot->state == TransportState::Claimed
            && slot->claim_generation == key.generation
            && slot->attachment == attachment;
        const bool live = current && state_ != State::Forced
            && state_ != State::Closed
            && (attachment == nullptr
                || slot->attachment_generation == attachment_generation)
            && (attachment == nullptr || (attachment->state
                    == PagerAttachment::State::Attached
                && attachment->generation == attachment_generation));
        if (owner && live) {
            KASSERT(slot->leases != 0);
            --slot->leases;
            release_claim_locked(*slot);
            slot->state = TransportState::Queued;
            KASSERT(claimed_ != 0);
            --claimed_;
            if (attachment != nullptr && attachment->leases != 0) {
                --attachment->leases;
                if (attachment->state == PagerAttachment::State::Retiring
                    && attachment->leases == 0) {
                    attachment->state = PagerAttachment::State::Detached;
                    drained = true;
                }
            }
            KASSERT(ready_.try_push_back(key.delivery.slot));
            signal = notification_ != nullptr;
            accepted = true;
        } else if (current && !live) {
            compensate = true;
        } else if (current) {
            KASSERT(slot->leases != 0);
            --slot->leases;
            if (attachment != nullptr) {
                KASSERT(attachment->leases != 0);
                --attachment->leases;
            }
            if (attachment != nullptr
                && attachment->state == PagerAttachment::State::Retiring
                && attachment->leases == 0) {
                attachment->state = PagerAttachment::State::Detached;
                drained = true;
            }
        }
        drain = state_ == State::Forced;
    }
    if (compensate && attachment != nullptr) {
        static_cast<void>(attachment->transition(
            attachment->context, request, PagerAttachment::Event::Forced));
    }
    if (compensate) {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_locked(key.delivery);
        if (slot != nullptr && slot->state == TransportState::Claimed
            && slot->claim_generation == key.generation
            && slot->attachment == attachment) {
            KASSERT(slot->leases != 0 && claimed_ != 0);
            --slot->leases;
            --claimed_;
            release_claim_locked(*slot);
            slot->state = TransportState::Free;
            slot->attachment = nullptr;
            slot->attachment_generation = 0;
        }
        if (attachment != nullptr) {
            KASSERT(attachment->leases != 0);
            --attachment->leases;
            if (attachment->state == PagerAttachment::State::Retiring
                && attachment->leases == 0) {
                attachment->state = PagerAttachment::State::Detached;
                drained = true;
            }
        }
        drain = state_ == State::Forced;
    }
    if (drained && attachment != nullptr && attachment->drained != nullptr) {
        attachment->drained(attachment->context);
    }
    if (drain) {
        static_cast<void>(close(true));
    }
    if (signal) {
        static_cast<void>(source_link_.signal());
    }
    /*luna change: cover requeue compensation slot release, reason:
      a forced owner transition can free transport capacity while Open*/
    wake_capacity();
    return accepted ? libk::Expected<void, Error>{libk::expected()}
                    : libk::Expected<void, Error>{
                          libk::unexpected(Error::Stale)};
}

auto Pager::finish_locked(
    Slot& slot,
    PagerAttachment*& attachment,
    Request& request,
    bool& drained) noexcept -> bool {
    KASSERT(slot.state == TransportState::Completing);
    attachment = slot.attachment;
    request = view(slot, static_cast<u16>(&slot - slots_));
    drained = false;
    release_claim_locked(slot);
    slot.state = TransportState::Free;
    if (slot.leases != 0) {
        --slot.leases;
    }
    if (slot.attachment != nullptr && slot.attachment->leases != 0) {
        --slot.attachment->leases;
        if (slot.attachment->state == PagerAttachment::State::Retiring
            && slot.attachment->leases == 0) {
            slot.attachment->state = PagerAttachment::State::Detached;
            drained = true;
        }
    }
    slot.attachment = nullptr;
    slot.attachment_generation = 0;
    KASSERT(claimed_ != 0);
    --claimed_;
    return (state_ == State::Closing || state_ == State::Forced)
        && ready_.empty() && claimed_ == 0;
}

auto Pager::finish_reply(Reply& reply) noexcept
    -> libk::Expected<void, Error> {
    bool close{};
    PagerAttachment* attachment{};
    Request request{};
    bool drained{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_locked(reply.key_.delivery);
        if (slot == nullptr) {
            return libk::unexpected(Error::InvalidKey);
        }
        if (slot->state != TransportState::Completing
            || slot->claim_generation != reply.key_.generation
            || slot->leases == 0) {
            return libk::unexpected(Error::Stale);
        }
        if (reply.attachment_ != slot->attachment
            || (reply.attachment_ != nullptr
                && reply.attachment_->state
                    == PagerAttachment::State::Attached
                && reply.attachment_->generation
                    != slot->attachment_generation)) {
            return libk::unexpected(Error::Stale);
        }
        close = finish_locked(*slot, attachment, request, drained);
        if (close) {
            state_ = State::Closed;
        }
    }
    if (drained && attachment != nullptr && attachment->drained != nullptr) {
        attachment->drained(attachment->context);
    }
    /*luna change: wake one producer after reply commit frees the slot,
      reason: Pager capacity release is the exact retry edge for Full*/
    wake_capacity();
    /*luna change: complete Pager cleanup after all Pager accesses,
      reason: cleanup may release the owning object and no callback may touch
      this transport afterward*/
    if (close && cleanup_) {
        cleanup_.complete();
    }
    return libk::expected();
}

auto Pager::abort_reply(Reply& reply) noexcept
    -> libk::Expected<void, Error> {
    PagerAttachment* attachment{};
    Request request{};
    bool drained{};
    bool terminal{};
    bool close{};
    bool drain{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_locked(reply.key_.delivery);
        if (slot == nullptr || slot->state != TransportState::Completing
            || slot->claim_generation != reply.key_.generation
            || slot->leases == 0 || reply.attachment_ != slot->attachment) {
            return libk::unexpected(Error::Stale);
        }
        const bool attached = slot->attachment == nullptr
            || (slot->attachment->state == PagerAttachment::State::Attached
                && slot->attachment->generation
                    == slot->attachment_generation);
        terminal = state_ == State::Forced || state_ == State::Closed
            || !attached;
        if (!terminal) {
            slot->state = TransportState::Claimed;
            --slot->leases;
            if (slot->attachment != nullptr && slot->attachment->leases != 0) {
                --slot->attachment->leases;
                if (slot->attachment->state == PagerAttachment::State::Retiring
                    && slot->attachment->leases == 0) {
                    slot->attachment->state = PagerAttachment::State::Detached;
                    attachment = slot->attachment;
                    drained = true;
                }
            }
        } else {
            attachment = slot->attachment;
            request = view(*slot, reply.key_.delivery.slot);
        }
    }
    if (terminal) {
        if (attachment != nullptr) {
            static_cast<void>(attachment->transition(
                attachment->context, request, PagerAttachment::Event::Forced));
        }
        {
            kernel::sync::IrqLockGuard guard{lock_};
            Slot* const slot = find_locked(reply.key_.delivery);
            if (slot != nullptr && slot->state == TransportState::Completing
                && slot->claim_generation == reply.key_.generation
                && slot->attachment == attachment) {
                close = finish_locked(*slot, attachment, request, drained);
                if (close) {
                    state_ = State::Closed;
                }
                drain = state_ == State::Forced;
            }
        }
    }
    if (drained && attachment != nullptr && attachment->drained != nullptr) {
        attachment->drained(attachment->context);
    }
    if (drain) {
        static_cast<void>(this->close(true));
    }
    if (terminal && !drain) {
        /*luna change: wake a producer after terminal reply abort frees a slot,
          reason: only Open transport can admit a retained Full publication*/
        wake_capacity();
    }
    /*luna change: defer cleanup until terminal callbacks and capacity access
      finish, reason: ObjectCleanup may release Pager storage immediately*/
    if (!drain && close && cleanup_) {
        cleanup_.complete();
    }
    return libk::expected();
}

auto Pager::close(bool force) noexcept -> bool {
    bool detach_notification{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Closed) {
            /*luna change: clear any stale capacity hook on repeated close,
              reason: Closed transport cannot retain producer admission*/
            while (!capacity_waiters_.empty()) {
                auto it = capacity_waiters_.begin();
                capacity_waiters_.erase(*it);
            }
            return true;
        }
        if (state_ == State::Forced) {
            force = true;
        } else if (force) {
            state_ = State::Forced;
        } else if (state_ == State::Open) {
            state_ = State::Closing;
        }
        /*luna change: unlink all producer waiters when admission closes,
          reason: forced/graceful teardown cannot publish new work or invoke
          callbacks against retiring attachment storage*/
        while (!capacity_waiters_.empty()) {
            auto it = capacity_waiters_.begin();
            capacity_waiters_.erase(*it);
        }
        detach_notification = notification_ != nullptr;
        notification_ = nullptr;
        badge_ = 0;
    }
    if (detach_notification) {
        source_link_.reset();
    }
    if (!force) {
        bool complete{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            complete = ready_.empty() && claimed_ == 0;
            if (complete) {
                state_ = State::Closed;
            }
        }
        if (complete && cleanup_) {
            cleanup_.complete();
        }
        return complete;
    }

    for (;;) {
        PagerAttachment* attachment{};
        Request request{};
        bool removed{};
        bool complete{};
        bool held{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (!ready_.empty()) {
                const u16 index = ready_.front();
                ready_.pop_front();
                Slot& slot = slots_[index];
                attachment = slot.attachment;
                request = view(slot, index);
                if (attachment != nullptr) {
                    KASSERT(attachment->leases
                        != libk::numeric_limits<u32>::max());
                    ++attachment->leases;
                    held = true;
                }
                slot.state = TransportState::Free;
                slot.attachment = nullptr;
                slot.attachment_generation = 0;
                removed = true;
            } else {
                for (Slot& slot : slots_) {
                    if (slot.state != TransportState::Claimed
                        || slot.leases != 0) {
                        continue;
                    }
                    const u16 index = static_cast<u16>(&slot - slots_);
                    attachment = slot.attachment;
                    request = view(slot, index);
                    if (attachment != nullptr) {
                        if (attachment->leases
                                == libk::numeric_limits<u32>::max()) {
                            continue;
                        }
                        ++attachment->leases;
                        held = true;
                    }
                    release_claim_locked(slot);
                    slot.state = TransportState::Free;
                    slot.attachment = nullptr;
                    slot.attachment_generation = 0;
                    KASSERT(claimed_ != 0);
                    --claimed_;
                    removed = true;
                    break;
                }
            }
            if (!removed) {
                complete = ready_.empty() && claimed_ == 0;
                if (complete) {
                    state_ = State::Closed;
                }
            }
        }
        if (removed && attachment != nullptr && attachment->transition != nullptr) {
            attachment->transition(
                attachment->context, request, PagerAttachment::Event::Forced);
        }
        if (held && attachment != nullptr) {
            bool drained{};
            {
                kernel::sync::IrqLockGuard guard{lock_};
                if (attachment->leases != 0) {
                    --attachment->leases;
                }
                if (attachment->state == PagerAttachment::State::Retiring
                    && attachment->leases == 0) {
                    attachment->state = PagerAttachment::State::Detached;
                    drained = true;
                }
            }
            if (drained && attachment->drained != nullptr) {
                attachment->drained(attachment->context);
            }
        }
        if (!removed) {
            if (complete && cleanup_) {
                cleanup_.complete();
            }
            return complete;
        }
    }
}

void Pager::retire(object::ObjectCleanup&& cleanup) noexcept {
    bool complete{};
    bool force{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!cleanup_);
        cleanup_ = libk::move(cleanup);
        complete = state_ == State::Closed;
        force = state_ == State::Forced;
    }
    if (complete) {
        cleanup_.complete();
        return;
    }
    static_cast<void>(close(force));
}

} // namespace kernel::pager
