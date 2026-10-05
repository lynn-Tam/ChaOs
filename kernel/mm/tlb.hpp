#pragma once

#include <cpu/types.hpp>
#include <mm/pmm.hpp>
#include <sync.hpp>

class CpuRegistry;
struct CpuLocal;

namespace mm {

class Tlb;

// One stable owner for an invalidation and its detached physical pages.
// The caller keeps this object until complete(), even if its waiter exits.
class Flush final : private libk::noncopyable_nonmovable {
  public:
    explicit Flush(sync::Latch::Notifier notify = {}) noexcept : done_(notify) {}
    explicit Flush(Pmm &pmm, sync::Latch::Notifier notify = {}) noexcept
        : pages_(pmm.group()), done_(notify) {}
    ~Flush() noexcept;
    bool submitted() const noexcept { return done_.initialized(); }
    bool complete() const noexcept { return !submitted() || done_.complete(); }
    bool acknowledged(CpuId) const noexcept;
    bool executable() const noexcept { return executable_; }
    const CpuSet &targets() const noexcept { return targets_; }
    bool adopt(OwnedPage &&page) noexcept { return pages_.attach(std::move(page)); }
    bool adopt(OwnedPage &&page, resource::Charge &&fee) noexcept {
        if (fee.amount() != resource::budget{.memory = page_size} || !adopt(std::move(page))) return false;
        fee_.merge(std::move(fee));
        return true;
    }
    usize page_count() const noexcept { return pages_.page_count(); }
    bool release(resource::Charge &refund) noexcept;
    // Failed sends leave the same pending request live; duplicate IPIs are safe.
    bool kick(CpuRegistry &) const noexcept;

  private:
    friend class Tlb;
    friend void drain_tlb(CpuId) noexcept;
    void ack(CpuId) noexcept;
    resource::Charge fee_{};
    PageGroup pages_{};
    sync::Latch done_;
    Tlb *owner_{};
    CpuSet targets_{}, pending_{};
    bool executable_{};
    Flush *next_{};
    Flush **link_{};
};

// Admission and PTE edits share this lock. A late entrant fences locally;
// an entrant already present in the snapshot must acknowledge the request.
class Tlb final : private libk::noncopyable_nonmovable {
  public:
    class Edit final {
      public:
        Edit(Edit &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), lock_(std::move(other.lock_)) {}
        Edit(const Edit &) = delete;
        ~Edit() noexcept { abort(); }
        const CpuSet &targets() const noexcept { return owner_->active_; }
        bool commit(Flush &, CpuRegistry *, CpuId local, bool executable = false) noexcept;
        void publish_fresh() noexcept;
        void abort() noexcept {
            if (owner_) {
                lock_.restore();
                owner_ = nullptr;
            }
        }

      private:
        friend class Tlb;
        explicit Edit(Tlb &owner) noexcept : owner_(&owner), lock_(owner.lock_) {}
        Tlb *owner_{};
        sync::Lock<sync::Spin> lock_;
    };
    ~Tlb() noexcept;
    void enter(CpuId) noexcept;
    void leave(CpuId) noexcept;
    Edit begin() noexcept { return Edit{*this}; }
    CpuSet active_cpus() const noexcept;

  private:
    friend class Edit;
    friend class Flush;
    mutable sync::Spin lock_{};
    CpuSet active_{};
    libk::Atomic<usize> retained_{};
};

// Borrowed hardware root, used by both user and shared kernel address spaces.
class Root final {
  public:
    constexpr Root(Tlb &state, usize root) noexcept : state_(&state), root_(root) {}
    Tlb &state() const noexcept { return *state_; }
    usize root() const noexcept { return root_; }
    void activate(CpuLocal &) const noexcept;
    void adopt(CpuLocal &) const noexcept;

  private:
    Tlb *state_{};
    usize root_{};
};

void drain_tlb(CpuId) noexcept;
} // namespace mm
