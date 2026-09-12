#pragma once

#include <libk/shared_ring.hpp>
#include <uapi/status.h>

namespace myos::io {

inline constexpr uint64_t QueueVersion = 1;
inline constexpr size_t QueueDepth = 32;
using Submissions = libk::SharedRing<8, QueueDepth>;
using Completions = libk::SharedRing<4, QueueDepth>;

enum class Operation : uint64_t { Read = 1 };
// Cancellation travels over the session's control Channel, so a full data
// queue never prevents it. Its reply acknowledges receipt, not buffer release.
enum class Control : uint64_t { Open = 1, Cancel, Close };

struct Request final {
    uint64_t id{};
    uint64_t operation{};
    uint64_t object{};
    uint64_t offset{};
    uint64_t buffer{};
    uint64_t buffer_offset{};
    uint64_t length{};
    uint64_t flags{};

    [[nodiscard]] auto encode() const noexcept -> Submissions::Entry {
        return {{id, operation, object, offset, buffer, buffer_offset, length, flags}};
    }
    [[nodiscard]] static auto decode(const Submissions::Entry& value) noexcept -> Request {
        return {value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7]};
    }
};

struct Completion final {
    uint64_t id{};
    int64_t status{};
    uint64_t bytes{};
    uint64_t flags{};

    [[nodiscard]] auto encode() const noexcept -> Completions::Entry {
        return {{id, static_cast<uint64_t>(status), bytes, flags}};
    }
    [[nodiscard]] static auto decode(const Completions::Entry& value) noexcept -> Completion {
        return {value[0], __builtin_bit_cast(int64_t, value[1]), value[2], value[3]};
    }
};

// The service owns the MemoryObjects and initializes both pages before
// granting access. ClientPage is writable only by the client; ServerPage only
// by the service. Each peer maps the other page read-only. Neither page holds
// payload buffers or capability selectors. Session identity comes from the
// authenticated control exchange, not an untrusted shared header.
struct alignas(4096) ClientPage final {
    Submissions::Data submissions{};
    Completions::Cursor completions{};
};
struct alignas(4096) ServerPage final {
    Completions::Data completions{};
    Submissions::Cursor submissions{};
};
static_assert(sizeof(ClientPage) == 4096 && sizeof(ServerPage) == 4096);

// Owned by one client execution. Publish once per submitted batch and signal
// the server Notification when publish() returns true. After draining a batch,
// release completions and signal again so admission blocked on credits resumes.
class ClientQueue final : private libk::noncopyable_nonmovable {
public:
    ClientQueue(ClientPage& client, const ServerPage& server) noexcept
        : submissions_(client.submissions, server.submissions),
          completions_(server.completions, client.completions) {}

    [[nodiscard]] auto submit(Request& request) noexcept -> libk::RingResult {
        if (next_id_ == UINT64_MAX) return libk::RingResult::Exhausted;
        Request snapshot = request;
        snapshot.id = next_id_ + 1;
        const auto result = submissions_.push(snapshot.encode());
        if (result == libk::RingResult::Ready) {
            request.id = ++next_id_;
        }
        return result;
    }
    [[nodiscard]] auto publish() noexcept -> bool { return submissions_.publish(); }
    [[nodiscard]] auto take(Completion& completion) noexcept -> libk::RingResult {
        Completions::Entry entry{};
        const auto result = completions_.pop(entry);
        if (result == libk::RingResult::Ready) completion = Completion::decode(entry);
        return result;
    }
    [[nodiscard]] auto release() noexcept -> bool { return completions_.release(); }

private:
    Submissions::Producer submissions_;
    Completions::Consumer completions_;
    uint64_t next_id_{};
};

enum class Admission : uint8_t { Ready, Empty, Backpressure, Closed, InvalidPeer };

struct Ticket final {
    size_t slot{};
    uint64_t id{};
};

// One service execution owns admission, cancellation and completion. Device
// callbacks carry a Ticket, never a borrowed pointer to a reusable cell. The
// request snapshot is the authority for validation/dispatch; rings only carry
// transport state. This class does not grant buffer or file authority.
class ServerQueue final : private libk::noncopyable_nonmovable {
public:
    ServerQueue(const ClientPage& client, ServerPage& server) noexcept
        : submissions_(client.submissions, server.submissions),
          completions_(server.completions, client.completions) {}

    [[nodiscard]] auto admit(Ticket& ticket) noexcept -> Admission {
        if (stopping_) return Admission::Closed;
        if (!completions_.refresh()) return invalid_peer();
        const auto retained = completions_.produced() - completions_.acknowledged();
        // Every admitted operation reserves its terminal CQ cell until that
        // completion is consumed. Pending storage is therefore also bounded.
        if (retained + active_ == QueueDepth) return Admission::Backpressure;
        Submissions::Entry wire{};
        const auto result = submissions_.pop(wire);
        if (result == libk::RingResult::Empty) return Admission::Empty;
        if (result != libk::RingResult::Ready) return invalid_peer();
        const Request request = Request::decode(wire);
        // IDs increase across the entire session, including completed work.
        // This makes delayed cancellation/completion unable to hit a reuse.
        if (request.id <= last_id_) return invalid_peer();
        for (size_t slot = 0; slot < QueueDepth; ++slot) {
            if (pending_[slot].active) continue;
            pending_[slot] = Pending{request, true, false};
            ++active_;
            last_id_ = request.id;
            ticket = {slot, request.id};
            return Admission::Ready;
        }
        // Only local bookkeeping could violate the reserved-cell invariant.
        __builtin_trap();
    }

    [[nodiscard]] auto request(Ticket ticket) const noexcept -> const Request* {
        return valid(ticket) ? &pending_[ticket.slot].request : nullptr;
    }
    [[nodiscard]] auto cancel(uint64_t id, Ticket& ticket) noexcept -> bool {
        for (size_t slot = 0; slot < QueueDepth; ++slot) {
            auto& pending = pending_[slot];
            if (pending.active && pending.request.id == id) {
                pending.cancelled = true;
                ticket = {slot, id};
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] auto cancelled(Ticket ticket) const noexcept -> bool {
        return valid(ticket) && pending_[ticket.slot].cancelled;
    }

    // Caller first ends all backend and buffer access. A cancellation marker
    // alone is never sufficient. A stale or duplicate Ticket changes nothing.
    [[nodiscard]] auto finish(Ticket ticket, int64_t status, uint64_t bytes) noexcept -> bool {
        if (!valid(ticket)) return false;
        const Completion completion{ticket.id, status, bytes, 0};
        if (completions_.push(completion.encode()) != libk::RingResult::Ready) {
            stopping_ = true;
            return false;
        }
        pending_[ticket.slot] = {};
        --active_;
        return true;
    }
    [[nodiscard]] auto publish() noexcept -> bool { return completions_.publish(); }
    [[nodiscard]] auto release() noexcept -> bool { return submissions_.release(); }
    void stop() noexcept { stopping_ = true; }
    [[nodiscard]] auto active() const noexcept -> size_t { return active_; }

    // A failed/dead peer cannot consume completions. Its session owner first
    // drains backend access (including IOSpace close when required), then drops
    // each exact request through this path. It is not a successful completion.
    [[nodiscard]] auto abandon(Ticket ticket) noexcept -> bool {
        if (!stopping_ || !valid(ticket)) return false;
        pending_[ticket.slot] = {};
        --active_;
        return true;
    }

private:
    struct Pending final {
        Request request{};
        bool active{};
        bool cancelled{};
    };
    [[nodiscard]] auto valid(Ticket ticket) const noexcept -> bool {
        return ticket.slot < QueueDepth && pending_[ticket.slot].active
            && pending_[ticket.slot].request.id == ticket.id;
    }
    [[nodiscard]] auto invalid_peer() noexcept -> Admission {
        stopping_ = true;
        return Admission::InvalidPeer;
    }

    Submissions::Consumer submissions_;
    Completions::Producer completions_;
    Pending pending_[QueueDepth]{};
    uint64_t last_id_{};
    size_t active_{};
    bool stopping_{};
};

} // namespace myos::io
