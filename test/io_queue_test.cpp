#include <user/lib/io_queue.hpp>

#include <cstdio>

namespace {
using namespace myos::io;
using Result = libk::RingResult;

struct Session final {
    ClientPage client_page{};
    ServerPage server_page{};
    ClientQueue client{client_page, server_page};
    ServerQueue server{client_page, server_page};
};

auto completion_credits() -> bool {
    Session session{};
    Ticket tickets[QueueDepth]{};
    for (size_t i = 0; i < QueueDepth; ++i) {
        Request request{.operation = static_cast<uint64_t>(Operation::Read),
                        .offset = i * 512, .length = 512};
        if (session.client.submit(request) != Result::Ready || request.id != i + 1)
            return false;
    }
    if (!session.client.publish()) return false;
    for (size_t i = 0; i < QueueDepth; ++i) {
        if (session.server.admit(tickets[i]) != Admission::Ready) return false;
    }
    if (!session.server.release()) return false;
    Request extra{.operation = static_cast<uint64_t>(Operation::Read), .length = 512};
    if (session.client.submit(extra) != Result::Ready || !session.client.publish()) return false;
    Ticket next{};
    if (session.server.admit(next) != Admission::Backpressure) return false;

    // Complete in reverse order. Every operation already owns its CQ credit;
    // an unread full CQ must retain backpressure even when no backend is busy.
    for (size_t i = QueueDepth; i != 0; --i) {
        if (!session.server.finish(tickets[i - 1], MYOS_STATUS_OK, 512)) return false;
    }
    if (session.server.active() != 0 || !session.server.publish()
        || session.server.admit(next) != Admission::Backpressure) return false;
    Completion completion{};
    if (session.client.take(completion) != Result::Ready || completion.id != QueueDepth
        || session.server.admit(next) != Admission::Backpressure) return false;
    if (!session.client.release() || session.server.admit(next) != Admission::Ready
        || next.id != QueueDepth + 1) return false;
    if (session.server.finish(tickets[0], 0, 512)) return false;
    if (!session.server.finish(next, 0, 512) || !session.server.publish()) return false;
    for (size_t i = QueueDepth - 1; i != 0; --i) {
        if (session.client.take(completion) != Result::Ready || completion.id != i
            || completion.bytes != 512 || completion.status != 0) return false;
    }
    return session.client.take(completion) == Result::Ready
        && completion.id == QueueDepth + 1
        && session.client.take(completion) == Result::Empty;
}

auto cancellation_and_close() -> bool {
    Session session{};
    Request request{.operation = static_cast<uint64_t>(Operation::Read), .length = 4096};
    if (session.client.submit(request) != Result::Ready || !session.client.publish()) return false;
    Ticket ticket{}, cancelled{};
    if (session.server.admit(ticket) != Admission::Ready
        || !session.server.cancel(request.id, cancelled)
        || cancelled.id != ticket.id || cancelled.slot != ticket.slot
        || !session.server.cancelled(ticket)) return false;
    // Receipt of cancellation has not ended the borrow or produced a result.
    Completion completion{};
    if (session.server.active() != 1 || session.server.request(ticket) == nullptr
        || session.client.take(completion) != Result::Empty) return false;
    if (!session.server.finish(ticket, MYOS_STATUS_CANCELED, 0)
        || !session.server.publish()
        || session.server.cancel(request.id, cancelled)
        || session.server.finish(ticket, 0, 4096)) return false;
    if (session.client.take(completion) != Result::Ready
        || completion.id != request.id || completion.status != MYOS_STATUS_CANCELED)
        return false;
    if (!session.client.release() || !session.server.release()) return false;
    if (session.client.submit(request) != Result::Ready || !session.client.publish()
        || session.server.admit(cancelled) != Admission::Ready) return false;
    if (session.server.request(ticket) != nullptr || session.server.abandon(cancelled)) return false;
    session.server.stop();
    Ticket ignored{};
    return session.server.admit(ignored) == Admission::Closed
        && !session.server.abandon(ticket)
        && session.server.abandon(cancelled)
        && !session.server.abandon(cancelled) && session.server.active() == 0;
}

auto hostile_session() -> bool {
    Session session{};
    Submissions::Producer peer{session.client_page.submissions, session.server_page.submissions};
    Request request{.id = 7, .operation = 1, .offset = 512, .length = 512};
    if (peer.push(request.encode()) != Result::Ready || !peer.publish()) return false;
    Ticket ticket{};
    if (session.server.admit(ticket) != Admission::Ready) return false;
    // Mutating shared words after admission cannot change the retained request.
    session.client_page.submissions.entries[0][3].store<libk::MemoryOrder::Relaxed>(UINT64_MAX);
    const auto* retained = session.server.request(ticket);
    if (retained == nullptr || retained->offset != 512) return false;
    if (!session.server.release() || !session.server.finish(ticket, 0, 512)
        || !session.server.publish()) return false;
    if (peer.push(request.encode()) != Result::Ready || !peer.publish()) return false;
    Ticket duplicate{};
    return session.server.admit(duplicate) == Admission::InvalidPeer
        && session.server.admit(duplicate) == Admission::Closed;
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
    std::printf("io-queue: %u passed, %u failed\n", 3 - failed, failed);
    return failed == 0 ? 0 : 1;
}
