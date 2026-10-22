#include <resource/quota.hpp>
#include <limits>

namespace resource {

Reservation::Reservation(Reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), ref_(std::move(other.ref_)),
      usage_(std::exchange(other.usage_, Budget{})) {}

auto Reservation::operator=(Reservation&& other) noexcept -> Reservation& {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        ref_ = std::move(other.ref_);
        usage_ = std::exchange(other.usage_, Budget{});
    }
    return *this;
}

Reservation::~Reservation() noexcept { reset(); }

Release::Release(Release&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), ref_(std::move(other.ref_)),
      usage_(std::exchange(other.usage_, Budget{})) {}

auto Release::operator=(Release&& other) noexcept -> Release& {
    if (this != &other) {
        complete();
        owner_ = std::exchange(other.owner_, nullptr);
        ref_ = std::move(other.ref_);
        usage_ = std::exchange(other.usage_, Budget{});
    }
    return *this;
}

Release::~Release() noexcept { complete(); }

Usage::Usage(Usage&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), ref_(std::move(other.ref_)),
      amount_(std::exchange(other.amount_, Budget{})) {}

auto Usage::operator=(Usage&& other) noexcept -> Usage& {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        ref_ = std::move(other.ref_);
        amount_ = std::exchange(other.amount_, Budget{});
    }
    return *this;
}

Usage::~Usage() noexcept { reset(); }

void Reservation::reset() noexcept {
    if (!owner_) { libk_assert(!ref_ && usage_.empty()); return; }
    libk_assert(ref_);
    owner_->cancel(usage_);
    owner_ = nullptr;
    usage_ = {};
    ref_.reset();
}

auto Reservation::commit() && noexcept -> Usage {
    libk_assert(owner_ && ref_ && !usage_.empty());
    owner_->commit();
    Usage result;
    result.owner_ = std::exchange(owner_, nullptr);
    result.ref_ = std::move(ref_);
    result.amount_ = std::exchange(usage_, Budget{});
    return result;
}

void Release::complete() noexcept {
    if (!owner_) { libk_assert(!ref_ && usage_.empty()); return; }
    libk_assert(ref_);
    owner_->release(usage_);
    owner_ = nullptr;
    usage_ = {};
    ref_.reset();
}

auto Usage::split(Budget part) noexcept -> Usage {
    libk_assert(owner_ && ref_ && amount_.contains(part) && !part.empty());
    if (part == amount_) return Usage{std::move(*this)};
    amount_.memory -= part.memory;
    amount_.caps -= part.caps;
    Usage result;
    result.owner_ = owner_;
    result.ref_ = ref_;
    result.amount_ = part;
    return result;
}

void Usage::merge(Usage&& other) noexcept {
    if (!other) return;
    if (!*this) { *this = std::move(other); return; }
    libk_assert(owner_ == other.owner_ && ref_.id() == other.ref_.id());
    libk_assert(std::numeric_limits<u64>::max() - amount_.memory >= other.amount_.memory);
    libk_assert(std::numeric_limits<u64>::max() - amount_.caps >= other.amount_.caps);
    amount_.memory += other.amount_.memory;
    amount_.caps += other.amount_.caps;
    other.owner_ = nullptr;
    other.amount_ = {};
    other.ref_.reset();
}

void Usage::reset() noexcept {
    if (!owner_) { libk_assert(!ref_ && amount_.empty()); return; }
    libk_assert(ref_ && !amount_.empty());
    owner_->release(amount_);
    owner_ = nullptr;
    amount_ = {};
    ref_.reset();
}

Allocation::~Allocation() noexcept {
    libk_assert(!owner_ && !ref_ && usage_.empty());
    libk_assert(!control_ && !reassigning_);
}

void Allocation::commit(Reservation&& reservation) noexcept {
    libk_assert(!owner_ && !ref_ && reservation && !reassigning_);
    epoch_ = 1;
    owner_ = std::exchange(reservation.owner_, nullptr);
    ref_ = std::move(reservation.ref_);
    usage_ = std::exchange(reservation.usage_, Budget{});
    owner_->attach(*this);
}

auto Allocation::reserve(Budget usage) const noexcept -> std::expected<Reservation, errc> {
    object::ref<> quota;
    Quota* owner;
    {
        sync::Lock guard{object::Control::lock};
        if (!owner_ || reassigning_) return std::unexpected(errc::closed);
        quota = ref_; owner = owner_;
    }
    return owner->reserve(std::move(quota), usage);
}

auto acquire(const object::ref<>& quota, Budget amount) noexcept
    -> std::expected<Usage, errc> {
    if (!quota) return Usage{};
    auto owner = quota.as<Quota>();
    if (!owner) return std::unexpected(errc::invalid);
    auto reserved = owner->get().reserve(quota, amount);
    if (!reserved) return std::unexpected(reserved.error());
    return std::move(*reserved).commit();
}

auto Allocation::acquire(Budget amount) const noexcept
    -> std::expected<Usage, errc> {
    auto reservation = reserve(amount);
    if (!reservation) return std::unexpected(reservation.error());
    return std::move(*reservation).commit();
}

auto Allocation::pause(const object::ref<>& quota, u64 epoch) noexcept -> status_t {
    sync::Lock guard{object::Control::lock};
    if (!owner_ || ref_.id() != quota.id() || epoch_ != epoch) return STATUS_STALE;
    if (reassigning_) return STATUS_BUSY;
    reassigning_ = true;
    return STATUS_OK;
}
void Allocation::resume() noexcept {
    sync::Lock guard{object::Control::lock};
    libk_assert(reassigning_);
    reassigning_ = false;
}

void Allocation::bind_control(object::Control& target) noexcept {
    sync::Lock guard{object::Control::lock};
    libk_assert(owner_ && !control_);
    control_ = &target;
    object::Control::changed();
}

auto Allocation::detach() noexcept -> Release {
    if (!owner_) return {};
    {
        sync::Lock guard{object::Control::lock};
        control_ = nullptr;
    }
    owner_->detach(*this);
    Release release;
    release.owner_ = std::exchange(owner_, nullptr);
    release.ref_ = std::move(ref_);
    release.usage_ = std::exchange(usage_, Budget{});
    return release;
}

Quota::Quota(Budget limit, Usage delegation) noexcept
    : limit_(limit), available_(limit), delegation_(std::move(delegation)) {
    events_.update(STATE_OPEN);
}

Quota::~Quota() noexcept {
    libk_assert(pending_ == 0 && allocations_ == 0 && available_ == limit_ && !cleanup_ && !finishing_ && !reassigning_ && !calls_);
}

auto Quota::reserve(object::ref<> self, Budget amount) noexcept -> std::expected<Reservation, errc> {
    auto typed = self.as<Quota>();
    if (!typed || &typed->get() != this || amount.empty()) return std::unexpected(errc::invalid);
    {
        sync::Lock guard{lock_};
        if (phase_ != Phase::Open) return std::unexpected(errc::closed);
        if (!available_.contains(amount) || pending_ == std::numeric_limits<usize>::max())
            return std::unexpected(errc::exhausted);
        if (!self.publish([&] {
            available_.memory -= amount.memory;
            available_.caps -= amount.caps;
            ++pending_;
        })) return std::unexpected(errc::closed);
    }
    Reservation reserved;
    reserved.owner_ = this;
    reserved.ref_ = std::move(self);
    reserved.usage_ = amount;
    return reserved;
}
auto Quota::limit() const noexcept -> Budget { sync::Lock guard{lock_}; return limit_; }
auto Quota::available() const noexcept -> Budget { sync::Lock guard{lock_}; return available_; }
auto Quota::state() const noexcept -> Phase { sync::Lock guard{lock_}; return phase_; }
auto Quota::info(u64) const noexcept -> QuotaInfo {
    sync::Lock guard{lock_};
    return {.version=1, .size=sizeof(QuotaInfo), .memory_limit=limit_.memory,
        .memory_available=available_.memory, .cap_limit=limit_.caps, .caps_available=available_.caps,
        .allocations=allocations_, .reservations=pending_, .state=static_cast<u32>(phase_), .reserved=0};
}
void Quota::cancel(Budget amount) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(pending_ && limit_.memory - available_.memory >= amount.memory
            && limit_.caps - available_.caps >= amount.caps);
        --pending_;
        available_.memory += amount.memory;
        available_.caps += amount.caps;
    }
    service();
}
void Quota::commit() noexcept {
    { sync::Lock guard{lock_}; libk_assert(pending_); --pending_; }
    service();
}
void Quota::attach(Allocation&) noexcept {
    sync::Lock guard{lock_};
    libk_assert(pending_ && allocations_ != std::numeric_limits<usize>::max());
    --pending_; ++allocations_;
}
void Quota::detach(Allocation&) noexcept {
    sync::Lock guard{lock_};
    libk_assert(allocations_); --allocations_;
}
void Quota::release(Budget amount) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(limit_.memory - available_.memory >= amount.memory
            && limit_.caps - available_.caps >= amount.caps);
        available_.memory += amount.memory;
        available_.caps += amount.caps;
    }
    service();
}
auto Quota::close() noexcept -> Phase {
    service(true);
    return state();
}
void Quota::retire(object::cleanup&& cleanup) noexcept {
    service(true, std::move(cleanup));
}
void Quota::service(bool close, object::cleanup cleanup) noexcept {
    Source::Publication publication;
    Usage delegation;
    bool finish{};
    {
        sync::Lock structure{object::Control::lock};
        sync::Lock guard{lock_};
        ++calls_;
        if (cleanup) { libk_assert(!cleanup_); cleanup_ = std::move(cleanup); }
        if (close && phase_ == Phase::Open) {
            phase_ = Phase::Closing;
            publication = events_.prepare(STATE_CLOSING, true);
        }
        if (phase_ == Phase::Closing && !finishing_ && !reassigning_ && pending_ == 0 && available_ == limit_) {
            finishing_ = finish = true;
            delegation = std::move(delegation_);
        }
    }
    publication.publish();
    if (finish) {
        delegation.reset();
        {
            sync::Lock guard{lock_};
            libk_assert(finishing_ && phase_ == Phase::Closing);
            finishing_ = false;
            phase_ = Phase::Drained;
            publication = events_.prepare(STATE_DRAINED, true);
        }
        publication.publish();
    }
    bool retiring;
    { sync::Lock guard{lock_}; retiring = phase_ == Phase::Drained && bool(cleanup_); }
    // Keep this call alive through close(), including synchronous callbacks.
    if (retiring) events_.close();
    object::cleanup done;
    {
        sync::Lock guard{lock_};
        libk_assert(calls_);
        if (--calls_ == 0 && retiring && events_.drained()) done = std::move(cleanup_);
    }
    if (done) done.complete();
}

void Quota::complete_cleanup() noexcept {
    object::cleanup cleanup;
    {
        sync::Lock guard{lock_};
        if (phase_ != Phase::Drained || calls_ || !cleanup_ || !events_.drained()) return;
        cleanup = std::move(cleanup_);
    }
    cleanup.complete(); // No payload access after the retirement ACK.
}

} // namespace resource
