#pragma once

#include <libk/noncopyable.hpp>
#include <sched/ready_queue.hpp>

namespace kernel::sched {

struct DispatchCandidate final {
    Binding* binding{};
};

class BuiltinPolicy final : private libk::noncopyable_nonmovable {
public:
    void enqueue(Binding& binding, Urgency urgency) noexcept {
        ready_.enqueue(binding, urgency);
    }
    void remove(Binding& binding, Urgency urgency) noexcept {
        ready_.remove(binding, urgency);
    }
    [[nodiscard]] auto select() noexcept -> DispatchCandidate {
        return DispatchCandidate{ready_.front()};
    }
    [[nodiscard]] auto select_activation() noexcept -> DispatchCandidate {
        return DispatchCandidate{ready_.activation_front()};
    }
    [[nodiscard]] auto ready_count() const noexcept -> usize {
        return ready_.size();
    }

private:
    ReadyQueue ready_{};
};

} // namespace kernel::sched
