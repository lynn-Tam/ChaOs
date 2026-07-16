#pragma once

#include <arch/trap.hpp>
#include <core/types.hpp>
#include <libk/noncopyable.hpp>

namespace kernel {

class CpuRegistry;
class Thread;

// One non-owning edge from a Thread to a subsystem-owned blocking operation.
// The subsystem owns the relation storage and operation lifetime. Thread owns
// only the fact that this is its single canonical wait.
class WaitRelation final : private libk::noncopyable_nonmovable {
public:
    template<
        typename Owner,
        bool (Owner::*Complete)() const noexcept,
        void (Owner::*Resume)(arch::TrapContext&) noexcept,
        void (Owner::*Cancel)() noexcept>
    [[nodiscard]] static auto bind(Owner& owner) noexcept -> WaitRelation {
        static constexpr Ops operations{
            .complete = [](const void* context) noexcept {
                return (static_cast<const Owner*>(context)->*Complete)();
            },
            .resume = [](void* context, arch::TrapContext& trap) noexcept {
                (static_cast<Owner*>(context)->*Resume)(trap);
            },
            .cancel = [](void* context) noexcept {
                (static_cast<Owner*>(context)->*Cancel)();
            },
        };
        return WaitRelation{owner, operations};
    }

    ~WaitRelation() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool {
        return thread_ != nullptr;
    }
    [[nodiscard]] auto complete() const noexcept -> bool {
        return ops_->complete(owner_);
    }

    // Completion notification is deliberately narrower than scheduling: the
    // relation may request a wake, but it cannot mutate Thread state itself.
    void notify() noexcept;

private:
    friend class Thread;

    struct Ops final {
        bool (*complete)(const void*) noexcept;
        void (*resume)(void*, arch::TrapContext&) noexcept;
        void (*cancel)(void*) noexcept;
    };

    template<typename Owner>
    explicit WaitRelation(Owner& owner, const Ops& ops) noexcept
        : owner_(&owner), ops_(&ops) {}

    void attach(Thread& thread, CpuRegistry& cpus) noexcept;
    void resume(arch::TrapContext& trap) noexcept;
    void cancel() noexcept;
    void detach() noexcept;

    void* owner_{};
    const Ops* ops_{};
    Thread* thread_{};
    CpuRegistry* cpus_{};
};

} // namespace kernel
