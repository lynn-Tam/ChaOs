#pragma once

#include <libk/types.hpp>
#include <ipc/notification_source.hpp>
#include <sync.hpp>
#include <uapi/abi.h>
#include <optional>

namespace ipc { class Notification; }

// Immutable result and target-owned doorbell. Query reads the record;
// Notification only announces a change and can aggregate many threads.
class Exit final {
public:
    Exit() noexcept = default;
    ~Exit() noexcept;
    Exit(const Exit&) = delete;
    auto operator=(const Exit&) -> Exit& = delete;

    [[nodiscard]] auto observe(ipc::Notification&, u64 badge) noexcept -> bool;
    [[nodiscard]] auto read() const noexcept -> std::optional<status_t>;
    [[nodiscard]] auto claim(status_t) noexcept -> bool;
    [[nodiscard]] auto published() const noexcept -> bool;

private:
    mutable sync::Spin lock_{};
    std::optional<status_t> status_;
    ipc::NotificationSource source_{};
};
