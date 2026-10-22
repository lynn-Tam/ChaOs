#pragma once

#include <utility>
#include <array>

#include <bit>

#include <cpu.hpp>
#include <cap/cap.hpp>
#include <cap/grant.hpp>
#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <expected>
#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <span>
#include <libk/inplace_vector.hpp>
#include <uapi/ipc.h>
#include <sync.hpp>
#include <mm/pmm.hpp>
#include <resource/quota.hpp>

namespace mm {
class VSpace;
}

class Env;

namespace ipc {
class Channel;
}
namespace object {
template<typename T>
struct traits;
}

namespace cap {

class Batch;
enum class XferOp : u8 { Copy, Move, Derive };
using XferLimit = std::variant<Rights, View, Attenuation>;
struct XferSpec final {
    Handle source{};
    XferLimit limit{Rights{}};
    XferOp op{XferOp::Copy};
};

enum class CSpaceError : u8 {
    InvalidHandle,
    InvalidState,
    WrongKind,
    Denied,
    Amplification,
    OutOfMemory,
    SlotQuota,
    PageQuota,
    GenerationExhausted,
    Contended,
    GrantUnavailable,
    ResourceExhausted,
    InvalidDescriptor,
};

class CSpace final : private libk::noncopyable_nonmovable {
public:
    struct Quota final {
        usize slots{4096};
        usize pages{130};
    };

    class Reservation final : private libk::noncopyable {
    public:
        Reservation() noexcept = default;
        Reservation(Reservation&& other) noexcept;
        auto operator=(Reservation&& other) noexcept -> Reservation&;
        ~Reservation() noexcept;

        [[nodiscard]] auto handle() const noexcept -> Handle {
            return handle_;
        }

    private:
        friend class CSpace;
        friend class Batch;
        friend class ipc::Channel;
        Reservation(
            CSpace& owner,
            Handle handle,
            resource::Usage&& usage) noexcept
            : owner_(&owner),
              handle_(handle),
              usage_(std::move(usage)) {}
        void reset() noexcept;
        void disarm() noexcept {
            owner_ = nullptr;
            handle_ = {};
        }

        CSpace* owner_{};
        Handle handle_{};
        resource::Usage usage_{};
    };

    // Validated preparation and reserved storage; publication rechecks admission.
    class NewCap {
    public:
        NewCap() noexcept = default;
        NewCap(NewCap&&) noexcept = default;
        auto operator=(NewCap&&) noexcept -> NewCap& = default;
        auto handle() const noexcept -> Handle { return slot_.handle(); }

    private:
        friend class CSpace;
        NewCap(Reservation&& slot, Capability&& cap) noexcept
            : slot_(std::move(slot)), cap_(std::move(cap)) {}
        Reservation slot_;
        Capability cap_;
    };

    explicit CSpace(mm::Pmm& pmm) noexcept;
    CSpace(mm::Pmm& pmm, Quota quota) noexcept;
    ~CSpace() noexcept;

    [[nodiscard]] auto reserve() noexcept
        -> std::expected<Reservation, CSpaceError>;
    [[nodiscard]] auto insert(
        GrantRef&& grant,
        View view) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto insert(
        Reservation&& reservation,
        GrantRef&& grant,
        View view) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto prepare(Reservation&&, GrantRef&&, View) noexcept
        -> std::expected<NewCap, CSpaceError>;
    // All slots become visible under one lock, or none do. No allocation here.
    [[nodiscard]] auto insert(std::span<NewCap>, Graph::Admission = {},
        std::span<const object::ref<>*> gates = {},
        libk::delegate<void() noexcept> commit = {},
        std::span<const Graph::Attach> relations = {}) noexcept
        -> std::expected<void, CSpaceError>;
    using PublishError = std::variant<resource::errc, GrantError, CSpaceError>;
    template<usize N, class Make, class Bind>
    auto publish(const std::array<View, N>& views, Make&& make, Bind&& bind) noexcept
        -> std::expected<std::array<Handle, N>, PublishError> {
        std::array<NewCap, N> caps;
        std::array<Handle, N> handles;
        for (usize i = 0; i < N; ++i) {
            auto slot = reserve();
            if (!slot) return std::unexpected(slot.error());
            auto grant = make(views[i]);
            if (!grant) return std::unexpected(grant.error());
            if (!bind(*grant, i)) return std::unexpected(CSpaceError::InvalidState);
            auto cap = prepare(std::move(*slot), std::move(*grant), views[i]);
            if (!cap) return std::unexpected(cap.error());
            handles[i] = cap->handle();
            caps[i] = std::move(*cap);
        }
        auto result = insert(std::span{caps});
        if (!result) return std::unexpected(result.error());
        return handles;
    }
    template<usize N, class Make>
    auto publish(const std::array<View, N>& views, Make&& make) noexcept {
        return publish(views, std::forward<Make>(make), [](auto&, usize) { return true; });
    }
    [[nodiscard]] auto close(Handle handle) noexcept
        -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto transfer(Handle, CSpace&, XferOp, XferLimit = Rights{}, resource::Reservation reservation = {}) noexcept
        -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto revoke(Handle, Source::Context* = nullptr, GrantId selected = {}) noexcept -> std::expected<WaitResult, CSpaceError>;
    [[nodiscard]] auto destroy(Handle source) noexcept
        -> std::expected<void, CSpaceError>;

    template<typename T>
    [[nodiscard]] auto resolve(
        Handle handle,
        Rights requested) noexcept -> std::expected<Resolved<T>, CSpaceError> {
        static_assert(std::is_void_v<T> || object::kind<T> != ObjInvalid);
        auto copied = snapshot(handle);
        if (!copied) {
            return std::unexpected(copied.error());
        }
        Snapshot source = std::move(copied).value();
        GrantPin pin = std::move(source.pin);
        if (!std::is_void_v<T> && pin.kind() != object::kind<T>) {
            return std::unexpected(CSpaceError::WrongKind);
        }
        if (!source.view.rights.contains(requested)) {
            return std::unexpected(CSpaceError::Denied);
        }
        if (!pin.target_live()) return std::unexpected(CSpaceError::GrantUnavailable);
        return (Resolved<T>{
            *this,
            std::move(pin),
            source.view, handle, source.token});
    }

    template<class Transaction> auto reassign_usage(const object::ref<>&, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        {
            sync::Lock guard{lock_};
            if (!accepting_ || reassigning_ || growing_ || preparing_ || escrows_ || releasing_)
                return std::unexpected(STATUS_BUSY);
            reassigning_ = true;
        }
        libk::scope_exit resume{[&]() noexcept {
            { sync::Lock guard{lock_}; reassigning_ = false; }
            finish_retire();
        }};
        return transaction([&](auto&& visit) { visit(usage_); }, [] {});
    }
    auto inspect(GrantQuery, usize depth, GrantRecords&) noexcept -> status_t;
    auto observe(Handle seed, GrantId selected, usize depth) noexcept -> std::expected<Graph::Sample, status_t>;
    auto inspect(Handle cursor, CSpaceInfo&, u64 epoch = 0) noexcept -> std::expected<void, CSpaceError>;
    auto inspect(Handle) noexcept -> std::expected<CapInfo, CSpaceError>;
    void bind_work(WorkQueue& work, object::Key owner) noexcept { work_ = &work; job_.bind(owner); }
    void retire() noexcept;
    void retire(object::cleanup&&) noexcept;
    [[nodiscard]] auto binding_count() const noexcept -> usize;
    [[nodiscard]] auto live_slots() const noexcept -> usize;
    [[nodiscard]] auto table_pages() const noexcept -> usize;

private:
    friend class ::mm::VSpace;
    friend class ::Env;
    friend class Batch;
    friend class ipc::Channel;
    friend struct object::traits<CSpace>;
    static constexpr usize dir_bits = 8;
    static constexpr usize dir_entries = usize{1} << dir_bits;
    static constexpr u32 invalid_index = ~u32{};
    static constexpr usize max_leaves = dir_entries * dir_entries;

    enum class SlotState : u8 {
        Empty,
        Reserved,
        Occupied,
        Quarantined,
    };

    struct Slot final {
        mutable sync::Spin lock;
        union Storage {
            byte empty;
            Capability capability;

            constexpr Storage() noexcept : empty{} {}
            ~Storage() {}
        } storage{};
        u64 generation{};
        u32 next{invalid_index};
        u32 previous{invalid_index};
        SlotState state{SlotState::Empty};

        ~Slot() noexcept {
            libk_assert(state != SlotState::Occupied);
        }
    };

    struct DirPage final {
        explicit DirPage(mm::Page physical) noexcept : page(physical) {}
        mm::Page page{};
        std::atomic<void*> children[dir_entries]{};
    };

    static constexpr usize leaf_bits = std::bit_width(
        (mm::page_size - sizeof(mm::Page)) / sizeof(Slot)) - 1;
    static constexpr usize leaf_slots = usize{1} << leaf_bits;

    struct LeafPage final {
        explicit LeafPage(mm::Page physical) noexcept : page(physical) {}
        mm::Page page{};
        Slot slots[leaf_slots]{};
    };

    static_assert(sizeof(DirPage) <= mm::page_size);
    static_assert(sizeof(LeafPage) <= mm::page_size);

    // The slot view is validated once while acquiring its grant admission.
    struct Snapshot final {
        GrantPin pin;
        View view;
        u64 token{};
    };

    auto install(std::span<Capability*>, std::span<Reservation*>, Graph::Admission,
        std::span<const object::ref<>*>, libk::delegate<void() noexcept>, std::span<const Graph::Attach> = {}) noexcept -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto snapshot(Handle handle) noexcept
        -> std::expected<Snapshot, CSpaceError>;
    struct Xfer final {
        Reservation slot;
        GrantPin pin;
        GrantRef grant;
        Handle source;
        View original, view;
        XferOp op;
    };
    [[nodiscard]] auto prepare_xfer(CSpace&, const XferSpec&, resource::Reservation reservation = {}) noexcept
        -> std::expected<Xfer, CSpaceError>;
    [[nodiscard]] auto commit_xfers(CSpace&, std::span<Xfer>) noexcept
        -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto reserved(const Reservation&) noexcept -> Slot*;
    void publish(Reservation&, Capability&&) noexcept;
    void rollback(Handle handle) noexcept;
    void run_work() noexcept;
    void release() noexcept;
    void finish_retire() noexcept;
    void changed() noexcept;
    void post() noexcept { if (work_) work_->post(job_); }
    void complete_cleanup() noexcept;
    [[nodiscard]] auto reserve_grant() noexcept
        -> std::expected<resource::Reservation, CSpaceError>;
    [[nodiscard]] auto escrow_move(
        Handle source,
        Capability& cap,
        Reservation& reservation) noexcept
        -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto escrow_restore(
        Reservation& reservation,
        Capability&& cap) noexcept -> bool;
    [[nodiscard]] auto escrow_drop(
        Reservation& reservation) noexcept -> resource::Usage;
    void retain_escrow() noexcept;
    void release_escrow() noexcept;
    [[nodiscard]] auto attach_execution() noexcept -> bool;
    void detach_execution() noexcept;
    void bind_allocation(resource::Allocation& allocation) noexcept;
    [[nodiscard]] auto grow() noexcept -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto slot(usize index) noexcept -> Slot*;
    [[nodiscard]] auto slot(usize index) const noexcept -> const Slot*;
    void push_free(usize index, Slot& slot) noexcept;
    void link_occupied(usize index, Slot& slot) noexcept;
    void unlink_occupied(usize index, Slot& slot) noexcept;
    [[nodiscard]] static auto policy_error(PolicyError error) noexcept
        -> CSpaceError;
    [[nodiscard]] static auto grant_error(GrantError error) noexcept
        -> CSpaceError;

    mm::Pmm* pmm_{};
    mm::PageGroup pages_;
    Quota quota_{};
    mutable sync::Spin lock_{};
    std::atomic<DirPage*> root_{};
    u32 free_head_{invalid_index};
    u32 occupied_head_{invalid_index};
    usize next_leaf_{};
    usize page_count_{};
    usize live_slots_{};
    u64 epoch_{1};
    usize quarantined_slots_{};
    std::atomic<bool> accepting_{true};
    bool growing_{}, reassigning_{};
    usize preparing_{};
    bool releasing_{};
    bool retired_{};
    object::cleanup cleanup_{};
    DirPage* retiring_{};
    WorkQueue* work_{};
    Work job_{Work::Fn::bind<&CSpace::run_work>(*this)};
    bool dirty_{}, advancing_{}, again_{};
    usize bindings_{};
    usize escrows_{};
    resource::Allocation* allocation_{};
    // All table pages and committed selectors share this immutable source.
    // Detached usage tokens release only after the corresponding storage is free.
    resource::Usage usage_{};
};

// Captured admissions and reserved slots; source moves are revalidated and
// the complete batch is published under both CSpace locks, or none of it is.
class Batch final : private libk::noncopyable {
public:
    using Specs = libk::inplace_vector<XferSpec, IPC_MAX_CAPS>;
    using Handles = libk::inplace_vector<Handle, IPC_MAX_CAPS>;
    Batch() noexcept = default;
    Batch(Batch&&) noexcept = default;
    auto operator=(Batch&&) noexcept -> Batch& = default;
    [[nodiscard]] static auto prepare(Batch&, CSpace&, CSpace&, const Specs&) noexcept
        -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto commit() noexcept -> std::expected<Handles, CSpaceError>;
    [[nodiscard]] auto handles() const noexcept -> Handles;
    void abort() noexcept { entries_.clear(); source_ = destination_ = nullptr; }
    [[nodiscard]] auto empty() const noexcept -> bool { return entries_.empty(); }
private:
    CSpace* source_{};
    CSpace* destination_{};
    libk::inplace_vector<CSpace::Xfer, IPC_MAX_CAPS> entries_;
};

} // namespace cap
