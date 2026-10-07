#pragma once

#include <sys/channel.hpp>
#include <sys/start.hpp>

namespace sys::service {
inline void require(status_t status) noexcept {
    if (status != STATUS_OK) sys::exit(status);
}
// A service connection has one reader. Readiness belongs to that reader's
// Notification, allowing one event loop to multiplex several sources.
// Construct once for the service lifetime:
// task authority revocation withdraws the binding. The supervisor stops the
// connected services together if either persistent peer terminates.
class Connection final {
    cap_t channel_;
    cap_t events_;
    word_t readable_;
    uint64_t sequence_{};
    word_t writable_{};
    uint64_t write_sequence_{};
public:
    Connection(cap_t channel, cap_t events) noexcept
        : channel_(channel), events_(events) {
        const auto binding = channel_bind(channel_, events_, CHANNEL_READABLE);
        require(binding.status);
        readable_ = binding.value;
    }
    Connection(const Connection&) = delete;
    auto operator=(const Connection&) -> Connection& = delete;

    auto send(const Message& message) const noexcept -> SysResult {
        return service::send(channel_, message);
    }
    auto enable_writable() noexcept -> status_t {
        const auto binding = channel_bind(channel_, events_, CHANNEL_WRITABLE);
        if (binding.status == STATUS_OK) writable_ = binding.value;
        return binding.status;
    }
    auto try_send(const Message& message) noexcept -> SysResult {
        const auto result = service::send(channel_, message, false);
        if (result.status == STATUS_OK)
            write_sequence_ = result.value;
        return result;
    }
    auto arm_writable() noexcept -> SysResult {
        const auto result = channel_arm(channel_, writable_, write_sequence_);
        if (result.status == STATUS_OK) write_sequence_ = result.value;
        return result;
    }
    auto try_receive(Message& message) noexcept -> SysResult {
        const auto result = service::receive(channel_, message, false);
        if (result.status == STATUS_OK) sequence_ = result.value;
        return result;
    }
    auto try_receive(Message& message, cap::OwnedCap& capability) noexcept -> SysResult {
        const auto result = service::receive_cap(channel_, message, capability);
        if (result.status == STATUS_OK || result.status == STATUS_BAD_ARGS)
            sequence_ = result.value;
        return result;
    }
    auto arm() noexcept -> SysResult {
        const auto result = channel_arm(channel_, readable_, sequence_);
        if (result.status == STATUS_OK) sequence_ = result.value;
        return result;
    }
    auto receive(Message& message) noexcept -> SysResult {
        for (;;) {
            const auto result = try_receive(message);
            if (result.status == STATUS_OK) {
                return result;
            }
            if (result.status != STATUS_WOULD_BLOCK) return result;
            const auto armed = arm();
            if (armed.status != STATUS_OK) return armed;
            const auto wake = notification_wait(events_);
            if (wake.status != STATUS_OK) return wake;
            // A shared event badge is only a hint. Always recheck this queue;
            // arm compares its sequence atomically with current readiness.
        }
    }
};


} // namespace sys::service
