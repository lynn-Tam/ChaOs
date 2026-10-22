#pragma once

#include <libk/types.hpp>
#include <resource/budget.hpp>
#include <expected>
#include <array>
#include <libk/scope_guard.hpp>
#include <uapi/abi.h>
#include <libk/noncopyable.hpp>
#include <object/ref.hpp>
#include <wait.hpp>



namespace resource {

class Quota;
class Allocation;
class Usage;

// An empty quota denotes internal storage without quota usage. A nonempty quota must
// name an open Quota; the returned usage owns its own structural reference.
[[nodiscard]] auto acquire(const object::ref<>& quota, Budget amount) noexcept
    -> std::expected<Usage, errc>;

// A reservation of capacity before commit. It also owns the structural reference that
// keeps the source quota alive until that capacity is returned.
class Reservation final : private libk::noncopyable {
public:
    Reservation() noexcept = default;
    Reservation(Reservation&& other) noexcept;
    auto operator=(Reservation&& other) noexcept -> Reservation&;
    ~Reservation() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

    [[nodiscard]] auto usage() const noexcept -> Budget { return usage_; }
    [[nodiscard]] auto quota() const noexcept -> const object::ref<>& { return ref_; }
    // Commits this deduction without registering a new object allocation. The
    // returned token must follow the concrete reusable resource (a page,
    // stack lease, queue cell, ...), and returns capacity only when that resource is
    // actually available again.
    [[nodiscard]] auto commit() && noexcept -> Usage;
    void reset() noexcept;

private:
    friend class Quota;
    friend class Allocation;

    Quota* owner_{};
    object::ref<> ref_{};
    Budget usage_{};
};

// Deferred release token. Allocation is detached before its object storage slot
// becomes reusable; this token releases the usage only after the actual slot
// and backing capacity have crossed that reuse boundary.
class Release final : private libk::noncopyable {
public:
    Release() noexcept = default;
    Release(Release&& other) noexcept;
    auto operator=(Release&& other) noexcept -> Release&;
    ~Release() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

    void complete() noexcept;

private:
    friend class Allocation;
    friend class Quota;
    Quota* owner_{};
    object::ref<> ref_{};
    Budget usage_{};
};

// A movable claim for capacity already deducted from a Quota.  Usage
// is paired with the real resource owner and may be split or merged as that
// ownership moves between containers.  Destroying it returns capacity, so it
// must be destroyed only after the represented resource becomes reusable.
class Usage final : private libk::noncopyable {
public:
    Usage() noexcept = default;
    Usage(Usage&& other) noexcept;
    auto operator=(Usage&& other) noexcept -> Usage&;
    ~Usage() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }
    [[nodiscard]] auto amount() const noexcept -> Budget { return amount_; }
    auto quota() const noexcept -> const object::ref<>& { return ref_; }

    [[nodiscard]] auto split(Budget part) noexcept -> Usage;
    void merge(Usage&& other) noexcept;
    // The destination quota has reserved all of the paused owner's usage. Publication
    // swaps only prepared refs; Allocation transfers the aggregate commitment.
    void reassign(const Usage& reserved, object::ref<>& quota) noexcept {
        libk_assert(owner_ && reserved.owner_ && reserved.amount_.contains(amount_)
            && quota && quota.get() == reserved.owner_);
        owner_ = reserved.owner_; std::swap(ref_, quota);
    }
    void reset() noexcept;

private:
    friend class Reservation;
    friend class Allocation;
    friend class Quota;
    friend class object::Control;
    Quota* owner_{};
    object::ref<> ref_{};
    Budget amount_{};
};

// Object storage usage only; lifecycle membership is independent.
class Allocation final : private libk::noncopyable_nonmovable {
    friend class object::Scope;
    friend class object::Control;
public:
    Allocation() noexcept = default;
    ~Allocation() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

    [[nodiscard]] auto usage() const noexcept -> Budget { return usage_; }
    [[nodiscard]] auto quota() const noexcept -> const object::ref<>& { return ref_; }
    // Mints a child usage from the same canonical pool. The returned
    // reservation owns a new structural hold; this attachment remains the
    // lineage fact but is not borrowed by the child allocation.
    [[nodiscard]] auto reserve(Budget usage) const noexcept
        -> std::expected<Reservation, errc>;
    // Creates independently movable resource usage.  Unlike Allocation, 
    // Usage is not a new allocation root; it follows physical ownership and
    // keeps the source quota alive until its resource is really reusable.
    [[nodiscard]] auto acquire(Budget usage) const noexcept
        -> std::expected<Usage, errc>;
    void commit(Reservation&& reservation) noexcept;
    void bind_control(object::Control&) noexcept;
    auto epoch() const noexcept -> u64 { sync::Lock guard{object::Control::lock}; return owner_ ? epoch_ : 0; }
    auto pause(const object::ref<>& quota, u64 epoch) noexcept -> status_t;
    void resume() noexcept;
    template<class Commit, class Publish> auto reassign(const object::ref<>& target, const object::ref<>& from,
        const object::ref<>& to, u64 epoch, Usage& replacement, Release& release, Commit&&, Publish&&) noexcept
        -> std::expected<u64, status_t>;
    [[nodiscard]] auto detach() noexcept -> Release;

private:
    friend class Quota;
    Quota* owner_{};
    object::ref<> ref_{};
    Budget usage_{};
    object::Control* control_{};
    u64 epoch_{1};
    bool reassigning_{};
};

// Capacity delegation is independent from Scope membership and Grant trees.
class Quota final : private libk::noncopyable_nonmovable {
public:
    enum class Phase : u8 { Open = OBJECT_OPEN, Closing = OBJECT_CLOSING, Drained = OBJECT_DRAINED };
    explicit Quota(Budget limit, Usage delegation = {}) noexcept;
    auto control() noexcept -> object::Control& { return control_; }
    template<class Transaction> auto reassign_usage(const object::ref<>& to, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        auto next = to.as<Quota>();
        if (!next) return std::unexpected(STATUS_INVALID_CAP);
        {
            sync::Lock guard{lock_};
            if (phase_ == Phase::Drained || finishing_ || reassigning_) return std::unexpected(STATUS_BUSY);
            reassigning_ = true;
        }
        libk::scope_exit resume{[&]() noexcept {
            { sync::Lock guard{lock_}; reassigning_ = false; }
            service();
        }};
        return transaction([&](auto&& visit) { visit(delegation_); }, [] {});
    }
    ~Quota() noexcept;
    auto reserve(object::ref<> self, Budget amount) noexcept -> std::expected<Reservation, errc>;
    template<class Publish> auto resize(const object::ref<>& self, Budget target,
        const object::ref<>& source, Budget ceiling, Publish&& publish) noexcept -> status_t;
    auto info(u64) const noexcept -> QuotaInfo;
    auto limit() const noexcept -> Budget;
    auto available() const noexcept -> Budget;
    auto state() const noexcept -> Phase;
    auto close() noexcept -> Phase;
    void retire(object::cleanup&&) noexcept;
    auto events() noexcept -> Source& { return events_; }
    void bind_work(WorkQueue& work, object::Key key) noexcept { events_.bind_work(work, key); }
private:
    friend class Reservation;
    friend class Release;
    friend class Usage;
    friend class Allocation;
    friend class object::Control;
    object::Control control_;
    void cancel(Budget) noexcept;
    void commit() noexcept;
    void attach(Allocation&) noexcept;
    void detach(Allocation&) noexcept;
    void release(Budget) noexcept;
    void service(bool close = false, object::cleanup cleanup = {}) noexcept;
    void complete_cleanup() noexcept;
    mutable sync::Spin lock_;
    Budget limit_, available_;
    usize pending_{}, allocations_{};
    Phase phase_{Phase::Open};
    bool finishing_{}, reassigning_{};
    Usage delegation_;
    Src<STATE_OPEN | STATE_CLOSING | STATE_DRAINED> events_{Source::Drained::bind<&Quota::complete_cleanup>(*this)};
    usize calls_{};
    object::cleanup cleanup_;
};

// Cold transfer of unused capacity. Allocation and delegation are the sole
// lineage facts; reserve/release retain their existing local cost.
template<class Publish>
auto Quota::resize(const object::ref<>& self, Budget target, const object::ref<>& source,
    Budget ceiling, Publish&& publish) noexcept -> status_t {
    if (self.get() != this) return STATUS_BAD_ARGS;
    auto* allocation = self.allocation();
    object::ref<> parent;
    {
        sync::Lock structure{object::Control::lock};
        if (!allocation || !allocation->owner_) return STATUS_DENIED;
        parent = allocation->ref_;
    }
    // Prepare the possible zero-to-nonzero Usage before the commit locks.
    object::ref<> prepared = parent, released;
    auto& quota = *static_cast<Quota*>(parent.get());
    Source::Publication publication;
    bool changed{};
    libk::scope_exit notify{[&]() noexcept {
        publication.publish();
        if (changed) { quota.service(); service(); object::changed(self.key()); }
    }};
    sync::Lock structure{object::Control::lock};
    if (allocation->ref_.id() != parent.id()) return STATUS_STALE;
    if (allocation->reassigning_) return STATUS_BUSY;
    auto* first = self.id().value < parent.id().value ? this : &quota;
    auto* second = first == this ? &quota : this;
    sync::Lock first_lock{first->lock_};
    sync::Lock second_lock{second->lock_};
    if (reassigning_ || finishing_) return STATUS_BUSY;
    if (phase_ == Phase::Drained) return STATUS_CLOSED;
    libk_assert((limit_.empty() && !delegation_) ||
        (delegation_.owner_ == &quota && delegation_.amount_ == limit_));
    const Budget committed{limit_.memory - available_.memory, limit_.caps - available_.caps};
    if (!target.contains(committed)) return STATUS_BUSY;
    const Budget up{target.memory > limit_.memory ? target.memory - limit_.memory : 0,
        target.caps > limit_.caps ? target.caps - limit_.caps : 0};
    const Budget down{target.memory < limit_.memory ? limit_.memory - target.memory : 0,
        target.caps < limit_.caps ? limit_.caps - target.caps : 0};
    const bool grow = !up.empty();
    if (grow) {
        if (phase_ != Phase::Open || quota.phase_ != Phase::Open) return STATUS_CLOSED;
        if (source.id() != parent.id() || !ceiling.contains(up)) return STATUS_DENIED;
        if (!quota.available_.contains(up)) return STATUS_NO_MEMORY;
    }
    std::array<const object::ref<>*, 2> gates{&self, &parent};
    const auto result = publish(std::span{gates.data(), grow ? 2u : 1u}, [&] {
        quota.available_.memory = quota.available_.memory - up.memory + down.memory;
        quota.available_.caps = quota.available_.caps - up.caps + down.caps;
        available_ = {target.memory - committed.memory, target.caps - committed.caps};
        limit_ = target;
        if (target.empty()) {
            released = std::move(delegation_.ref_);
            delegation_.owner_ = nullptr;
        } else if (!delegation_) {
            delegation_.owner_ = &quota;
            delegation_.ref_ = std::move(prepared);
        }
        delegation_.amount_ = target;
        publication = events_.prepare(static_cast<u32>(phase_) == OBJECT_OPEN ? STATE_OPEN : STATE_CLOSING, true);
        changed = true;
    });
    return result;
}

template<class Commit, class Publish>
auto Allocation::reassign(const object::ref<>& target, const object::ref<>& from, const object::ref<>& to,
    u64 epoch, Usage& replacement, Release& release, Commit&& commit, Publish&& publish) noexcept -> std::expected<u64, status_t> {
    auto old_quota = from.as<Quota>(), new_quota = to.as<Quota>();
    if (!old_quota || !new_quota || from.id() == to.id()) return std::unexpected(STATUS_BAD_ARGS);
    auto& old = old_quota->get();
    auto& next = new_quota->get();
    bool changed{};
    libk::scope_exit notify{[&]() noexcept { if (changed) object::changed(target.key()); }};
    sync::Lock structure{object::Control::lock};
    if (!reassigning_ || owner_ != &old || epoch_ != epoch) return std::unexpected(STATUS_STALE);
    if (epoch_ == std::numeric_limits<u64>::max()) return std::unexpected(STATUS_NO_MEMORY);
    if (control_) {
        auto cycle = next.control_.reaches(*control_);
        if (!cycle) return std::unexpected(STATUS_NO_MEMORY);
        if (*cycle) return std::unexpected(STATUS_DENIED);
    }
    auto* first = from.id().value < to.id().value ? &old : &next;
    auto* second = first == &old ? &next : &old;
    sync::Lock first_lock{first->lock_};
    sync::Lock second_lock{second->lock_};
    if (next.phase_ != Quota::Phase::Open) return std::unexpected(STATUS_CLOSED);
    libk_assert(replacement.owner_ == &next && replacement.amount_.contains(usage_) && !release);
    std::array<const object::ref<>*, 3> gates{&target, &from, &to};
    const auto status = publish(std::span{gates}, [&] {
        // Every replacement reference was prepared before taking Gate locks.
        // Commit only moves actual usage tokens and indices; capacity returns later.
        libk_assert(old.allocations_ && next.allocations_ != std::numeric_limits<usize>::max());
        --old.allocations_; ++next.allocations_;
        release.owner_ = owner_; release.ref_ = std::move(ref_); release.usage_ = replacement.amount_;
        owner_ = replacement.owner_;
        ref_ = std::move(replacement.ref_);
        commit();
        replacement.owner_ = nullptr; replacement.amount_ = {};
        ++epoch_;
    });
    if (status != STATUS_OK) return std::unexpected(status);
    changed = true;
    return epoch_;
}

} // namespace resource
