#include <util.hpp>
#include <string.h>
#include <protocols/block.hpp>
#include <utility>
#include <vector>
#include <memory>
#include <ipc.hpp>
#include <protocols/queue.hpp>
#include <start.hpp>
#include <servers/virtio_blk.hpp>

static blk_dev device;
static ch_port directory;
struct Client final {
    cap_owner channel;
    io_server session;
};
static std::vector<std::unique_ptr<Client>> clients;
// Device slots and client queue slots have independent lifetimes. Keep the
// exact client Ticket until hardware returns the corresponding device Ticket.
struct Pending final {
    Client* client{};
    io_ticket ticket{};
    uint64_t generation{};
};
Pending pending[io_queue_depth];
struct Flush final {
    Client* client{};
    io_ticket ticket{};
    bool submitted{};
} flush;
static void control(const ch_msg&, ch_reply& reply) noexcept {
    reply.message.status = STATUS_INVALID_OP;
}

static auto validate(const io_cmd& request) noexcept -> status_t {
    if (request.object != 0 || request.buffer != 0 || request.flags != 0)
        return STATUS_BAD_ARGS;
    if (request.operation == static_cast<uint64_t>(io_operation::Flush))
        return request.offset == 0 && request.length == 0 && request.buffer_offset == 0
                   ? STATUS_OK
                   : STATUS_BAD_ARGS;
    if (request.operation == static_cast<uint64_t>(io_operation::Identify))
        return request.offset == 0 && request.length == 20 && request.buffer_offset == 0
                   ? STATUS_OK
                   : STATUS_BAD_ARGS;
    if (request.operation != static_cast<uint64_t>(io_operation::Read) &&
        request.operation != static_cast<uint64_t>(io_operation::Write))
        return STATUS_BAD_ARGS;
    if (request.length == 0 || request.length > io_buffer_size ||
        request.buffer_offset > io_payload_size ||
        request.length > io_payload_size - request.buffer_offset)
        return STATUS_BAD_ARGS;
    return STATUS_OK;
}

int main() {

    const auto& info = rt_startup();
    const auto pool = info.pool;
    const auto vspace = info.vspace;
    const auto cspace = info.cspace;
    const auto events = info.events;
    const auto memory_pool = info.selector(io_memory);
    if (!memory_pool) sys::exit(STATUS_DENIED);
    if (const auto rc = device.open(pool, vspace, info.selector(io_host), events, rt_event_badge);
        rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = directory.open(info.selector(blk_service), events); rc != STATUS_OK)
        sys::exit(rc);
    size_t turn{};
    for (;;) {
        bool again{};
        if (const auto rc = device.acknowledge(); rc != STATUS_OK)
            sys::exit(rc);
        for (size_t count = 0; count < io_queue_depth; ++count) {
            ch_packet packet;
            const auto received = directory.receive(packet);
            if (received == STATUS_WOULD_BLOCK || received == STATUS_BUSY)
                break;
            if (received != STATUS_OK || packet.count != 2)
                continue;
            auto endpoint = std::move(packet.capabilities[0]);
            packet.capabilities[0] = std::move(packet.capabilities[1]);
            packet.count = 1;
            auto client = std::make_unique<Client>();
            const auto status = client->session.open(endpoint.selector(), events, memory_pool, vspace,
                                                     cspace, 0, device.capacity(), true);
            if (status != STATUS_OK) {
                const ch_msg reply{.operation = packet.message.operation,
                                   .id = packet.message.id,
                                   .status = status};
                (void) ch_port::send_to(endpoint.selector(), reply);
                continue;
            }
            client->channel = std::move(endpoint);
            if (client->session.accept(packet, control) != STATUS_OK)
                client->session.abort();
            clients.push_back(std::move(client));
        }
        for (auto& client : clients)
            if (client->session.poll(control) != STATUS_OK) {
                client->session.abort();
                again = true;
            }
        for (size_t count = 0; device.active() && count < io_queue_depth; ++count) {
            blk_dev::Completion completion;
            const auto status = device.take(completion);
            if (status == STATUS_WOULD_BLOCK)
                break;
            if (status != STATUS_OK)
                sys::exit(status);
            if (completion.ticket.slot >= io_queue_depth)
                sys::exit(STATUS_INTERNAL);
            auto& operation = pending[completion.ticket.slot];
            if (!operation.client || operation.generation != completion.ticket.id)
                sys::exit(STATUS_INTERNAL);
            auto& session = operation.client->session;
            auto& queue = *session.queue();
            const auto* request = queue.request(operation.ticket);
            if (!request)
                sys::exit(STATUS_INTERNAL);
            const bool cancelled = queue.cancelled(operation.ticket);
            if (!session.failed() && !cancelled && completion.status == STATUS_OK &&
                (request->operation == static_cast<uint64_t>(io_operation::Read) ||
                 request->operation == static_cast<uint64_t>(io_operation::Identify)))
                ::memcpy(session.payload() + request->buffer_offset, completion.data,
                         completion.size);
            if (session.failed() ||
                !queue.finish(operation.ticket, cancelled ? STATUS_CANCELED : completion.status,
                              cancelled ? 0 : completion.size)) {
                session.abort();
                if (!queue.abandon(operation.ticket))
                    sys::exit(STATUS_INTERNAL);
            }
            if (flush.client && flush.submitted && flush.client == operation.client &&
                flush.ticket.id == operation.ticket.id)
                flush = {};
            operation.client = nullptr;
        }
        if (flush.client && !flush.submitted) {
            auto& session = flush.client->session;
            auto& queue = *session.queue();
            if (session.failed() || queue.cancelled(flush.ticket)) {
                if (!queue.finish(flush.ticket, STATUS_CANCELED, 0)) {
                    session.abort();
                    if (!queue.abandon(flush.ticket))
                        sys::exit(STATUS_INTERNAL);
                }
                flush = {};
            } else if (device.active() == 0) {
                size_t slot{};
                while (slot < io_queue_depth &&
                       (pending[slot].client || pending[slot].generation == UINT64_MAX))
                    ++slot;
                if (slot != io_queue_depth) {
                    auto& operation = pending[slot];
                    const io_ticket device_ticket{static_cast<uint32_t>(slot),
                                                  ++operation.generation};
                    const auto status = device.submit(device_ticket, io_operation::Flush, 0, 0);
                    if (status == STATUS_OK) {
                        if (!queue.commit(flush.ticket))
                            sys::exit(STATUS_INTERNAL);
                        operation.client = flush.client;
                        operation.ticket = flush.ticket;
                        flush.submitted = true;
                    } else {
                        if (!queue.finish(flush.ticket, status, 0)) {
                            session.abort();
                            if (!queue.abandon(flush.ticket))
                                sys::exit(STATUS_INTERNAL);
                        }
                        flush = {};
                    }
                }
            }
        }
        // Interleave admission across clients; each batch and DMA working set
        // is bounded by the device queue, independent of client queue depth.
        for (size_t row = 0; row < io_queue_depth; ++row) {
            if (flush.client)
                break;
            for (size_t n = 0; n < clients.size(); ++n) {
                auto& client = *clients[(turn + n) % clients.size()];
                auto* queue = client.session.queue();
                if (!queue || client.session.closing())
                    continue;
                size_t slot{};
                while (slot < io_queue_depth &&
                       (pending[slot].client || pending[slot].generation == UINT64_MAX))
                    ++slot;
                if (slot == io_queue_depth)
                    break;
                io_ticket ticket{};
                const auto admitted = queue->admit(ticket);
                if (admitted == io_admission::Empty || admitted == io_admission::Backpressure)
                    continue;
                if (admitted != io_admission::Ready) {
                    client.session.abort();
                    continue;
                }
                const auto& request = *queue->request(ticket);
                auto status = validate(request);
                if (status == STATUS_OK &&
                    request.operation == static_cast<uint64_t>(io_operation::Flush)) {
                    flush = {&client, ticket, false};
                    again = true;
                    break;
                }
                auto& operation = pending[slot];
                if (status == STATUS_OK) {
                    const io_ticket device_ticket{static_cast<uint32_t>(slot),
                                                  ++operation.generation};
                    const auto kind = static_cast<io_operation>(request.operation);
                    const auto* source = kind == io_operation::Write
                                             ? client.session.payload() + request.buffer_offset
                                             : nullptr;
                    status =
                        device.submit(device_ticket, kind, request.offset, request.length, source);
                }
                if (status == STATUS_OK) {
                    if (request.operation == static_cast<uint64_t>(io_operation::Write) &&
                        !queue->commit(ticket))
                        sys::exit(STATUS_INTERNAL);
                    operation.client = &client;
                    operation.ticket = ticket;
                } else if (!queue->finish(ticket, status, 0)) {
                    client.session.abort();
                    if (!queue->abandon(ticket))
                        sys::exit(STATUS_INTERNAL);
                }
            }
        }
        turn = clients.empty() ? 0 : (turn + 1) % clients.size();
        device.publish();
        for (auto& owner : clients) {
            auto& client = *owner;
            // Session close drains its DMA and withdraws peer mappings. The
            // device remains owned by the driver and can serve other clients.
            if (client.session.flush(true) != STATUS_OK) {
                client.session.abort();
                again = true;
            }
            if (client.session.done()) {
                (void) sys::channel_close(client.channel.selector());
                client.session.reset();
                client.channel = {};
            } else if (client.session.arm() != STATUS_OK) {
                client.session.abort();
                again = true;
            }
        }
        std::erase_if(clients, [](const auto& client) { return !client->channel; });
        if (const auto rc = directory.arm().status; rc != STATUS_OK)
            sys::exit(rc);
        if (!again)
            if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
                sys::exit(rc);
    }
}
