#pragma once

#include <irq/route.hpp>
#include <ipc/notification_source.hpp>
#include <expected>
#include <object/ref.hpp>

namespace irq {

enum class Error : u8 { InvalidState, Busy, StaleSequence, BadSequence, Closed };
struct Delivery final {
    u64 sequence{};
    u64 generation{};
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return sequence != 0 && generation != 0;
    }
};

class Irq final : private libk::noncopyable_nonmovable {
public:
    explicit Irq(Line line) noexcept;
    ~Irq() noexcept;
    [[nodiscard]] auto source() const noexcept -> Line { return route_.line_; }
    [[nodiscard]] auto bound() const noexcept -> bool;
    [[nodiscard]] auto pending() const noexcept -> bool;
    [[nodiscard]] auto closed() const noexcept -> bool;
    [[nodiscard]] auto bind(ipc::Notification&, u64 badge) noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto unbind() noexcept -> bool;
    [[nodiscard]] auto delivery() const noexcept -> std::expected<Delivery, Error>;
    [[nodiscard]] auto ack(u64 generation, u64 sequence) noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto close() noexcept -> bool;
    void retire(object::cleanup&& cleanup) noexcept;
private:
    [[nodiscard]] auto publish() noexcept -> bool;
    Route route_;
    ipc::NotificationSource notice_{};
    u64 generation_{};
    u64 sequence_{};
    bool pending_{};
    bool closed_{};
};

} // namespace irq
