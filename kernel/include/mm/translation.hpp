#pragma once

#include <arch/interrupt.hpp>
#include <arch/page_table.hpp>
#include <core/types.hpp>
#include <cpu/cpu_set.hpp>
#include <cpu/ipi_delivery.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_ring.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <libk/sync/atomic.hpp>
#include <sync/lock.hpp>
#include <mm/pmm.hpp>
#include <resource/sponsorship.hpp>
#include <sync/completion.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel {
class CpuRegistry;
struct CpuLocal;
struct CpuRuntime;
}

namespace kernel::mm {

struct TranslationEpoch final {
    u64 raw{};

    [[nodiscard]] friend constexpr auto operator==(
        TranslationEpoch, TranslationEpoch) noexcept -> bool = default;
};

// Per-address-space instruction visibility generation. It advances only when
// executable PTEs become reachable; it is independent of ordinary TLB edits.
struct InstructionEpoch final {
    u64 raw{};

    [[nodiscard]] friend constexpr auto operator==(
        InstructionEpoch, InstructionEpoch) noexcept -> bool = default;
};

struct TranslationObservation final {
    TranslationEpoch translation{};
    InstructionEpoch instruction{};
};

enum class ShootdownError : u8 {
    EpochExhausted,
    QueueFull,
    TargetUnavailable,
};

enum class ShootdownStatus : u8 {
    Complete,
    Pending,
};

enum class ShootdownRetry : u8 {
    Idle,
    Delivered,
    TransportFailure,
};

class TranslationState;
class ShootdownPlan;

// One versioned completion identity. Requests hold its address, so a ticket is
// deliberately neither movable nor reusable.
class ShootdownTicket final : private libk::noncopyable_nonmovable {
public:
    explicit ShootdownTicket(
        kernel::sync::Completion::Notifier notifier = {}) noexcept
        : completion_(notifier) {}
    ~ShootdownTicket() noexcept;

    [[nodiscard]] auto initialized() const noexcept -> bool {
        return owner_ != nullptr;
    }
    [[nodiscard]] auto complete() const noexcept -> bool;
    [[nodiscard]] auto epoch() const noexcept -> TranslationEpoch;
    [[nodiscard]] auto instruction_epoch() const noexcept -> InstructionEpoch;
    [[nodiscard]] auto requires_instruction_sync() const noexcept -> bool {
        return instruction_sync_;
    }
    [[nodiscard]] auto targets() const noexcept -> const kernel::CpuSet&;
    [[nodiscard]] auto acknowledged(kernel::CpuId cpu) const noexcept -> bool;

private:
    friend class TranslationState;
    friend class ShootdownQueue;
    friend class RetireBatch;
    friend void drain_shootdowns(kernel::CpuRuntime& runtime) noexcept;

    void initialize(
        TranslationState& owner,
        TranslationEpoch epoch,
        InstructionEpoch instruction_epoch,
        bool instruction_sync,
        kernel::CpuSet targets) noexcept;
    void acknowledge(kernel::CpuId cpu) noexcept;
    [[nodiscard]] auto arm() noexcept -> bool { return completion_.arm(); }
    [[nodiscard]] auto notifiable() const noexcept -> bool {
        return completion_.notifiable();
    }

    TranslationState* owner_{};
    TranslationEpoch epoch_{};
    InstructionEpoch instruction_epoch_{};
    bool instruction_sync_{};
    kernel::CpuSet targets_{};
    libk::Atomic<u64> acknowledgements_[kernel::CpuSet::word_count]{};
    kernel::sync::Completion completion_;
};

// Detached hardware resources remain owned here until the associated ticket
// has observed every target CPU. The page group makes batch size independent
// of the edited virtual range.
class RetireBatch final : private libk::noncopyable_nonmovable {
public:
    explicit RetireBatch(Pmm& pmm) noexcept : pages_(pmm.make_page_group()) {}
    ~RetireBatch() noexcept;

    [[nodiscard]] auto adopt(OwnedPage&& page) noexcept -> bool {
        return pages_.attach(libk::move(page));
    }
    [[nodiscard]] auto adopt(
        OwnedPage&& page,
        kernel::resource::Charge&& charge) noexcept -> bool {
        if (charge.budget() != kernel::resource::Budget{
                .memory = page_size,
            }
            || !pages_.attach(libk::move(page))) {
            return false;
        }
        charge_.merge(libk::move(charge));
        return true;
    }
    [[nodiscard]] auto empty() const noexcept -> bool {
        return pages_.page_count() == 0;
    }
    [[nodiscard]] auto page_count() const noexcept -> usize {
        return pages_.page_count();
    }
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto release() noexcept -> bool;

private:
    friend class TranslationState;
    void bind(ShootdownTicket& ticket) noexcept;

    // Declared before pages_ so reverse destruction frees physical pages
    // before the corresponding capacity is refunded.
    kernel::resource::Charge charge_{};
    OwnedPageGroup pages_;
    ShootdownTicket* ticket_{};
    TranslationState* owner_{};
};

struct ShootdownRequest final {
    ShootdownTicket* ticket{};
    kernel::CpuId target{};
};

class ShootdownQueue final : private libk::noncopyable_nonmovable {
public:
    static constexpr usize capacity = 64;

private:
    friend class ShootdownPlan;
    friend void drain_shootdowns(kernel::CpuRuntime& runtime) noexcept;
    friend auto retry_shootdowns(
        kernel::CpuRegistry& cpus,
        const ShootdownTicket& ticket) noexcept -> ShootdownRetry;

    [[nodiscard]] auto reserve() noexcept -> bool;
    void cancel() noexcept;
    void publish(ShootdownRequest request) noexcept;
    [[nodiscard]] auto claim_transport() noexcept
        -> libk::optional<kernel::IpiDelivery::Token>;
    void transport_failed(kernel::IpiDelivery::Token token) noexcept;
    void take_all(libk::InplaceRing<ShootdownRequest, capacity>& batch) noexcept;

    kernel::sync::SpinLock<kernel::sync::LockClass::Shootdown> lock_{};
    libk::InplaceRing<ShootdownRequest, capacity> requests_{};
    usize reserved_{};
    kernel::IpiDelivery delivery_{};
};

// Capacity is reserved on every remote target before the caller edits a PTE.
// Destruction cancels unused reservations; publication consumes them.
class ShootdownPlan final {
public:
    ShootdownPlan(const ShootdownPlan&) = delete;
    auto operator=(const ShootdownPlan&) -> ShootdownPlan& = delete;
    ShootdownPlan(ShootdownPlan&& other) noexcept;
    auto operator=(ShootdownPlan&&) -> ShootdownPlan& = delete;
    ~ShootdownPlan() noexcept;

    [[nodiscard]] static auto prepare(
        kernel::CpuRegistry& cpus,
        kernel::CpuId local,
        const kernel::CpuSet& targets) noexcept
        -> libk::Expected<ShootdownPlan, ShootdownError>;

    // Useful before CpuRegistry publication and for deterministic protocol
    // tests. It accepts only an entirely local target snapshot.
    [[nodiscard]] static auto local(
        kernel::CpuId local,
        const kernel::CpuSet& targets) noexcept
        -> libk::Expected<ShootdownPlan, ShootdownError>;

private:
    friend class TranslationState;

    ShootdownPlan(
        kernel::CpuRegistry* cpus,
        kernel::CpuId local,
        kernel::CpuSet targets,
        kernel::CpuSet reserved) noexcept
        : cpus_(cpus), local_(local), targets_(targets), reserved_(reserved) {}

    [[nodiscard]] auto publish(ShootdownTicket& ticket) noexcept
        -> kernel::CpuSet;
    [[nodiscard]] auto kick(const kernel::CpuSet& targets) noexcept -> bool;
    void cancel() noexcept;

    kernel::CpuRegistry* cpus_{};
    kernel::CpuId local_{};
    kernel::CpuSet targets_{};
    kernel::CpuSet reserved_{};
};

class TranslationState final : private libk::noncopyable_nonmovable {
    using StateLock =
        kernel::sync::SpinLock<kernel::sync::LockClass::Translation>;

public:
    class Mutation final {
    public:
        Mutation(const Mutation&) = delete;
        auto operator=(const Mutation&) -> Mutation& = delete;
        Mutation(Mutation&& other) noexcept;
        auto operator=(Mutation&&) -> Mutation& = delete;
        ~Mutation() noexcept;

        [[nodiscard]] auto targets() const noexcept -> const kernel::CpuSet& {
            return targets_;
        }

        [[nodiscard]] auto commit(
            ShootdownPlan&& plan,
            ShootdownTicket& ticket,
            RetireBatch* retire = nullptr,
            bool instruction_sync = false) noexcept -> ShootdownStatus;
        // Publishes mappings at a virtual range that has never been visible
        // and will never be rebound to different backing. No remote CPU can
        // hold a stale translation for this operation.
        void publish_fresh(bool instruction_sync = false) noexcept;
        void abort() noexcept { unlock(); }

    private:
        friend class TranslationState;
        Mutation(
            TranslationState& owner,
            kernel::sync::IrqLockToken<StateLock>&& token,
            kernel::CpuSet targets) noexcept
            : owner_(&owner), token_(libk::move(token)), targets_(targets) {}

        void unlock() noexcept;

        TranslationState* owner_{};
        kernel::sync::IrqLockToken<StateLock> token_;
        kernel::CpuSet targets_{};
    };

    TranslationState() noexcept = default;
    ~TranslationState() noexcept;

    [[nodiscard]] auto enter(kernel::CpuId cpu) noexcept
        -> TranslationObservation;
    void leave(kernel::CpuId cpu) noexcept;
    [[nodiscard]] auto begin() noexcept
        -> libk::Expected<Mutation, ShootdownError>;

    [[nodiscard]] auto epoch() const noexcept -> TranslationEpoch;
    [[nodiscard]] auto observation() const noexcept -> TranslationObservation;
    [[nodiscard]] auto active_cpus() const noexcept -> kernel::CpuSet;
    [[nodiscard]] auto pending_tickets() const noexcept -> usize {
        return pending_tickets_.load<libk::MemoryOrder::Acquire>();
    }
    [[nodiscard]] auto pending_retires() const noexcept -> usize {
        return pending_retires_.load<libk::MemoryOrder::Acquire>();
    }

private:
    friend class Mutation;
    friend class TranslationView;
    friend class ShootdownTicket;
    friend class RetireBatch;
    friend void drain_shootdowns(kernel::CpuRuntime& runtime) noexcept;

    mutable StateLock lock_{};
    kernel::CpuSet active_{};
    TranslationEpoch epoch_{};
    InstructionEpoch instruction_epoch_{};
    libk::Atomic<usize> pending_tickets_{};
    libk::Atomic<usize> pending_retires_{};
};

// Non-owning hardware projection minted by a VSpace owner. It linearizes an
// address-space switch against mutations before touching SATP.
class TranslationView final {
public:
    constexpr TranslationView(
        TranslationState& state,
        arch::RootToken root) noexcept : state_(&state), root_(root) {}

    [[nodiscard]] auto state() const noexcept -> TranslationState& {
        return *state_;
    }
    [[nodiscard]] auto root() const noexcept -> arch::RootToken { return root_; }

    void activate(kernel::CpuLocal& cpu) const noexcept;
    void adopt(kernel::CpuLocal& cpu) const noexcept;

private:
    TranslationState* state_{};
    arch::RootToken root_;
};

void drain_shootdowns(kernel::CpuRuntime& runtime) noexcept;
[[nodiscard]] auto retry_shootdowns(
    kernel::CpuRegistry& cpus,
    const ShootdownTicket& ticket) noexcept -> ShootdownRetry;

} // namespace kernel::mm
