#include <utility>
#include <servers/runtime/service.hpp>
#include <servers/runtime/queue.hpp>
#include <sys/start.hpp>
#include <servers/block/device.hpp>
#include <sys/queue.hpp>

namespace {
using namespace sys;
block::Device device;
constexpr size_t Clients = 4;
io::ControlPort directory;
struct Client final {
    cap::OwnedCap channel;
    io::ServerSession session;
};
Client clients[Clients];
// Device slots and client queue slots have independent lifetimes. Keep the
// exact client Ticket until hardware returns the corresponding device Ticket.
struct Pending final { Client* client{}; io::Ticket ticket{}; uint64_t generation{}; };
Pending pending[io::QueueDepth];
struct Flush final { Client* client{}; io::Ticket ticket{}; bool submitted{}; } flush;
void control(const io::ControlMessage&, io::ControlReply& reply) noexcept {
    reply.message.status = STATUS_INVALID_OP;
}

auto validate(const io::Request& request) noexcept -> status_t {
    if (request.object != 0 || request.buffer != 0 || request.flags != 0)
        return STATUS_BAD_ARGS;
    if (request.operation == static_cast<uint64_t>(io::Operation::Flush))
        return request.offset == 0 && request.length == 0 && request.buffer_offset == 0
            ? STATUS_OK : STATUS_BAD_ARGS;
    if (request.operation == static_cast<uint64_t>(io::Operation::Identify))
        return request.offset == 0 && request.length == 20 && request.buffer_offset == 0
            ? STATUS_OK : STATUS_BAD_ARGS;
    if (request.operation != static_cast<uint64_t>(io::Operation::Read)
        && request.operation != static_cast<uint64_t>(io::Operation::Write))
        return STATUS_BAD_ARGS;
    if (request.length == 0 || request.length > io::BufferSize
        || request.buffer_offset > io::PayloadSize
        || request.length > io::PayloadSize - request.buffer_offset)
        return STATUS_BAD_ARGS;
    return STATUS_OK;
}
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    const auto pool = service::capability(info, BOOT_POOL);
    const auto vspace = service::capability(info, BOOT_VSPACE);
    const auto cspace = service::capability(info, BOOT_CSPACE);
    const auto events = service::capability(info, BOOT_EVENTS);
    service::require(device.open(pool, vspace, service::capability(info, BOOT_DEVICE), events));
    service::require(directory.open(service::capability(info, boot::Block), events));
    for (size_t i = 0; i < Clients; ++i)
        service::require(clients[i].session.prepare(pool, vspace, cspace,
            0x70000000 + i * 0x100000, true));
    size_t turn{};
    for (;;) {
        bool again{};
        service::require(device.acknowledge());
        for (size_t count = 0; count < Clients; ++count) {
            io::ControlPacket packet;
            const auto status = directory.receive(packet);
            if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) break;
            if (status != STATUS_OK || packet.count != 2) continue;
            auto endpoint = std::move(packet.capabilities[0]);
            packet.capabilities[0] = std::move(packet.capabilities[1]);
            packet.count = 1;
            Client* available{};
            for (auto& client : clients) if (!client.channel) { available = &client; break; }
            if (!available) {
                const io::ControlMessage reply{.operation = packet.message.operation, .id = packet.message.id,
                    .status = STATUS_NO_MEMORY};
                (void)io::ControlPort::send_to(endpoint.selector(), reply);
                (void)channel_close(endpoint.selector());
                continue;
            }
            auto& client = *available;
            client.channel = std::move(endpoint);
            const auto bound = client.session.bind(client.channel.selector(), events, device.capacity());
            if (bound != STATUS_OK || client.session.accept(packet, control) != STATUS_OK)
                client.session.abort();
        }
        for (auto& client : clients) if (client.channel) {
            if (client.session.poll(control) != STATUS_OK) {
                client.session.abort();
                again = true;
            }
        }
        for (size_t count = 0; device.active() && count < io::QueueDepth; ++count) {
            block::Device::Completion completion;
            const auto status = device.take(completion);
            if (status == STATUS_WOULD_BLOCK) break;
            service::require(status);
            if (completion.ticket.slot >= io::QueueDepth) exit(STATUS_INTERNAL);
            auto& operation = pending[completion.ticket.slot];
            if (!operation.client || operation.generation != completion.ticket.id) exit(STATUS_INTERNAL);
            auto& session = operation.client->session;
            auto& queue = *session.queue();
            const auto* request = queue.request(operation.ticket);
            if (!request) exit(STATUS_INTERNAL);
            const bool cancelled = queue.cancelled(operation.ticket);
            if (!session.failed() && !cancelled && completion.status == STATUS_OK
                && (request->operation == static_cast<uint64_t>(io::Operation::Read)
                    || request->operation == static_cast<uint64_t>(io::Operation::Identify)))
                service::copy(session.payload() + request->buffer_offset, completion.data, completion.size);
            if (session.failed() || !queue.finish(operation.ticket,
                cancelled ? STATUS_CANCELED : completion.status, cancelled ? 0 : completion.size)) {
                session.abort();
                if (!queue.abandon(operation.ticket)) exit(STATUS_INTERNAL);
            }
            if (flush.client && flush.submitted && flush.client == operation.client
                && flush.ticket.id == operation.ticket.id) flush = {};
            operation.client = nullptr;
        }
        if (flush.client && !flush.submitted) {
            auto& session = flush.client->session;
            auto& queue = *session.queue();
            if (session.failed() || queue.cancelled(flush.ticket)) {
                if (!queue.finish(flush.ticket, STATUS_CANCELED, 0)) {
                    session.abort();
                    if (!queue.abandon(flush.ticket)) exit(STATUS_INTERNAL);
                }
                flush = {};
            } else if (device.active() == 0) {
                size_t slot{};
                while (slot < io::QueueDepth && (pending[slot].client || pending[slot].generation == UINT64_MAX)) ++slot;
                if (slot != io::QueueDepth) {
                    auto& operation = pending[slot];
                    const io::Ticket device_ticket{static_cast<uint32_t>(slot), ++operation.generation};
                    const auto status = device.submit(device_ticket, io::Operation::Flush, 0, 0);
                    if (status == STATUS_OK) {
                        if (!queue.commit(flush.ticket)) exit(STATUS_INTERNAL);
                        operation.client = flush.client;
                        operation.ticket = flush.ticket;
                        flush.submitted = true;
                    } else {
                        if (!queue.finish(flush.ticket, status, 0)) {
                            session.abort();
                            if (!queue.abandon(flush.ticket)) exit(STATUS_INTERNAL);
                        }
                        flush = {};
                    }
                }
            }
        }
        // Interleave admission across clients; each batch and DMA working set
        // is bounded by the device queue, independent of client queue depth.
        for (size_t row = 0; row < io::QueueDepth; ++row) {
            if (flush.client) break;
            for (size_t n = 0; n < Clients; ++n) {
                auto& client = clients[(turn + n) % Clients];
                auto* queue = client.session.queue();
                if (!client.channel || !queue || client.session.closing()) continue;
                size_t slot{};
                while (slot < io::QueueDepth && (pending[slot].client || pending[slot].generation == UINT64_MAX)) ++slot;
                if (slot == io::QueueDepth) break;
                io::Ticket ticket{};
                const auto admitted = queue->admit(ticket);
                if (admitted == io::Admission::Empty || admitted == io::Admission::Backpressure) continue;
                if (admitted != io::Admission::Ready) { client.session.abort(); continue; }
                const auto& request = *queue->request(ticket);
                auto status = validate(request);
                if (status == STATUS_OK
                    && request.operation == static_cast<uint64_t>(io::Operation::Flush)) {
                    flush = {&client, ticket, false};
                    again = true;
                    break;
                }
                auto& operation = pending[slot];
                if (status == STATUS_OK) {
                    const io::Ticket device_ticket{static_cast<uint32_t>(slot), ++operation.generation};
                    const auto kind = static_cast<io::Operation>(request.operation);
                    const auto* source = kind == io::Operation::Write
                        ? client.session.payload() + request.buffer_offset : nullptr;
                    status = device.submit(device_ticket, kind, request.offset, request.length, source);
                }
                if (status == STATUS_OK) {
                    if (request.operation == static_cast<uint64_t>(io::Operation::Write)
                        && !queue->commit(ticket)) exit(STATUS_INTERNAL);
                    operation.client = &client;
                    operation.ticket = ticket;
                }
                else if (!queue->finish(ticket, status, 0)) {
                    client.session.abort();
                    if (!queue->abandon(ticket)) exit(STATUS_INTERNAL);
                }
            }
        }
        turn = (turn + 1) % Clients;
        device.publish();
        for (auto& client : clients) if (client.channel) {
            // Session close drains its DMA and withdraws peer mappings. The
            // device remains owned by the driver and can serve other clients.
            if (client.session.flush(true) != STATUS_OK) { client.session.abort(); again = true; }
            if (client.session.done()) {
                (void)channel_close(client.channel.selector());
                client.channel = {};
                client.session.reset();
            } else if (client.session.arm() != STATUS_OK) { client.session.abort(); again = true; }
        }
        service::require(directory.arm());
        if (!again) service::require(notification_wait(events).status);
    }
}
