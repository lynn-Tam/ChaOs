#pragma once

#include <base/types.hpp>
#include <ipc/notification_source.hpp>
#include <sync.hpp>
#include <uapi/status.h>

namespace ipc { class Notification; }

// Immutable result and target-owned doorbell. Query reads the record;
// Notification only announces a change and can aggregate many threads.
class Exit final {
public:
    enum class Reason : u8 { Normal, Failed, Fault, Stop, Revoked, Closed, Invariant };

    struct Info final {
        u64 sequence{};
        Reason reason{Reason::Normal};
        myos_status_t status{};
        usize detail{};
        usize pc{};
        usize address{};
        u8 locus{};
    };

    Exit() noexcept = default;
    ~Exit() noexcept;
    Exit(const Exit&) = delete;
    auto operator=(const Exit&) -> Exit& = delete;

    [[nodiscard]] auto observe(ipc::Notification&, u64 badge) noexcept -> bool;
    [[nodiscard]] auto read() const noexcept -> Info;
    [[nodiscard]] auto claim(Reason, myos_status_t, usize detail = 0,
        usize pc = 0, usize address = 0, u8 locus = 0) noexcept -> bool;
    [[nodiscard]] auto published() const noexcept -> bool;

private:
    mutable sync::Spin lock_{};
    Info value_{};
    ipc::NotificationSource source_{};
};
