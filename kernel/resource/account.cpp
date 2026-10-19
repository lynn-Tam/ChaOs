#include <resource/account.hpp>
#include <limits>

namespace resource {

Reservation::Reservation(Reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), ref_(std::move(other.ref_)),
      charge_(std::exchange(other.charge_, budget{})) {}

auto Reservation::operator=(Reservation&& other) noexcept -> Reservation& {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        ref_ = std::move(other.ref_);
        charge_ = std::exchange(other.charge_, budget{});
    }
    return *this;
}

Reservation::~Reservation() noexcept { reset(); }

Refund::Refund(Refund&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), ref_(std::move(other.ref_)),
      charge_(std::exchange(other.charge_, budget{})) {}

auto Refund::operator=(Refund&& other) noexcept -> Refund& {
    if (this != &other) {
        complete();
        owner_ = std::exchange(other.owner_, nullptr);
        ref_ = std::move(other.ref_);
        charge_ = std::exchange(other.charge_, budget{});
    }
    return *this;
}

Refund::~Refund() noexcept { complete(); }

Charge::Charge(Charge&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), ref_(std::move(other.ref_)),
      amount_(std::exchange(other.amount_, budget{})) {}

auto Charge::operator=(Charge&& other) noexcept -> Charge& {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        ref_ = std::move(other.ref_);
        amount_ = std::exchange(other.amount_, budget{});
    }
    return *this;
}

Charge::~Charge() noexcept { reset(); }

void Reservation::reset() noexcept {
    if (!owner_) { libk_assert(!ref_ && charge_.empty()); return; }
    libk_assert(ref_);
    owner_->cancel(charge_);
    owner_ = nullptr;
    charge_ = {};
    ref_.reset();
}

auto Reservation::commit() && noexcept -> Charge {
    libk_assert(owner_ && ref_ && !charge_.empty());
    owner_->commit();
    Charge result;
    result.owner_ = std::exchange(owner_, nullptr);
    result.ref_ = std::move(ref_);
    result.amount_ = std::exchange(charge_, budget{});
    return result;
}

void Refund::complete() noexcept {
    if (!owner_) { libk_assert(!ref_ && charge_.empty()); return; }
    libk_assert(ref_);
    owner_->refund(charge_);
    owner_ = nullptr;
    charge_ = {};
    ref_.reset();
}

auto Charge::split(budget part) noexcept -> Charge {
    libk_assert(owner_ && ref_ && amount_.contains(part) && !part.empty());
    if (part == amount_) return Charge{std::move(*this)};
    amount_.memory -= part.memory;
    amount_.caps -= part.caps;
    Charge result;
    result.owner_ = owner_;
    result.ref_ = ref_;
    result.amount_ = part;
    return result;
}

void Charge::merge(Charge&& other) noexcept {
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

void Charge::reset() noexcept {
    if (!owner_) { libk_assert(!ref_ && amount_.empty()); return; }
    libk_assert(ref_ && !amount_.empty());
    owner_->refund(amount_);
    owner_ = nullptr;
    amount_ = {};
    ref_.reset();
}

Funding::~Funding() noexcept {
    libk_assert(!owner_ && !ref_ && charge_.empty());
    libk_assert(!control_ && !rebilling_);
}

void Funding::commit(Reservation&& reservation) noexcept {
    libk_assert(!owner_ && !ref_ && reservation && !rebilling_);
    epoch_ = 1;
    owner_ = std::exchange(reservation.owner_, nullptr);
    ref_ = std::move(reservation.ref_);
    charge_ = std::exchange(reservation.charge_, budget{});
    owner_->attach(*this);
}

auto Funding::reserve(budget charge) const noexcept -> std::expected<Reservation, errc> {
    object::ref<> payer;
    Account* owner;
    {
        sync::Lock guard{object::Control::lock};
        if (!owner_ || rebilling_) return std::unexpected(errc::closed);
        payer = ref_; owner = owner_;
    }
    return owner->reserve(std::move(payer), charge);
}

auto acquire(const object::ref<>& payer, budget amount) noexcept
    -> std::expected<Charge, errc> {
    if (!payer) return Charge{};
    auto owner = payer.as<Account>();
    if (!owner) return std::unexpected(errc::invalid);
    auto reserved = owner->get().reserve(payer, amount);
    if (!reserved) return std::unexpected(reserved.error());
    return std::move(*reserved).commit();
}

auto Funding::acquire(budget amount) const noexcept
    -> std::expected<Charge, errc> {
    auto reservation = reserve(amount);
    if (!reservation) return std::unexpected(reservation.error());
    return std::move(*reservation).commit();
}

auto Funding::pause(const object::ref<>& payer, u64 epoch) noexcept -> status_t {
    sync::Lock guard{object::Control::lock};
    if (!owner_ || ref_.id() != payer.id() || epoch_ != epoch) return STATUS_STALE;
    if (rebilling_) return STATUS_BUSY;
    rebilling_ = true;
    return STATUS_OK;
}
void Funding::resume() noexcept {
    sync::Lock guard{object::Control::lock};
    libk_assert(rebilling_);
    rebilling_ = false;
}

void Funding::bind_control(object::Control& target) noexcept {
    sync::Lock guard{object::Control::lock};
    libk_assert(owner_ && !control_);
    control_ = &target;
}

auto Funding::detach() noexcept -> Refund {
    if (!owner_) return {};
    {
        sync::Lock guard{object::Control::lock};
        control_ = nullptr;
    }
    owner_->detach(*this);
    Refund refund;
    refund.owner_ = std::exchange(owner_, nullptr);
    refund.ref_ = std::move(ref_);
    refund.charge_ = std::exchange(charge_, budget{});
    return refund;
}

Account::Account(budget limit, Charge delegation) noexcept
    : limit_(limit), available_(limit), delegation_(std::move(delegation)) {
    events_.update(STATE_OPEN);
}

Account::~Account() noexcept {
    libk_assert(pending_ == 0 && allocations_ == 0 && available_ == limit_ && !cleanup_ && !finishing_ && !billing_ && !calls_);
}

auto Account::reserve(object::ref<> self, budget amount) noexcept -> std::expected<Reservation, errc> {
    auto typed = self.as<Account>();
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
    reserved.charge_ = amount;
    return reserved;
}
auto Account::limit() const noexcept -> budget { sync::Lock guard{lock_}; return limit_; }
auto Account::available() const noexcept -> budget { sync::Lock guard{lock_}; return available_; }
auto Account::state() const noexcept -> Phase { sync::Lock guard{lock_}; return phase_; }
auto Account::info(u64) const noexcept -> AccountInfo {
    sync::Lock guard{lock_};
    return {.version=1, .size=sizeof(AccountInfo), .memory_limit=limit_.memory,
        .memory_available=available_.memory, .cap_limit=limit_.caps, .caps_available=available_.caps,
        .allocations=allocations_, .reservations=pending_, .state=static_cast<u32>(phase_), .reserved=0};
}
void Account::cancel(budget amount) noexcept {
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
void Account::commit() noexcept {
    { sync::Lock guard{lock_}; libk_assert(pending_); --pending_; }
    service();
}
void Account::attach(Funding&) noexcept {
    sync::Lock guard{lock_};
    libk_assert(pending_ && allocations_ != std::numeric_limits<usize>::max());
    --pending_; ++allocations_;
}
void Account::detach(Funding&) noexcept {
    sync::Lock guard{lock_};
    libk_assert(allocations_); --allocations_;
}
void Account::refund(budget amount) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(limit_.memory - available_.memory >= amount.memory
            && limit_.caps - available_.caps >= amount.caps);
        available_.memory += amount.memory;
        available_.caps += amount.caps;
    }
    service();
}
auto Account::close() noexcept -> Phase {
    service(true);
    return state();
}
void Account::retire(object::cleanup&& cleanup) noexcept {
    service(true, std::move(cleanup));
}
void Account::service(bool close, object::cleanup cleanup) noexcept {
    Source::Publication publication;
    Charge delegation;
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
        if (phase_ == Phase::Closing && !finishing_ && !billing_ && pending_ == 0 && available_ == limit_) {
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

void Account::complete_cleanup() noexcept {
    object::cleanup cleanup;
    {
        sync::Lock guard{lock_};
        if (phase_ != Phase::Drained || calls_ || !cleanup_ || !events_.drained()) return;
        cleanup = std::move(cleanup_);
    }
    cleanup.complete(); // No payload access after the retirement ACK.
}

} // namespace resource
