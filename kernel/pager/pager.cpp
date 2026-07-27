#include <pager/pager.hpp>

#include <libk/limits.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::pager {

Pager::Pager() noexcept
    : source_link_(ipc::NotificationSource::bind<
          Pager,
          &Pager::notification_closed>(*this)) {}

Pager::~Pager() noexcept {
    KASSERT(state_ == State::Open || state_ == State::Closed);
    KASSERT(claimed_ == 0 && !cleanup_);
    KASSERT(notification_ == nullptr && !source_link_.attached());
}

auto Pager::state() const noexcept -> State {
    kernel::sync::IrqLockGuard guard{lock_};
    return state_;
}

auto Pager::pending() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return ready_.size();
}

auto Pager::find_locked(RequestKey key) noexcept -> Slot* {
    if (!key || key.slot >= max_requests) {
        return nullptr;
    }
    Slot& slot = slots_[key.slot];
    return slot.occupied && slot.generation == key.generation
        ? &slot : nullptr;
}

auto Pager::find_page_locked(
    mm::PageKey page_key,
    u64 claim_generation) noexcept -> Slot* {
    if (!page_key || claim_generation == 0) {
        return nullptr;
    }
    for (Slot& slot : slots_) {
        if (slot.occupied && slot.claimed
            && slot.generation == claim_generation
            && slot.request.page_key == page_key) {
            return &slot;
        }
    }
    return nullptr;
}

auto Pager::find_free_locked() noexcept -> Slot* {
    for (Slot& slot : slots_) {
        if (!slot.occupied) {
            return &slot;
        }
    }
    return nullptr;
}

auto Pager::publish(
    mm::PageKey page_key,
    usize first,
    usize count,
    u64 backing_epoch,
    u8 urgency,
    RequestLink link) noexcept -> libk::Expected<Request, Error> {
    if (!page_key || count == 0 || count > max_request_pages
        || first > libk::numeric_limits<usize>::max() - count
        || backing_epoch == 0) {
        return libk::unexpected(Error::InvalidRange);
    }
    Request result{};
    bool signal{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open) {
            return libk::unexpected(Error::Closed);
        }
        Slot* const slot = find_free_locked();
        if (slot == nullptr || ready_.full()) {
            return libk::unexpected(Error::Full);
        }
        if (slot->generation == libk::numeric_limits<u64>::max()) {
            return libk::unexpected(Error::GenerationExhausted);
        }
        ++slot->generation;
        slot->request = Request{
            .key = RequestKey{
                .slot = static_cast<u16>(slot - slots_),
                .generation = slot->generation,
            },
            .page_key = page_key,
            .first = first,
            .count = count,
            .backing_epoch = backing_epoch,
            .urgency = urgency,
        };
        slot->occupied = true;
        slot->claimed = false;
        slot->link = link;
        if (!ready_.try_push_back(slot->request.key.slot)) {
            slot->occupied = false;
            slot->link = {};
            return libk::unexpected(Error::Full);
        }
        result = slot->request;
        signal = notification_ != nullptr;
    }
    if (signal) {
        static_cast<void>(source_link_.signal());
    }
    return libk::expected(result);
}

auto Pager::try_claim() noexcept -> libk::Expected<Request, Error> {
    kernel::sync::IrqLockGuard guard{lock_};
    if (state_ == State::Closed) {
        return libk::unexpected(Error::Closed);
    }
    if (ready_.empty()) {
        return libk::unexpected(Error::Busy);
    }
    const u16 index = ready_.front();
    ready_.pop_front();
    Slot& slot = slots_[index];
    KASSERT(slot.occupied && !slot.claimed);
    slot.claimed = true;
    ++claimed_;
    return libk::expected(slot.request);
}

auto Pager::claimed(RequestKey key) const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (!key || key.slot >= max_requests) {
        return false;
    }
    const Slot& slot = slots_[key.slot];
    return slot.occupied && slot.claimed && slot.generation == key.generation;
}

auto Pager::claimed(
    mm::PageKey page_key,
    u64 claim_generation) const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return const_cast<Pager*>(this)->find_page_locked(
               page_key, claim_generation)
        != nullptr;
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
    bool publish{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open || notification_ != nullptr) {
            source_link_.reset();
            return libk::unexpected(Error::Busy);
        }
        notification_ = &notification;
        badge_ = badge;
        publish = !ready_.empty();
    }
    if (publish) {
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

auto Pager::claim(RequestKey key) noexcept
    -> libk::Expected<Request, Error> {
    kernel::sync::IrqLockGuard guard{lock_};
    Slot* const slot = find_locked(key);
    if (slot == nullptr) {
        return libk::unexpected(Error::InvalidKey);
    }
    if (slot->claimed) {
        return libk::unexpected(Error::Busy);
    }
    // A direct claim is intentionally only for a request still in the ready
    // queue.  It never manufactures a second delivery edge.
    bool found{};
    for (usize index = 0; index < ready_.size(); ++index) {
        if (ready_[index] == key.slot) {
            found = true;
            break;
        }
    }
    if (!found) {
        return libk::unexpected(Error::Busy);
    }
    // The ring has no arbitrary erase; preserve FIFO while removing one key.
    libk::InplaceRing<u16, max_requests> rebuilt{};
    while (!ready_.empty()) {
        const u16 candidate = ready_.front();
        ready_.pop_front();
        if (candidate != key.slot) {
            KASSERT(rebuilt.try_push_back(candidate));
        }
    }
    while (!rebuilt.empty()) {
        KASSERT(ready_.try_push_back(rebuilt.front()));
        rebuilt.pop_front();
    }
    slot->claimed = true;
    ++claimed_;
    return libk::expected(slot->request);
}

auto Pager::requeue(RequestKey key) noexcept
    -> libk::Expected<void, Error> {
    bool signal{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_locked(key);
        if (slot == nullptr) {
            return libk::unexpected(Error::InvalidKey);
        }
        if (!slot->claimed || state_ == State::Closed || ready_.full()) {
            return libk::unexpected(
                state_ == State::Closed ? Error::Closed : Error::Busy);
        }
        slot->claimed = false;
        KASSERT(claimed_ != 0);
        --claimed_;
        KASSERT(ready_.try_push_back(key.slot));
        signal = notification_ != nullptr;
    }
    if (signal) {
        static_cast<void>(source_link_.signal());
    }
    return libk::expected();
}

auto Pager::finish_locked(
    Slot& slot,
    RequestLink& link,
    mm::PageKey& page_key) noexcept -> bool {
    KASSERT(slot.occupied && slot.claimed);
    link = slot.link;
    page_key = slot.request.page_key;
    slot.occupied = false;
    slot.claimed = false;
    slot.link = {};
    KASSERT(claimed_ != 0);
    --claimed_;
    return state_ == State::Closing && ready_.empty() && claimed_ == 0;
}

auto Pager::complete(RequestKey key) noexcept
    -> libk::Expected<void, Error> {
    bool close{};
    RequestLink link{};
    mm::PageKey page_key{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_locked(key);
        if (slot == nullptr) {
            return libk::unexpected(Error::InvalidKey);
        }
        if (!slot->claimed) {
            return libk::unexpected(Error::Busy);
        }
        close = finish_locked(*slot, link, page_key);
        if (close) {
            state_ = State::Closed;
        }
    }
    if (close && cleanup_) {
        cleanup_.complete();
    }
    if (link) {
        link.finish(link.context, page_key, false);
    }
    return libk::expected();
}

auto Pager::complete(
    mm::PageKey page_key,
    u64 claim_generation) noexcept -> libk::Expected<void, Error> {
    RequestKey key{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_page_locked(page_key, claim_generation);
        if (slot == nullptr) {
            return libk::unexpected(Error::InvalidKey);
        }
        key = slot->request.key;
    }
    return complete(key);
}

auto Pager::fail(RequestKey key) noexcept
    -> libk::Expected<void, Error> {
    bool close{};
    RequestLink link{};
    mm::PageKey page_key{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_locked(key);
        if (slot == nullptr) {
            return libk::unexpected(Error::InvalidKey);
        }
        if (!slot->claimed) {
            return libk::unexpected(Error::Busy);
        }
        close = finish_locked(*slot, link, page_key);
        if (close) {
            state_ = State::Closed;
        }
    }
    if (close && cleanup_) {
        cleanup_.complete();
    }
    if (link) {
        link.finish(link.context, page_key, true);
    }
    return libk::expected();
}

auto Pager::fail(
    mm::PageKey page_key,
    u64 claim_generation) noexcept -> libk::Expected<void, Error> {
    RequestKey key{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        Slot* const slot = find_page_locked(page_key, claim_generation);
        if (slot == nullptr) {
            return libk::unexpected(Error::InvalidKey);
        }
        key = slot->request.key;
    }
    return fail(key);
}

void Pager::detach_links(void* context) noexcept {
    if (context == nullptr) {
        return;
    }
    kernel::sync::IrqLockGuard guard{lock_};
    for (Slot& slot : slots_) {
        if (slot.occupied && slot.link.context == context) {
            slot.link = {};
        }
    }
}

auto Pager::close(bool force) noexcept -> bool {
    bool complete{};
    bool detach_notification{};
    RequestLink links[max_requests]{};
    mm::PageKey page_keys[max_requests]{};
    usize callbacks{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Closed) {
            return true;
        }
        state_ = State::Closing;
        detach_notification = notification_ != nullptr;
        notification_ = nullptr;
        badge_ = 0;
        if (force) {
            while (!ready_.empty()) {
                const u16 index = ready_.front();
                ready_.pop_front();
                Slot& slot = slots_[index];
                if (slot.link && callbacks < max_requests) {
                    links[callbacks] = slot.link;
                    page_keys[callbacks] = slot.request.page_key;
                    ++callbacks;
                }
                slot.occupied = false;
                slot.claimed = false;
                slot.link = {};
            }
            for (Slot& slot : slots_) {
                if (!slot.occupied) {
                    continue;
                }
                KASSERT(slot.claimed);
                if (slot.link && callbacks < max_requests) {
                    links[callbacks] = slot.link;
                    page_keys[callbacks] = slot.request.page_key;
                    ++callbacks;
                }
                slot.occupied = false;
                slot.claimed = false;
                slot.link = {};
            }
            claimed_ = 0;
        }
        complete = ready_.empty() && claimed_ == 0;
        if (complete) {
            state_ = State::Closed;
        }
    }
    if (detach_notification) {
        source_link_.reset();
    }
    if (complete && cleanup_) {
        cleanup_.complete();
    }
    for (usize index = 0; index < callbacks; ++index) {
        links[index].finish(links[index].context, page_keys[index], true);
    }
    return complete;
}

void Pager::retire(object::ObjectCleanup&& cleanup) noexcept {
    bool complete{};
    bool detach_notification{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!cleanup_);
        cleanup_ = libk::move(cleanup);
        state_ = State::Closing;
        detach_notification = notification_ != nullptr;
        notification_ = nullptr;
        badge_ = 0;
        complete = ready_.empty() && claimed_ == 0;
        if (complete) {
            state_ = State::Closed;
        }
    }
    if (detach_notification) {
        source_link_.reset();
    }
    if (complete) {
        cleanup_.complete();
    }
}

} // namespace kernel::pager
