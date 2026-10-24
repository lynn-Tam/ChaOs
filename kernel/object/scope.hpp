#pragma once
#include <mm/slab.hpp>

#include <utility>
#include <libk/unique_handle.hpp>
#include <expected>
#include <libk/noncopyable.hpp>
#include <sync.hpp>
#include <object/ref.hpp>
#include <cap/grant.hpp>
#include <resource/quota.hpp>
#include <work.hpp>
#include <libk/assert.hpp>
#include <base/slab.hpp>
#include <libk/types.hpp>
#include <libk/delegate.hpp>
#include <limits>
#include <memory>
#include <tuple>
#include <libk/manual_lifetime.hpp>
#include <mm/pmm.hpp>

namespace object {
template <typename T> struct traits;
}

namespace cap {
class Graph;
}

namespace object {

// A rare large ancestor check keeps a DFS stack charged to the calling task.
// Every step releases locks; versioned results are consumed at the actual commit.
class ControlScan {
    struct Frame {
        Frame(ref<> value, Frame* previous) noexcept : value(std::move(value)), previous(previous) {}
        ref<> value;
        Frame* previous;
        usize next{};
    };
    mm::Slab<Frame, false> frames_;
    ref<> quota_, goal_;
    Frame* top_{};
    std::array<Control::Check, 2> checks_{};
    usize selected_{2};
    u64 visit_{};
    status_t status_{STATUS_PENDING};
    void pop() noexcept;
public:
    ControlScan(mm::Pmm& pmm, ref<> quota) noexcept : frames_(pmm, {~usize{}, ~usize{}}), quota_(std::move(quota)) {}
    static auto probe(const ref<>&, const ref<>&) noexcept -> std::expected<Control::Check, status_t>;
    auto check(const ref<>&, const ref<>&, usize) noexcept -> std::expected<Control::Check, status_t>;
    auto drain() noexcept -> bool;
};

class Membership;
class Transfer;

// This is solely a lifecycle index; dependencies and authorization live elsewhere.
class Membership final : private libk::noncopyable_nonmovable {
    enum class Phase : u8 { Empty, Pending, Live, Retiring };
    friend class Scope;
    friend class Control;
    friend void changed(Key) noexcept;
    friend class Transfer;
    template<class> friend class pool;
    ref<Scope> owner_ref_;
    Scope* owner_{};
    Key target_{};
    libk::intrusive_tree_hook hook_;
    auto id() const noexcept -> u64 { return reinterpret_cast<anchor*>(target_.slot)->id_.value; }
    Membership* pending_previous_{};
    Membership* pending_next_{};
    u64 epoch_{1};
    Phase phase_{Phase::Empty};
    bool close_requested_{}, announced_{};
    bool queued_{};
    void dequeue() noexcept;
    void commit() noexcept;
    void abort() noexcept;
    void detach() noexcept;
    auto pin_target() const noexcept -> ref<>;
    auto close() noexcept -> bool;
public:
    Membership() noexcept = default;
    ~Membership() noexcept { libk_assert(!owner_ && !owner_ref_ && phase_ == Phase::Empty); }
};

class Scope final : private libk::noncopyable_nonmovable {
public:
    enum class Phase : u8 { Open = OBJECT_OPEN, Closing = OBJECT_CLOSING, Drained = OBJECT_DRAINED };
    class Admission {
        friend class Scope;
        template<class> friend class pool;
        ref<Scope> owner_;
        pin use_;
        Admission(ref<Scope> owner, pin use) noexcept : owner_(std::move(owner)), use_(std::move(use)) {}
        auto attach(Membership&, Key) noexcept -> bool;
    public:
        Admission() noexcept = default;
        Admission(Admission&&) noexcept = default;
        auto operator=(Admission&&) noexcept -> Admission& = default;
    };
    static constexpr bool fixed_usage = true;
    Scope() noexcept { events_.update(STATE_OPEN); }
    ~Scope() noexcept;
    static auto reserve(ref<Scope>) noexcept -> std::expected<Admission, error>;
    auto control() noexcept -> Control& { return control_; }
    auto events() noexcept -> Source& { return events_; }
    auto close() noexcept -> Phase;
    auto info(u64, u64 kinds = OBJ_KINDS) const noexcept -> ScopeInfo;
    auto inspect(Id source, ScopeCursor, u64 kinds, ScopeMembers&, usize depth = 1) const noexcept -> status_t;
    // ID lookup borrows storage only within this explicit observation scope.
    auto lookup(Id, u64 kinds, usize depth) const noexcept -> ref<>;
    auto describe(Id, u64 kinds, usize depth) const noexcept -> std::expected<ScopeObject, status_t>;
    auto contains(Id, u64 kinds, usize depth) const noexcept -> bool;
    void retire(cleanup&&) noexcept;
    void bind_membership(Membership& member) noexcept { membership_ = &member; }
    void bind_work(WorkQueue& work, Key owner) noexcept { work_ = &work; job_.bind(owner); events_.bind_work(work, owner); }
    static auto adopt(const ref<>& target, ref<Scope> source, ref<Scope> destination, u64 epoch, cap::Graph::Admission admission = {}, const Control::Check* check = nullptr) noexcept
        -> std::expected<u64, status_t>;
private:
    friend class Membership;
    friend class ipc::Port;
    friend void changed(Key) noexcept;
    friend class Transfer;
    friend class Control;
    template<class> friend class pool;
    class Flight {
        Scope& owner_;
    public:
        explicit Flight(Scope& owner) noexcept : owner_(owner) { sync::Lock guard{Control::lock}; ++owner_.calls_; }
        ~Flight() noexcept { owner_.finish_call(true); }
    };
    void changed(bool schedule = true) noexcept;
    void post() noexcept;
    void service() noexcept;
    void complete() noexcept { finish_call(false); }
    void finish_call(bool release) noexcept;
    auto find(Id, u64 kinds, usize depth) const noexcept -> anchor*;
    Control control_;
    Membership* membership_{};
    struct Compare {
        bool operator()(const Membership& a, const Membership& b) const noexcept { return a.id() < b.id(); }
        bool operator()(u64 a, const Membership& b) const noexcept { return a < b.id(); }
        bool operator()(const Membership& a, u64 b) const noexcept { return a.id() < b; }
    };
    libk::intrusive_tree<Membership, &Membership::hook_, Compare> members_;
    std::array<usize, ObjCount> kinds_{};
    Membership* scan_{};
    Membership* pending_{};
    Phase phase_{Phase::Open};
    u64 version_{1};
    bool servicing_{}, dirty_{};
    usize calls_{};
    Src<STATE_OPEN | STATE_CLOSING | STATE_DRAINED> events_{Source::Drained::bind<&Scope::complete>(*this)};
    cleanup cleanup_;
    WorkQueue* work_{};
    Work job_{Work::Fn::bind<&Scope::service>(*this)};
};

// Concrete ownership/quota transaction. The replacement owner reference
// is prepared here; final commit moves it without cloning under Gate locks.
class Transfer final : private libk::noncopyable {
    ref<> target_, source_, destination_;
    ref<Scope> replacement_;
    u64 epoch_{};
    cap::Graph::Admission admission_;
    const Control::Check* control_check_{};
    auto check() const noexcept -> status_t;
    void commit() noexcept;
public:
    Transfer(ref<> target, ref<Scope> source, ref<Scope> destination, u64 epoch,
        cap::Graph::Admission admission = {}, const Control::Check* check = nullptr) noexcept
        : target_(std::move(target)), source_(source), destination_(destination),
          replacement_(std::move(destination)), epoch_(epoch), admission_(admission), control_check_(check) {}
    auto validate() const noexcept -> status_t { sync::Lock guard{Control::lock}; return check(); }
    auto epoch() const noexcept -> u64 { return epoch_; }
    auto adopt() noexcept -> std::expected<u64, status_t>;
    template<class Commit> auto publish(std::span<const ref<>*> dependencies, Commit&& apply) noexcept -> status_t {
        libk_assert(Control::lock.held());
        const auto valid = check();
        if (valid != STATUS_OK) return valid;
        std::array<const ref<>*, 5> gates;
        libk_assert(dependencies.size() <= 3);
        std::copy(dependencies.begin(), dependencies.end(), gates.begin());
        auto count = dependencies.size();
        if (source_) { gates[count++] = &source_; gates[count++] = &destination_; }
        const auto status = admission_.publish(std::span{gates.data(), count}, [&] { apply(); commit(); });
        if (status == STATUS_OK && source_) {
            static_cast<Scope*>(source_.get())->post();
            static_cast<Scope*>(destination_.get())->post();
        }
        return status;
    }
};

// The caller pauses its concrete allocation owner before entering this
// transaction. Scratch and replacement references are paid and prepared
// outside publication locks; every recorded pointer names a real Usage.
template<class Visit, class Commit, class Transaction>
auto reassign(mm::Pmm& pmm, resource::Allocation& allocation, const ref<>& target, const ref<>& from,
    const ref<>& to, u64 epoch, resource::Capacity ceiling, Visit&& visit, Commit&& commit, Transaction& transaction, const Control::Check* check = nullptr) noexcept
    -> std::expected<u64, status_t> {
    if (from.id() == to.id()) return std::unexpected(STATUS_BAD_ARGS);
    const auto paused = allocation.pause(from, epoch);
    if (paused != STATUS_OK) return std::unexpected(paused);
    libk::scope_exit resume{[&]() noexcept { allocation.resume(); }};
    struct Change { resource::Usage* target{}; ref<> quota; Change* next{}; };
    resource::Capacity total = allocation.usage();
    usize count{};
    bool valid = true;
    visit([&](resource::Usage& usage) {
        if (!usage) return;
        const auto memory = libk::checked_add(total.memory, usage.amount().memory);
        const auto caps = libk::checked_add(total.caps, usage.amount().caps);
        if (usage.quota().id() != from.id() || !memory || !caps || count == std::numeric_limits<usize>::max()) {
            valid = false; return;
        }
        total = {*memory, *caps}; ++count;
    });
    if (!valid) return std::unexpected(STATUS_BUSY);
    const auto bytes = libk::checked_multiply(mm::Slab<Change, false>::quota_for(count).pages, mm::page_size);
    const auto peak = bytes ? libk::checked_add(total.memory, *bytes) : std::nullopt;
    if (!peak || !ceiling.contains({*peak, total.caps})) return std::unexpected(STATUS_DENIED);
    auto quota = to.as<resource::Quota>();
    if (!quota) return std::unexpected(STATUS_INVALID_CAP);
    auto reserved = quota->get().reserve(to, total);
    if (!reserved) return std::unexpected(reserved.error() == resource::errc::closed ? STATUS_CLOSED : STATUS_NO_MEMORY);
    auto replacement = std::move(*reserved).commit();
    mm::Slab<Change, false> scratch{pmm};
    Change* head{};
    libk::scope_exit clear{[&]() noexcept {
        while (head) { auto* item = head; head = head->next; scratch.destroy(*item); }
    }};
    for (usize i = 0; i < count; ++i) {
        auto made = scratch.create(to);
        if (!made) return std::unexpected(STATUS_NO_MEMORY);
        (*made)->next = head; head = *made;
    }
    auto* entry = head;
    visit([&](resource::Usage& usage) {
        if (!usage) return;
        libk_assert(entry);
        entry->target = &usage;
        entry->quota = to;
        entry = entry->next;
    });
    libk_assert(!entry);
    resource::Release release;
    auto result = allocation.reassign(target, from, to, epoch, replacement, release, [&] {
        for (auto* change = head; change; change = change->next)
            change->target->reassign(replacement, change->quota);
        commit();
    }, [&](auto gates, auto&& publish) { return transaction.publish(gates, publish); }, check);
    // One aggregate release follows the lock boundary. Scratch holds only old
    // refs after success; failed preparation leaves every owner intact.
    return result;
}

} // namespace object
