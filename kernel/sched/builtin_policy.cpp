#include <sched/builtin_policy.hpp>

#include <libk/bits.hpp>
#include <sched/context.hpp>

namespace kernel::sched {

void ReadyQueue::enqueue(Binding& binding, Urgency urgency) noexcept {
    KASSERT(!binding.queued());
    const u8 level = urgency.value();
    levels_[level].push_back(binding);
    bitmap_ |= u32{1} << level;
    ++size_;
}

void ReadyQueue::remove(Binding& binding, Urgency urgency) noexcept {
    KASSERT(binding.queued());
    const u8 level = urgency.value();
    auto& queue = levels_[level];
    queue.erase(binding);
    KASSERT(size_ != 0);
    --size_;
    if (queue.empty()) {
        bitmap_ &= ~(u32{1} << level);
    }
}

auto ReadyQueue::front(bool prefer_activation) noexcept -> Binding* {
    if (bitmap_ == 0) {
        return nullptr;
    }
    const usize level =
        (sizeof(u32) * 8 - 1) - libk::countl_zero(bitmap_);
    KASSERT(level < Urgency::level_count);
    // Activation is only a tie-breaker inside the highest effective urgency
    // level. Once the policy has consumed its streak, prefer an ordinary
    // peer so a producer cannot turn repeated signals into starvation.
    for (Binding& binding : levels_[level]) {
        if (binding.activation_credit_ == prefer_activation) {
            return &binding;
        }
    }
    return &levels_[level].front();
}

auto BuiltinPolicy::select() noexcept -> DispatchCandidate {
    Binding* const candidate = ready_.front(!activation_turn_used_);
    if (candidate == nullptr) {
        activation_turn_used_ = false;
    } else {
        activation_turn_used_ = candidate->activation_credit_;
    }
    return DispatchCandidate{candidate};
}

auto ReadyQueue::pop_front(Urgency urgency) noexcept -> Binding* {
    const u8 level = urgency.value();
    auto& queue = levels_[level];
    if (queue.empty()) {
        return nullptr;
    }
    Binding& binding = queue.pop_front();
    KASSERT(size_ != 0);
    --size_;
    if (queue.empty()) {
        bitmap_ &= ~(u32{1} << level);
    }
    return &binding;
}

} // namespace kernel::sched
