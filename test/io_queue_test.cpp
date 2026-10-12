#include <io.hpp>

#include <cstdio>

namespace {
using Result = libk::ring_status;

struct Session final {
    io_tx_page client_page{};
    io_rx_page server_page{};
    io_tx client{client_page, server_page};
    io_rx server{client_page, server_page};
};

auto completion_credits() -> bool {
    Session session{};
    io_ticket tickets[io_queue_depth]{};
    for (size_t i = 0; i < io_queue_depth; ++i) {
        io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                        .offset = i * 512, .length = 512};
        if (session.client.submit(request) != Result::ready || request.id != i + 1)
            return false;
    }
    if (!session.client.publish()) return false;
    for (size_t i = 0; i < io_queue_depth; ++i) {
        if (session.server.admit(tickets[i]) != io_admission::Ready) return false;
    }
    if (!session.server.release()) return false;
    io_cmd extra{.operation = static_cast<uint64_t>(io_operation::Read), .length = 512};
    if (session.client.submit(extra) != Result::ready || !session.client.publish()) return false;
    io_ticket next{};
    if (session.server.admit(next) != io_admission::Backpressure) return false;

    // Complete in reverse order. Every operation already owns its CQ credit;
    // an unread full CQ must retain backpressure even when no backend is busy.
    for (size_t i = io_queue_depth; i != 0; --i) {
        if (!session.server.finish(tickets[i - 1], STATUS_OK, 512)) return false;
    }
    if (session.server.active() != 0 || !session.server.publish()
        || session.server.admit(next) != io_admission::Backpressure) return false;
    io_done completion{};
    if (session.client.take(completion) != Result::ready || completion.id != io_queue_depth
        || session.server.admit(next) != io_admission::Backpressure) return false;
    if (!session.client.release() || session.server.admit(next) != io_admission::Ready
        || next.id != io_queue_depth + 1) return false;
    if (session.server.finish(tickets[0], 0, 512)) return false;
    if (!session.server.finish(next, 0, 512) || !session.server.publish()) return false;
    for (size_t i = io_queue_depth - 1; i != 0; --i) {
        if (session.client.take(completion) != Result::ready || completion.id != i
            || completion.bytes != 512 || completion.status != 0) return false;
    }
    return session.client.take(completion) == Result::ready
        && completion.id == io_queue_depth + 1
        && session.client.take(completion) == Result::empty;
}

auto cancellation_and_close() -> bool {
    Session session{};
    io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read), .length = 4096};
    if (session.client.submit(request) != Result::ready || !session.client.publish()) return false;
    io_ticket ticket{}, cancelled{};
    if (session.server.admit(ticket) != io_admission::Ready
        || session.server.cancel(request.id, cancelled) != STATUS_OK
        || cancelled.id != ticket.id || cancelled.slot != ticket.slot
        || !session.server.cancelled(ticket) || session.server.commit(ticket)) return false;
    // Receipt of cancellation has not ended the borrow or produced a result.
    io_done completion{};
    if (session.server.active() != 1 || session.server.request(ticket) == nullptr
        || session.client.take(completion) != Result::empty) return false;
    if (!session.server.finish(ticket, STATUS_CANCELED, 0)
        || !session.server.publish()
        || session.server.cancel(request.id, cancelled) != STATUS_NOT_FOUND
        || session.server.finish(ticket, 0, 4096)) return false;
    if (session.client.take(completion) != Result::ready
        || completion.id != request.id || completion.status != STATUS_CANCELED)
        return false;
    if (!session.client.release() || !session.server.release()) return false;
    if (session.client.submit(request) != Result::ready || !session.client.publish()
        || session.server.admit(cancelled) != io_admission::Ready) return false;
    if (session.server.request(ticket) != nullptr || session.server.abandon(cancelled)
        || !session.server.commit(cancelled)
        || session.server.cancel(request.id, ticket) != STATUS_BUSY
        || session.server.cancelled(cancelled)) return false;
    session.server.stop();
    io_ticket ignored{};
    return session.server.admit(ignored) == io_admission::Closed
        && !session.server.abandon(ticket)
        && session.server.abandon(cancelled)
        && !session.server.abandon(cancelled) && session.server.active() == 0;
}

auto hostile_session() -> bool {
    Session session{};
    io_submissions::producer peer{session.client_page.submissions, session.server_page.submissions};
    io_cmd request{.id = 7, .operation = 1, .offset = 512, .length = 512};
    if (peer.push(request.encode()) != Result::ready || !peer.publish()) return false;
    io_ticket ticket{};
    if (session.server.admit(ticket) != io_admission::Ready) return false;
    // Mutating shared words after admission cannot change the retained request.
    session.client_page.submissions.entries[0][3].store(UINT64_MAX, std::memory_order_relaxed);
    const auto* retained = session.server.request(ticket);
    if (retained == nullptr || retained->offset != 512) return false;
    if (!session.server.release() || !session.server.finish(ticket, 0, 512)
        || !session.server.publish()) return false;
    if (peer.push(request.encode()) != Result::ready || !peer.publish()) return false;
    io_ticket duplicate{};
    return session.server.admit(duplicate) == io_admission::InvalidPeer
        && session.server.admit(duplicate) == io_admission::Closed;
}

auto buffered_requests() -> bool {
    Session session{};
    std::array<uint8_t, io_payload_size> payload{};
    io_requests transfers{session.client, payload.data(), payload.data()};
    uint8_t data[8]{}, value = 'W';
    io_cmd write{.operation = static_cast<uint64_t>(io_operation::Write), .length = 1};
    io_cmd read{.operation = static_cast<uint64_t>(io_operation::Read), .length = sizeof(data)};
    if (transfers.submit(write, &value, true) || transfers.submit(read, data)
        || write.buffer_offset == read.buffer_offset || payload[write.buffer_offset] != value
        || !session.client.publish()) return false;
    io_ticket a{}, b{};
    if (session.server.admit(a) != io_admission::Ready || session.server.admit(b) != io_admission::Ready)
        return false;
    payload[read.buffer_offset] = 'R';
    if (!session.server.finish(b, 0, 1) || !session.server.finish(a, 0, 1)
        || !session.server.publish()) return false;
    io_done result{};
    if (transfers.take(result, write.id) || result.id != write.id || result.bytes != 1
        || transfers.contains(write.id) || !transfers.contains(read.id)
        || transfers.take(result) || result.id != read.id || result.bytes != 1
        || data[0] != 'R' || data[1] != 0 || transfers.next()) return false;
    if (!session.client.release() || !session.server.release()) return false;
    if (transfers.submit(read, data) || transfers.fail(STATUS_PEER_FAULT) != STATUS_PEER_FAULT
        || transfers.take(result) || result.id != read.id || result.status != STATUS_PEER_FAULT
        || transfers.submit(write, &value) != STATUS_PEER_FAULT) return false;
    return !transfers.next();
}
} // namespace

int main() {
    unsigned failed{};
    const auto check = [&](const char* name, bool result) {
        if (!result) {
            std::fprintf(stderr, "FAIL io-queue: %s\n", name);
            ++failed;
        }
    };
    check("completion credits and out-of-order results", completion_credits());
    check("cancellation, stale tickets and drained close", cancellation_and_close());
    check("hostile descriptor mutation and duplicate IDs", hostile_session());
    check("buffer slots, short reads, retained results and terminal failure", buffered_requests());
    std::printf("io-queue: %u passed, %u failed\n", 4 - failed, failed);
    return failed == 0 ? 0 : 1;
}
