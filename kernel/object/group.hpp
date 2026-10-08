#pragma once

#include <utility>
#include <task/thread.hpp>
#include <array>
#include <variant>
#include <cap/cspace.hpp>
#include <libk/unique_handle.hpp>

#include <expected>
#include <libk/noncopyable.hpp>
#include <sync.hpp>
#include <object/ref.hpp>
#include <cap/grant.hpp>
#include <mm/pmm.hpp>
#include <resource/sponsorship.hpp>

namespace object {
template <typename T> struct traits;
}

namespace cap {
class Graph;
}

namespace object {

class allocation;

class group final : private libk::noncopyable_nonmovable {
  public:
    enum class phase : u8 {
        open,
        closing,
        revoking,
        stopping,
        reclaiming,
        closed,
    };

    // One accepted creation owns its allocation record before the object is
    // built. Destruction rolls back the record; publication releases admission.
    class Txn {
      public:
        Txn() noexcept = default;
        Txn(Txn&&) noexcept = default;
        auto operator=(Txn&&) noexcept -> Txn& = default;
        explicit operator bool() const noexcept { return bool(h_); }
        auto adopt(cap::Graph&, ref<>&& target, cap::View ceiling) noexcept
            -> std::expected<void, cap::GrantError>;
        auto root(cap::Graph&, cap::View) noexcept -> std::expected<void, cap::GrantError>;
        template <class T, class... Args>
        auto make(pool<T>& storage, resource::Reservation charge, Args&&... args) noexcept {
            using Err = typename decltype(storage.create(std::move(charge),
                std::forward<Args>(args)...))::error_type;
            using Result = std::expected<ref<T>, Err>;
            auto made = storage.create(std::move(charge), std::forward<Args>(args)...);
            if (!made) return Result{std::unexpected(made.error())};
            auto value = std::move(*made).publish();
            auto target = value.erase();
            libk_assert(target);
            own(std::move(*target));
            return Result{std::move(value)};
        }
        auto acquire() const noexcept -> std::expected<cap::GrantLease, cap::GrantError>;
        using Error = std::variant<resource::errc, cap::GrantError, cap::CSpaceError>;
        template <usize N, class Bind>
        auto publish(cap::CSpace& space, const std::array<cap::View, N>& views, Bind&& bind) noexcept
            -> std::expected<std::array<cap::Handle, N>, Error> {
            static_assert(N > 0);
            auto root = acquire();
            if (!root) return std::unexpected(root.error());
            std::array<cap::CSpace::NewCap, N> caps;
            std::array<cap::Handle, N> handles;
            for (usize i = 0; i < N; ++i) {
                auto slot = space.reserve();
                if (!slot) return std::unexpected(slot.error());
                auto grant = derive(*root, views[i]);
                if (!grant) return std::unexpected(grant.error());
                if (!bind(*grant, i)) return std::unexpected(cap::CSpaceError::InvalidState);
                auto cap = space.prepare(std::move(*slot), std::move(*grant), views[i]);
                if (!cap) return std::unexpected(cap.error());
                handles[i] = cap->handle();
                caps[i] = std::move(*cap);
            }
            auto inserted = space.insert(std::span{caps});
            if (!inserted) return std::unexpected(inserted.error());
            commit();
            return handles;
        }
        template <usize N>
        auto publish(cap::CSpace& space, const std::array<cap::View, N>& views) noexcept
            -> std::expected<std::array<cap::Handle, N>, Error> {
            return publish(space, views, [](auto&, usize) { return true; });
        }
        void commit() noexcept;
        void reset() noexcept { h_.reset(); }

      private:
        friend class group;
        void own(ref<>&&) noexcept;
        auto derive(cap::GrantLease&, cap::View) noexcept -> std::expected<cap::GrantRef, Error>;
        struct Data {
            group* owner{};
            ref<> self{};
            allocation* item{};
            resource::Reservation fee{};
            cap::GrantRef root{};
            static auto empty() noexcept -> Data { return {}; }
            static bool is_empty(const Data& d) noexcept { return !d.owner; }
        };
        struct Drop {
            void operator()(Data& d) const noexcept;
        };
        libk::unique_handle<Data, Drop, Data> h_{};
    };

    group(mm::Pmm& pmm, resource::budget limit) noexcept;
    ~group() noexcept;

    [[nodiscard]] auto reserve(ref<> self, resource::budget charge) noexcept
        -> std::expected<resource::Reservation, resource::errc>;
    [[nodiscard]] auto begin(ref<> self) noexcept -> std::expected<Txn, resource::errc>;

    [[nodiscard]] static auto allocation_charge() noexcept -> resource::budget;

    [[nodiscard]] auto limit() const noexcept -> resource::budget;
    [[nodiscard]] auto available() const noexcept -> resource::budget;
    [[nodiscard]] auto sponsorship_count() const noexcept -> usize;
    [[nodiscard]] auto state() const noexcept -> phase;
    [[nodiscard]] auto close() noexcept -> phase;
    [[nodiscard]] auto observe_refund(const ref<>& self, resource::RefundNotifier notifier) noexcept -> bool;
    [[nodiscard]] auto can_retire() const noexcept -> bool;

  private:
    friend struct traits<group>;
    friend class resource::Reservation;
    friend class resource::Refund;
    friend class resource::Charge;
    friend class resource::Sponsorship;
    friend class allocation;
    friend class cap::Graph;

    void bind_sponsor(resource::Sponsorship& sponsor) noexcept {
        libk_assert(sponsor_ == nullptr);
        sponsor_ = &sponsor;
    }

    [[nodiscard]] auto reserve(const resource::Sponsorship& parent, resource::budget charge) noexcept
        -> std::expected<resource::Reservation, resource::errc>;
    void cancel(resource::budget amount) noexcept;
    void commit() noexcept;
    void finish() noexcept;
    void commit(allocation& allocation) noexcept;
    void ready(allocation& allocation) noexcept;
    void target_ready(allocation& allocation) noexcept;
    void child_closed(allocation& allocation) noexcept;
    void close_allocation(allocation& allocation) noexcept;
    void bind_parent(allocation& allocation) noexcept;
    void unbind_parent(allocation& allocation) noexcept;
    void attach(allocation& allocation) noexcept;
    void detach(allocation& allocation) noexcept;
    void attach(resource::Sponsorship& sponsorship) noexcept;
    void detach(resource::Sponsorship& sponsorship) noexcept;
    void refund(resource::budget amount) noexcept;
    void service() noexcept;
    [[nodiscard]] auto closed_locked() const noexcept -> bool;

    mutable sync::Spin lock_{};
    resource::Sponsorship* sponsor_{};
    resource::account account_;
    mm::Slab<allocation, false> allocations_;
    phase state_{phase::open};
    allocation* roots_{};
    usize root_count_{};
    resource::Sponsorship* sponsorships_{};
    usize sponsorship_count_{};
    usize construction_count_{};
    bool servicing_{};
    allocation* parent_{};
    bool parent_notified_{};
};

// Owned by an allocation root, never by ordinary derived grants. The root
// sponsorship covers both slots; this relation is used to revoke
// all derived capabilities and retire the target during strong close.
class allocation final : private libk::noncopyable_nonmovable {
    enum class phase : u8 {
        empty,
        pending,
        live,
        revoking,
        revoked,
        stopping,
        stopped,
        retiring,
    };

  public:
    allocation() noexcept;
    ~allocation() noexcept;

  private:
    friend class group;
    friend class cap::Graph;
    friend class group::Txn;

    void commit() noexcept;
    void abort() noexcept;
    void stop() noexcept;
    void retire() noexcept;
    void ready() noexcept;
    void target_ready() noexcept;
    void child_closed() noexcept;

    group* owner_{};
    cap::Graph* graph_{};
    cap::GrantKey root_{};
    ref<> target_{};
    cap::GrantRevoke revoke_{};
    Stop stop_;
    allocation* previous_{};
    allocation* next_{};
    allocation* revoke_retry_next_{};
    phase phase_{phase::empty};
    bool independent_close_{};
};

} // namespace object
