#pragma once

#include <libk/types.hpp>
#include <optional>
#include <libk/assert.hpp>
#include <libk/noncopyable.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/inplace_ring.hpp>
#include <sync.hpp>
#include <limits>

class Thread;

namespace sched {

class Domain;
class Dispatcher;

// A real runnable position for Domain branches and Thread leaves.
// Only its home CPU changes these queue fields.
struct Node final {
    libk::intrusive_tree_hook hook{};
    struct Less {
        bool operator()(const Node& a, const Node& b) const noexcept {
            return std::pair{a.service, a.sequence} < std::pair{b.service, b.sequence};
        }
    };
    using Queue = libk::intrusive_tree<Node, &Node::hook, Less>;
    Queue ready{};
    Node* parent{};
    Domain* domain{};
    ::Thread* thread{};
    u64 service{}, fraction{}, floor{}, sequence{};
    u32 weight{1024};
};

// A conserved aggregate ledger. Escrow is unavailable to other CPUs until
// its owner reaches a real scheduling boundary and settles actual execution.
class Budget final : private libk::noncopyable_nonmovable {
public:
    static constexpr usize max_refills = 8;
    struct Loan { u64 amount{}, generation{}; };
    struct Info { __int128 balance; u64 refill, escrow, generation, available, debt; };
    Budget(u64 amount, u64 period, usize capacity = max_refills) noexcept
        : amount_(amount), period_(period), capacity_(capacity), balance_(amount) { libk_assert(capacity && capacity <= max_refills); }
    ~Budget() noexcept { libk_assert(!escrow_); }
    auto info(u64 now) const noexcept -> Info;
    auto next_refill() const noexcept -> u64;
    auto reserve(u64 now, u64 limit) noexcept -> Loan;
    auto settle(Loan, u64 now, u64 elapsed) noexcept -> bool;
    auto consume(u64 now, u64 elapsed) noexcept -> u64;
    auto configure(u64 amount, u64 period, usize capacity, u64 now) noexcept -> bool;
private:
    struct Refill { u64 ready, amount; };
    void mature(u64 now) noexcept;
    void record(u64 now, u64 elapsed) noexcept;
    mutable sync::Spin lock_;
    u64 amount_{}, period_{}, escrow_{}, generation_{1};
    u64 refill_tail_{}, refill_fraction_{};
    usize capacity_;
    __int128 balance_{};
    libk::inplace_ring<Refill, max_refills> refills_;
};

enum class DispatchReason : u8 {
    Start,
    Yield,
    Timer,
    Block,
    Exit,
    Stop,
    RemoteWake,
};

class Dispatcher;

// Fixed-storage one-shot deadline owned by exactly one Dispatcher while
// armed. The callback runs on that CPU with interrupts disabled; remote
// producers publish subsystem state and wake the owner instead of mutating
// this queue.
class Deadline final : private libk::noncopyable_nonmovable {
public:
    using Fn = libk::delegate<void() noexcept>;

    explicit Deadline(Fn cb) noexcept : cb_(cb) {}
    ~Deadline() noexcept { libk_assert(!armed() && !hook_.is_linked() && cb_); }

    [[nodiscard]] auto armed() const noexcept -> bool {
        return owner_ != nullptr;
    }

private:
    friend class Dispatcher;

    libk::intrusive_tree_hook hook_{};
    Fn cb_{};
    Dispatcher* owner_{};
    u64 when_{};
};

} // namespace sched
