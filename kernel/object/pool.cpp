#include <object/pool.hpp>

namespace object {

constinit libk::manual_lifetime<anchor::Index> anchor::ids_;
void anchor::index(bool insert) noexcept {
    sync::Lock guard{Control::lock};
    if (!ids_) (void)ids_.emplace();
    if (insert) ids_->insert(*this);
    else ids_->erase(*this);
}

auto Scope::find(Id id, u64 kinds, usize depth) const noexcept -> anchor* {
    libk_assert(Control::lock.held());
    auto* target = anchor::ids_ ? anchor::ids_->find(id.value) : nullptr;
    if (!target) return nullptr;
    if (reinterpret_cast<usize>(target) == job_.key().slot) return target;
    if (!(kinds & OBJ_BIT(target->kind_))) return nullptr;
    auto* member = target->membership_;
    for (usize level = 0; member && level < depth; ++level) {
        if (member->owner_ == this) return target;
        member = member->owner_ ? member->owner_->membership_ : nullptr;
    }
    return nullptr;
}
auto Scope::lookup(Id id, u64 kinds, usize depth) const noexcept -> ref<> {
    sync::Lock guard{Control::lock};
    auto* target = find(id, kinds, depth);
    return target ? ref<>::retain_alive({reinterpret_cast<usize>(target), target->generation_}) : ref<>{};
}
auto Scope::contains(Id id, u64 kinds, usize depth) const noexcept -> bool {
    sync::Lock guard{Control::lock};
    return find(id, kinds, depth) != nullptr;
}


auto Scope::describe(Id id, u64 kinds, usize depth) const noexcept -> std::expected<ScopeObject, status_t> {
    sync::Lock guard{Control::lock};
    auto* target = find(id, kinds, depth);
    if (!target) return std::unexpected(STATUS_NOT_FOUND);
    ScopeObject out{};
    out.version = 1; out.size = sizeof(out); out.object_id = id.value;
    if (auto* member = target->membership_) {
        out.owner_id = member->owner_ref_.id().value; out.owner_epoch = member->epoch_;
    }
    if (auto* fee = target->ops_->funding(*target)) {
        out.account_id = fee->ref_.id().value; out.billing_epoch = fee->epoch_;
        out.storage_memory = fee->charge_.memory; out.storage_caps = fee->charge_.caps;
    }
    auto clip = [&](u64& endpoint) noexcept {
        if (endpoint && !find(Id{endpoint}, kinds, depth)) { endpoint = 0; out.flags |= OBS_BOUNDARY; }
    };
    clip(out.owner_id); clip(out.account_id);
    out.kind = target->kind_; out.gate = target->gate_.open() ? OBJECT_OPEN : OBJECT_CLOSING;
    return out;
}

// Walk actual parents, rather than indexing every controlled descendant.
// Membership, Funding and the delegation Charge remain the only authorities.
void Control::edges(Control*& pending, u64 epoch) noexcept {
    if (!owner_) return;
    if (auto* member = owner_->membership_; member && member->owner_)
        push(pending, &member->owner_->control_, epoch);
    if (auto* fee = owner_->ops_->funding(*owner_); fee && fee->owner_)
        push(pending, &fee->owner_->control_, epoch);
    if (owner_->kind_ == ObjAccount) {
        auto& account = *static_cast<resource::Account*>(owner_->ops_->payload(*owner_));
        if (account.delegation_.owner_) push(pending, &account.delegation_.owner_->control_, epoch);
    }
}

Scope::~Scope() noexcept {
    libk_assert(members_.empty() && !scan_ && !pending_ && !servicing_ && !cleanup_ && !calls_);
}

auto Scope::reserve(ref<Scope> owner) noexcept -> std::expected<Admission, error> {
    if (!owner) return std::unexpected(error::invalid_id);
    auto use = owner.admit();
    if (!use) return std::unexpected(error::closed);
    return Admission{std::move(owner), std::move(use)};
}

auto Scope::Admission::attach(Membership& member, Key target) noexcept -> bool {
    if (!owner_ || !use_) return false;
    auto& scope = owner_.get();
    sync::Lock guard{Control::lock};
    if (scope.phase_ != Phase::Open || scope.version_ == std::numeric_limits<u64>::max()) return false;
    auto* anchor = reinterpret_cast<object::anchor*>(target.slot);
    if (auto* control = anchor->ops_->control(*anchor)) {
        auto cycle = scope.control_.reaches(*control);
        if (!cycle || *cycle) return false;
    }
    const auto attached = use_.publish([&] {
        libk_assert(!member.owner_ && member.phase_ == Membership::Phase::Empty);
        member.owner_ = &scope;
        member.owner_ref_ = std::move(owner_);
        member.target_ = target;
        member.phase_ = Membership::Phase::Pending;
        scope.members_.insert(member);
        ++scope.kinds_[anchor->kind_];
    });
    if (attached) scope.changed();
    return attached;
}

auto Scope::info(u64, u64 kinds) const noexcept -> ScopeInfo {
    sync::Lock guard{Control::lock};
    usize count{};
    for (usize kind = 1; kind < ObjCount; ++kind) if (kinds & OBJ_BIT(kind)) count += kinds_[kind];
    return {.version=1, .size=sizeof(ScopeInfo), .members=count, .epoch=version_,
        .state=static_cast<u32>(phase_), .reserved=0};
}

auto Scope::inspect(Id source, ScopeCursor cursor, u64 kinds, ScopeMembers& out, usize depth) const noexcept -> status_t {
    sync::Lock guard{Control::lock};
    if (version_ == std::numeric_limits<u64>::max()
        || ((cursor.source || cursor.version || cursor.last)
            && (cursor.source != source.value || cursor.version != version_))) return STATUS_STALE;
    out = {};
    out.version = 1; out.size = sizeof(out); out.next = {source.value, version_, 0};
    usize level = 1;
    const Membership* member = depth ? members_.minimum() : nullptr;
    auto next = [&](const Membership* current) noexcept -> const Membership* {
        auto* target = reinterpret_cast<anchor*>(current->target_.slot);
        if (target->kind_ == ObjScope && level < depth) {
            auto& child = *static_cast<const Scope*>(target->ops_->payload(*target));
            if (auto* first = child.members_.minimum()) { ++level; return first; }
        }
        while (current) {
            const auto* owner = current->owner_;
            if (auto* sibling = owner->members_.next(*current)) return sibling;
            if (owner == this) return nullptr;
            current = owner->membership_; --level;
        }
        return nullptr;
    };
    if (cursor.last) {
        auto* target = anchor::ids_ ? anchor::ids_->find(cursor.last) : nullptr;
        member = target ? target->membership_ : nullptr;
        auto* ancestor = member;
        level = 1;
        while (ancestor && ancestor->owner_ != this && level <= depth) {
            ancestor = ancestor->owner_ ? ancestor->owner_->membership_ : nullptr; ++level;
        }
        if (!ancestor || level > depth) return STATUS_STALE;
        member = next(member);
    }
    // Real tree DFS; the last examined member resumes even a sparse filter.
    for (usize scanned = 0; member && scanned < SCOPE_MEMBER_PAGE; ++scanned) {
        auto* target = reinterpret_cast<anchor*>(member->target_.slot);
        out.next.last = target->id_.value;
        if (kinds & OBJ_BIT(target->kind_)) {
            out.records[out.count++] = {target->id_.value, member->epoch_, static_cast<u32>(target->kind_),
                !target->gate_.open() || member->phase_ == Membership::Phase::Retiring
                    ? SCOPE_MEMBER_CLOSING : member->phase_ == Membership::Phase::Pending
                    ? SCOPE_MEMBER_PENDING : SCOPE_MEMBER_LIVE};
        }
        member = next(member);
    }
    out.more = member != nullptr;
    if (!out.more) out.next.last = 0;
    return STATUS_OK;
}

auto Scope::close() noexcept -> Phase {
    Flight flight{*this};
    Source::Publication publication;
    {
        sync::Lock guard{Control::lock};
        if (phase_ == Phase::Open) {
            phase_ = Phase::Closing;
            scan_ = members_.minimum();
            publication = events_.prepare(STATE_CLOSING, true);
        }
    }
    publication.publish();
    service();
    sync::Lock guard{Control::lock};
    return phase_;
}

void Scope::retire(cleanup&& done) noexcept {
    Flight flight{*this};
    { sync::Lock guard{Control::lock}; libk_assert(!cleanup_); cleanup_ = std::move(done); }
    (void)close();
}

auto Membership::close() noexcept -> bool {
    ref<Scope> owner;
    {
        sync::Lock guard{Control::lock};
        if (!owner_) return false;
        close_requested_ = true;
        if (!queued_ && !announced_) {
            queued_ = true;
            pending_next_ = owner_->pending_;
            if (pending_next_) pending_next_->pending_previous_ = this;
            owner_->pending_ = this;
        }
        owner = owner_ref_;
    }
    owner->service();
    return true;
}

void changed(Key key) noexcept {
    if (!key.valid()) return;
    sync::Lock guard{Control::lock};
    auto& target = *reinterpret_cast<anchor*>(key.slot);
    libk_assert(target.generation_ == key.generation);
    auto* member = target.membership_;
    if (member && member->owner_) member->owner_->changed();
}

void Scope::changed(bool schedule) noexcept {
    libk_assert(Control::lock.held());
    auto* scope = this;
    for (usize depth = 0; scope && depth < 64; ++depth) {
        if (scope->version_ != std::numeric_limits<u64>::max()) ++scope->version_;
        scope->dirty_ = true;
        if (schedule && scope->work_) scope->work_->post(scope->job_);
        scope = scope->membership_ ? scope->membership_->owner_ : nullptr;
    }
}

void Scope::post() noexcept {
    libk_assert(Control::lock.held());
    auto* scope = this;
    for (usize depth = 0; scope && depth < 64; ++depth) {
        if (scope->dirty_ && scope->work_) scope->work_->post(scope->job_);
        scope = scope->membership_ ? scope->membership_->owner_ : nullptr;
    }
}

void Scope::service() noexcept {
    Flight flight{*this};
    Source::Publication changed;
    {
        sync::Lock guard{Control::lock};
        if (servicing_) return;
        servicing_ = true;
        if (std::exchange(dirty_, false)) changed = events_.prepare(u64{1} << static_cast<u32>(phase_), true);
    }
    changed.publish();
    for (usize step = 0; step < 8; ++step) {
        ref<> target;
        bool retire{};
        Source::Publication publication;
        bool retiring{};
        {
            sync::Lock guard{Control::lock};
            auto* member = pending_ ? pending_ : scan_;
            if (member) {
                if (scan_ == member) scan_ = members_.next(*member);
                member->dequeue();
                if (!member->announced_ && member->phase_ != Membership::Phase::Retiring)
                    target = member->pin_target();
            }
            if (target) {
                member->announced_ = true;
                retire = member->phase_ == Membership::Phase::Live;
                if (retire) { member->phase_ = Membership::Phase::Retiring; this->changed(); }
            }
            if (!member) {
                servicing_ = false;
                if (phase_ == Phase::Closing && members_.empty()) {
                    phase_ = Phase::Drained;
                    publication = events_.prepare(STATE_DRAINED, true);
                }
                retiring = phase_ == Phase::Drained && bool(cleanup_);
            }
        }
        publication.publish();
        if (!target) {
            if (retiring) events_.close();
            sync::Lock guard{Control::lock};
            if (!servicing_) return;
            continue;
        }
        // Announce close to every pending constructor too; it must fail final
        // publication, then leave its preallocated membership during rollback.
        if (retire) (void)target.retire();
        else (void)target.destroy();
    }
    bool more{};
    {
        sync::Lock guard{Control::lock};
        servicing_ = false;
        more = dirty_ || scan_ || pending_ || (phase_ == Phase::Closing && members_.empty());
    }
    if (more) { libk_assert(work_); work_->post(job_); }
}

void Scope::finish_call(bool release) noexcept {
    cleanup done;
    {
        sync::Lock guard{Control::lock};
        if (release) { libk_assert(calls_); --calls_; }
        if (calls_ || servicing_ || phase_ != Phase::Drained || !cleanup_ || !events_.drained()) return;
        done = std::move(cleanup_);
    }
    if (done) { work_->close(job_); done.complete(); }
}

auto Transfer::check() const noexcept -> status_t {
    libk_assert(Control::lock.held());
    if (!target_) return STATUS_INVALID_CAP;
    if (!source_ && !destination_) return STATUS_OK;
    if (!source_ || !destination_) return STATUS_BAD_ARGS;
    auto* member = target_.membership();
    auto& source = *static_cast<Scope*>(source_.get());
    auto& destination = *static_cast<Scope*>(destination_.get());
    if (!member || member->owner_ != &source || member->epoch_ != epoch_) return STATUS_STALE;
    if (!target_.live() || source.phase_ != Scope::Phase::Open || destination.phase_ != Scope::Phase::Open
        || member->phase_ != Membership::Phase::Live || member->close_requested_) return STATUS_CLOSED;
    if (member->epoch_ == std::numeric_limits<u64>::max()
        || source.version_ == std::numeric_limits<u64>::max()
        || destination.version_ == std::numeric_limits<u64>::max()) return STATUS_NO_MEMORY;
    if (target_.kind() == ObjScope && static_cast<Scope*>(target_.get())->phase_ != Scope::Phase::Open)
        return STATUS_CLOSED;
    if (&source != &destination) if (auto* control = target_.control()) {
        auto cycle = destination.control_.reaches(*control);
        if (!cycle) return STATUS_NO_MEMORY;
        if (*cycle) return STATUS_DENIED;
    }
    return STATUS_OK;
}

void Transfer::commit() noexcept {
    libk_assert(Control::lock.held());
    if (!source_ || source_.id() == destination_.id()) return;
    auto& member = *target_.membership();
    auto& source = *static_cast<Scope*>(source_.get());
    auto& destination = *static_cast<Scope*>(destination_.get());
    source.members_.erase(member);
    --source.kinds_[target_.kind()]; source.changed(false);
    destination.members_.insert(member);
    ++destination.kinds_[target_.kind()];
    member.owner_ = &destination;
    std::swap(member.owner_ref_, replacement_);
    epoch_ = ++member.epoch_;
    destination.changed(false);
}

auto Transfer::adopt() noexcept -> std::expected<u64, status_t> {
    if (!source_ || !destination_) return std::unexpected(STATUS_BAD_ARGS);
    sync::Lock guard{Control::lock};
    std::array<const ref<>*, 1> gates{&target_};
    const auto status = publish(std::span{gates}, [] {});
    return status == STATUS_OK ? std::expected<u64, status_t>{epoch_} : std::unexpected(status);
}

auto Scope::adopt(const ref<>& target, ref<Scope> source, ref<Scope> destination, u64 epoch,
    cap::Graph::Admission authority) noexcept -> std::expected<u64, status_t> {
    Transfer transaction{target, std::move(source), std::move(destination), epoch, authority};
    return transaction.adopt();
}

auto Membership::pin_target() const noexcept -> ref<> {
    auto* anchor = reinterpret_cast<object::anchor*>(target_.slot);
    if (!anchor->ops_->try_retain(anchor->owner_, *anchor, target_.generation)) return {};
    return ref<>{*anchor, target_.generation};
}

void Membership::commit() noexcept {
    ref<Scope> owner;
    bool closing{};
    {
        sync::Lock guard{Control::lock};
        libk_assert(owner_ && phase_ == Phase::Pending);
        phase_ = Phase::Live; owner_->changed();
        closing = close_requested_ || owner_->phase_ != Scope::Phase::Open;
        if (closing) announced_ = false;
        owner = owner_ref_;
    }
    if (closing) (void)close();
    else owner->service();
}

void Membership::abort() noexcept {
    {
        sync::Lock guard{Control::lock};
        libk_assert(owner_ && phase_ == Phase::Pending);
        phase_ = Phase::Live; owner_->changed();
        close_requested_ = true;
        announced_ = false;
    }
    (void)close();
}

void Membership::dequeue() noexcept {
    libk_assert(Control::lock.held());
    if (!queued_) return;
    if (pending_previous_) pending_previous_->pending_next_ = pending_next_;
    else owner_->pending_ = pending_next_;
    if (pending_next_) pending_next_->pending_previous_ = pending_previous_;
    pending_previous_ = pending_next_ = nullptr;
    queued_ = false;
}

void Membership::detach() noexcept {
    ref<Scope> owner;
    {
        sync::Lock guard{Control::lock};
        if (!owner_) return;
        auto& scope = *owner_;
        if (scope.scan_ == this) scope.scan_ = scope.members_.next(*this);
        dequeue();
        scope.members_.erase(*this);
        auto* anchor = reinterpret_cast<object::anchor*>(target_.slot);
        libk_assert(scope.kinds_[anchor->kind_]);
        --scope.kinds_[anchor->kind_];
        scope.changed();
        owner = std::move(owner_ref_);
        owner_ = nullptr; target_ = {};
        phase_ = Phase::Empty; close_requested_ = announced_ = false;
    }
    owner->service();
}

} // namespace object
