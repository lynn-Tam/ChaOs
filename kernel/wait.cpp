#include <wait.hpp>
#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <cpu/cpu.hpp>
#include <task/thread.hpp>
#include <sched/dispatcher.hpp>
#include <sync.hpp>
#include <limits>

auto Wait::begin(Cpus& cpus, Thread& binding) noexcept -> bool {
    sync::Lock guard{lock_};
    if (attached()) return false;
    cpus_ = &cpus; binding_ = &binding;
    phase_.store(Phase::Armed, std::memory_order_release);
    return true;
}

auto Wait::finish() noexcept -> bool {
    object::ref<> storage;
    {
        sync::Lock guard{lock_};
        if (!ready()) return false;
        storage = object::ref<>::retain_alive(binding_->finalizer_.key());
        phase_.store(Phase::Releasing, std::memory_order_release);
    }
    source_.release();
    {
        sync::Lock guard{lock_};
        cpus_ = nullptr; binding_ = nullptr;
        phase_.store(Phase::Idle, std::memory_order_release);
    }
    return true;
}

auto Wait::cancel() noexcept -> bool {
    if (phase_.load(std::memory_order_acquire) == Phase::Armed) source_.cancel();
    return finish();
}

void Wait::wake() noexcept {
    object::ref<> storage;
    Cpus* cpus;
    Thread* binding;
    {
        sync::Lock guard{lock_};
        libk_assert(phase_.load() == Phase::Armed && cpus_ && binding_);
        cpus = cpus_; binding = binding_;
        storage = object::ref<>::retain_alive(binding->finalizer_.key());
        phase_.store(Phase::Ready, std::memory_order_release);
    }
    // The home may finish and reuse the fixed record immediately. Only these
    // local parameters and the real Thread storage ref survive publication.
    sync::Irq interrupts;
    const auto woke = sched::wake(*cpus, *binding);
    libk_assert(woke || binding->state() == Thread::State::Exited);
}

Source::Registration::Registration(Wait& owner) noexcept
    : owner_(owner), deadline_(sched::Deadline::Fn::bind<&Registration::expire>(*this)) {}

void Source::Registration::complete() noexcept { owner_.wake(); }

Source::Registration::~Registration() noexcept {
    libk_assert(!source_ && !drain_ && !release_ && !hook_.is_linked() && !deadline_.armed());
}

auto Source::Registration::deliver(WaitResult result) noexcept -> bool {
    if (!queued_) return false;
    source_->waiters_.erase(*this);
    queued_ = false;
    result_ = result;
    return true;
}

void Source::Registration::cancel() noexcept {
    bool claimed;
    { sync::Lock guard{source_->lock_}; claimed = deliver({STATUS_CANCELED}); }
    if (claimed) publish();
}

void Source::Registration::publish() noexcept {
    if (drain_) drain_();
    else complete();
}

void Source::Registration::revoke() noexcept {
    bool claimed;
    { sync::Lock guard{source_->lock_}; claimed = deliver({STATUS_CLOSED}); }
    if (claimed) publish();
}

void Source::Registration::expire() noexcept {
    bool claimed;
    { sync::Lock guard{source_->lock_}; claimed = deliver({STATUS_TIMED_OUT}); }
    if (claimed) publish();
}

void Source::Registration::release() noexcept {
    drain_ = {};
    const auto release = std::exchange(release_, {});
    if (deadline_.armed()) current_cpu().dispatcher().disarm(deadline_);
    auto* source = std::exchange(source_, nullptr);
    Source::Drained drained;
    {
        sync::Lock guard{source->lock_};
        libk_assert(source->registrations_);
        if (--source->registrations_ == 0 && !source->publishers_ && source->watches_.empty() && source->closed_)
            drained = source->drained_;
    }
    if (drained) drained();
    if (release) release(); // Owner storage is retained until registration actually drains.
}

Source::~Source() noexcept {
    libk_assert(waiters_.empty() && watches_.empty() && !registrations_ && !publishers_ && !scheduled_);
}

auto Source::matches(const Condition& condition) const noexcept -> bool {
    return (bits_ & condition.mask) || (condition.changed && version_ != condition.seen);
}

auto Source::result(const Condition& condition) noexcept -> WaitResult {
    const auto value = bits_ & condition.mask;
    return {STATUS_OK, value, version_};
}

auto Source::snapshot() const noexcept -> WaitResult {
    sync::Lock guard{lock_};
    return {closed_ ? STATUS_CLOSED : STATUS_OK, bits_, version_};
}

auto Source::take(u64 mask) noexcept -> WaitResult {
    sync::Lock guard{lock_};
    if (closed_) return {STATUS_CLOSED};
    if (!(bits_ & mask)) return {STATUS_RETRY, 0, version_};
    const auto ready = result({.mask = mask});
    bits_ &= ~ready.value;
    return ready;
}

auto Source::await(Context context, Condition condition, std::optional<u64> deadline,
    Admission admission, Action release) noexcept -> WaitResult {
    auto& [thread, cpus, dispatcher] = context;
    auto& registration = thread.wait().source_;
    u64 armed;
    {
        sync::Lock guard{lock_};
        if (registration.source_ || thread.waiting()) return {STATUS_BUSY};
        if (closed_) return {STATUS_CLOSED};
        if (matches(condition)) return result(condition);
        armed = version_;
    }
    if (deadline && !dispatcher.arm(registration.deadline_, *deadline)) return {STATUS_BUSY};
    registration.source_ = this;
    Publication publication;
    bool installed{};
    auto commit = [&](Action drain) noexcept {
        sync::Lock guard{lock_};
        // Stop/ready can claim the fixed record as soon as begin_wait makes it
        // visible, so its authority drain and final release must precede that.
        registration.drain_ = drain; registration.release_ = release;
        registration.condition_ = condition;
        registration.armed_at_ = armed; registration.scanned_ = scan_version_;
        registration.result_ = {STATUS_PENDING};
        ++registrations_; registration.queued_ = true;
        waiters_.push_back(registration);
        libk_assert(thread.begin_wait(cpus));
        installed = true;
        if (const auto ready = observed(condition, armed)) {
            ++publishers_; publication = Publication{*this};
            libk_assert(registration.deliver(*ready)); publication.ready_.push_back(registration);
        }
    };
    if (admission) {
        const auto status = admission(Commit::bind(commit), Action::bind<&Registration::revoke>(registration),
                                     Action::bind<&Registration::complete>(registration));
        if (status != STATUS_OK) {
            libk_assert(!installed);
            if (registration.deadline_.armed()) dispatcher.disarm(registration.deadline_);
            registration.source_ = nullptr;
            return {status};
        }
    } else commit({});
    libk_assert(installed);
    return {STATUS_PENDING};
}

auto Source::post(u64 bits) noexcept -> bool { auto publication = prepare(bits); return bool(publication); }
auto Source::update(u64 bits) noexcept -> bool { auto publication = prepare(bits, true); return bool(publication); }

auto Source::prepare(u64 bits, bool replace) noexcept -> Publication {
    sync::Lock guard{lock_};
    if (closed_ || version_ >= std::numeric_limits<u64>::max() - 1) return {};
    ++publishers_;
    bits_ = replace ? bits : bits_ | bits;
    ++version_;
    if (raised_) {
        libk_assert(!(bits_ & ~mask_));
        usize index{};
        for (auto remaining = mask_; remaining; remaining &= remaining - 1, ++index)
            if (bits_ & (u64{1} << std::countr_zero(remaining))) raised_[index] = version_;
    }
    Publication publication{*this};
    const usize limit = work_ && raised_ ? 8 : std::numeric_limits<usize>::max();
    usize scanned{};
    for (auto it = waiters_.begin(); it != waiters_.end() && scanned++ < limit;) {
        auto& waiter = *it++;
        if (!matches(waiter.condition_)) continue;
        libk_assert(waiter.deliver(result(waiter.condition_)));
        publication.ready_.push_back(waiter);
    }
    if (work_ && raised_) {
        publication.deferred_ = !waiters_.empty() || !watches_.empty();
        return publication;
    }
    for (auto& watch : watches_)
        if (watch.armed_ && matches(watch.condition_)) hint(watch, result(watch.condition_), publication);
    return publication;
}

void Source::Publication::publish() noexcept {
    if (!source_) return;
    while (!ready_.empty()) ready_.pop_front().publish();
    for (;;) {
        Watch* watch;
        WaitResult result;
        {
            sync::Lock guard{source_->lock_};
            if (hints_.empty()) break;
            watch = &hints_.pop_front();
            result = watch->pending_;
        }
        watch->deliver_(result);
        watch->release();
    }
    auto* source = std::exchange(source_, nullptr);
    if (deferred_) source->schedule();
    source->release();
}

void Source::close() noexcept {
    Publication publication;
    {
        sync::Lock guard{lock_};
        if (closed_) return;
        closed_ = true;
        closed_at_ = version_++;
        ++publishers_;
        publication = Publication{*this};
        if (work_ && raised_) {
            for (usize n = 0; n < 8 && !waiters_.empty(); ++n) {
                auto& waiter = waiters_.front();
                libk_assert(waiter.deliver(*observed(waiter.condition_, waiter.armed_at_)));
                publication.ready_.push_back(waiter);
            }
            publication.deferred_ = !waiters_.empty() || !watches_.empty();
            return;
        }
        for (auto& waiter : waiters_) {
            waiter.queued_ = false;
            waiter.result_ = *observed(waiter.condition_, waiter.armed_at_);
        }
        publication.ready_ = std::move(waiters_);
        for (auto& watch : watches_) hint(watch, terminal(watch), publication);
    }
}

auto Source::observed(Condition condition, u64 armed) const noexcept -> std::optional<WaitResult> {
    u64 value = bits_ & condition.mask;
    usize index{};
    for (auto remaining = mask_ & condition.mask; remaining; remaining &= remaining - 1) {
        const auto bit = u64{1} << std::countr_zero(remaining);
        index = std::popcount(mask_ & (bit - 1));
        if (raised_[index] > armed) value |= bit;
    }
    if (value || (condition.changed && (closed_ ? closed_at_ : version_) != condition.seen)) return WaitResult{STATUS_OK, value, version_};
    return closed_ ? std::optional{WaitResult{STATUS_CLOSED, 0, version_}} : std::nullopt;
}

auto Source::terminal(const Watch& watch) const noexcept -> WaitResult {
    const auto ready = watch.armed_ ? observed(watch.condition_, watch.armed_at_)->value
        : (watch.state_.load(std::memory_order_acquire) & ~Watch::registered) ? watch.pending_.value : 0;
    return {STATUS_CLOSED, ready, version_};
}

void Source::schedule() noexcept {
    {
        sync::Lock guard{lock_};
        if (scheduled_) return;
        scheduled_ = true; scan_version_ = version_; ++publishers_;
    }
    if (pin_) pin_(true);
    work_->queue_source(*this);
}

auto Source::scan(Pin& unpin) noexcept -> bool {
    Publication publication;
    {
        sync::Lock guard{lock_};
        ++publishers_; publication = Publication{*this};
        for (usize step = 0; step < 8; ++step) {
            if (!waiters_.empty() && waiters_.front().scanned_ != scan_version_) {
                auto& waiter = waiters_.front(); waiter.scanned_ = scan_version_;
                if (const auto result = observed(waiter.condition_, waiter.armed_at_)) {
                    libk_assert(waiter.deliver(*result)); publication.ready_.push_back(waiter);
                } else { (void)waiters_.pop_front(); waiters_.push_back(waiter); }
            } else if (!watches_.empty() && watches_.front().scanned_ != scan_version_) {
                auto& watch = watches_.pop_front(); watch.scanned_ = scan_version_; watches_.push_back(watch);
                if (closed_) hint(watch, terminal(watch), publication);
                else if (watch.armed_) if (const auto result = observed(watch.condition_, watch.armed_at_)) hint(watch, *result, publication);
            } else break;
        }
    }
    publication.publish();
    sync::Lock guard{lock_};
    if ((!waiters_.empty() && waiters_.front().scanned_ != scan_version_)
        || (!watches_.empty() && watches_.front().scanned_ != scan_version_)) return true;
    if (version_ != scan_version_) { scan_version_ = version_; return true; }
    unpin = pin_; scheduled_ = false;
    return false;
}

void WorkQueue::queue_source(Source& source) noexcept {
    std::optional<CpuId> runner;
    {
        sync::Lock guard{lock_};
        *source_tail_ = &source; source_tail_ = &source.next_;
        runner = enqueue_locked(source_job_, {});
    }
    wake(runner);
}

void WorkQueue::source_step() noexcept {
    Source* source;
    {
        sync::Lock guard{lock_}; source = sources_; libk_assert(source);
        sources_ = source->next_; source->next_ = nullptr;
        if (!sources_) source_tail_ = &sources_;
    }
    auto ticket = object::ref<>::retain_alive(source->key_);
    Source::Pin unpin;
    if (source->scan(unpin)) queue_source(*source);
    else {
        source->release(); // No Source access after handing back its last flight.
        if (unpin) unpin(false);
    }
    sync::Lock guard{lock_};
    if (sources_) static_cast<void>(enqueue_locked(source_job_, {}));
}

void Source::release() noexcept {
    Drained drained;
    {
        sync::Lock guard{lock_};
        libk_assert(publishers_);
        if (--publishers_ == 0 && !registrations_ && watches_.empty() && closed_) drained = drained_;
    }
    if (drained) drained();
}

auto Source::drained() const noexcept -> bool {
    sync::Lock guard{lock_};
    return closed_ && !registrations_ && !publishers_ && watches_.empty();
}

void Source::hint(Watch& watch, WaitResult result, Publication& publication) noexcept {
    libk_assert(lock_.held());
    watch.armed_ = false;
    watch.pending_ = result;
    if (watch.publish_hook_.is_linked()) return; // One queued hint per Watch.
    watch.state_.fetch_add(1, std::memory_order_acq_rel);
    publication.hints_.push_back(watch);
}

auto Source::arm(Watch& watch, Condition condition) noexcept -> status_t {
    return arm(watch, condition, [](auto&& commit) noexcept { commit(); return STATUS_OK; });
}

Source::Watch::~Watch() noexcept {
    libk_assert(!state_.load() && !source_ && !hook_.is_linked() && !publish_hook_.is_linked());
}

void Source::Watch::release() noexcept {
    const auto before = state_.fetch_sub(1, std::memory_order_acq_rel);
    libk_assert((before & ~registered) != 0);
    if (before == 1) {
        auto pin = std::exchange(pin_, {});
        drained_();
        if (pin) pin(false);
    }
}

void Source::Watch::detach() noexcept {
    auto* source = source_;
    if (!source) return;
    bool done;
    Pin pin;
    {
        sync::Lock guard{source->lock_};
        ++source->publishers_; // Covers the owner callback and any nested close.
        source->watches_.erase(*this);
        source_ = nullptr;
        armed_ = false;
        done = state_.fetch_and(~registered, std::memory_order_acq_rel) == registered;
        if (done) pin = std::exchange(pin_, {});
    }
    if (done) drained_();
    source->release();
    if (pin) pin(false);
}
