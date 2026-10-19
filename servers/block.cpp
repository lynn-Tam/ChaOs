#include <util.hpp>
#include <string.h>
#include <protocols/block.hpp>
#include <utility>
#include <new>
#include <memory>
#include <protocols/msg.hpp>
#include <protocols/queue.hpp>
#include <start.hpp>
#include <servers/virtio_blk.hpp>
#include <protocols/service.hpp>

static blk_dev device;
static msg_rx directory;
struct Client final {
    std::unique_ptr<Client> next;
    cap_owner channel;
    io_server session;
};
static std::unique_ptr<Client> clients;
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
static void control(const msg_frame&, msg_reply& reply) noexcept {
    reply.message.status = STATUS_INVALID_OP;
}

static auto validate(const io_cmd& request) noexcept -> status_t {
    if (request.flags != 0)
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
    const auto vspace = info.vspace;
    const auto cspace = info.cspace;
    const auto events = info.events;
    const CreateAuth memory_pool{info.selector(io_memory), info.selector(io_scope)};
    if (!memory_pool.account || !memory_pool.scope)
        sys::exit(STATUS_DENIED);
    if (const auto rc = device.open(info);
        rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = directory.open(info.selector(blk_service), events); rc != STATUS_OK)
        sys::exit(rc);
    if (const auto ready = info.selector(sv_ready)) {
        if (const auto status = sys::notification_signal(ready).status; status != STATUS_OK) sys::exit(status);
    }
    for (;;) {
        bool again{};
        if (const auto rc = device.acknowledge(); rc != STATUS_OK)
            sys::exit(rc);
        for (size_t count = 0; count < io_queue_depth; ++count) {
            msg_packet packet;
            const auto received = directory.receive(packet);
            if (received == STATUS_WOULD_BLOCK || received == STATUS_BUSY)
                break;
            if (received != STATUS_OK || packet.count != 3)
                continue;
            auto endpoint = std::move(packet.capabilities[1]);
            packet.capabilities[1] = std::move(packet.capabilities[2]);
            packet.count = 2;
            if (const auto status = rt_admit(packet.message.deadline); status != STATUS_OK) {
                msg_reply reply;
                reply.message = {.operation = packet.message.operation,
                                 .status = status, .sequence = packet.message.sequence};
                reply.respond(packet.capabilities[0].selector());
                continue;
            }
            auto client = std::unique_ptr<Client>{new (std::nothrow) Client};
            const auto status = client ? client->session.open(endpoint.selector(), events, memory_pool, info.alloc(),
                                                              vspace, cspace, 0, device.capacity(), true)
                                       : STATUS_NO_MEMORY;
            if (status != STATUS_OK) {
                const msg_frame reply{.operation = packet.message.operation,
                                   .status = status, .sequence = packet.message.sequence};
                msg_reply response;
                response.message = reply;
                response.respond(packet.capabilities[0].selector());
                continue;
            }
            client->channel = std::move(endpoint);
            if (client->session.accept(packet, control) != STATUS_OK)
                client->session.abort();
            auto* tail = &clients;
            while (*tail) tail = &(*tail)->next;
            *tail = std::move(client);
        }
        for (auto* client = clients.get(); client; client = client->next.get())
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
            } else if (const auto status = rt_admit(queue.request(flush.ticket)->deadline);
                       status != STATUS_OK) {
                if (!queue.finish(flush.ticket, status, 0))
                    sys::exit(STATUS_INTERNAL);
                flush = {};
            } else if (device.active() == 0) {
                size_t slot{};
                while (slot < device.depth() &&
                       (pending[slot].client || pending[slot].generation == UINT64_MAX))
                    ++slot;
                if (slot != device.depth()) {
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
            for (auto* owner = clients.get(); owner; owner = owner->next.get()) {
                auto& client = *owner;
                auto* queue = client.session.queue();
                if (!queue || client.session.closing())
                    continue;
                size_t slot{};
                while (slot < device.depth() &&
                       (pending[slot].client || pending[slot].generation == UINT64_MAX))
                    ++slot;
                if (slot == device.depth())
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
                auto status = rt_admit(request.deadline);
                if (status == STATUS_OK)
                    status = validate(request);
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
        device.publish();
        for (auto* link = &clients; *link;) {
            auto& client = **link;
            // Session close drains its DMA and withdraws peer mappings. The
            // device remains owned by the driver and can serve other clients.
            if (client.session.flush(true) != STATUS_OK) {
                client.session.abort();
                again = true;
            }
            if (client.session.done()) {
                (void) sys::channel_close(client.channel.selector());
                client.session.reset();
                auto retired = std::move(*link);
                *link = std::move(retired->next);
                continue;
            } else if (client.session.arm() != STATUS_OK) {
                client.session.abort();
                again = true;
            }
            link = &client.next;
        }
        // Rotate ownership instead of keeping a second client index/size.
        if (clients && clients->next) {
            auto first = std::move(clients);
            clients = std::move(first->next);
            auto* tail = &clients;
            while (*tail) tail = &(*tail)->next;
            *tail = std::move(first);
        }
        if (const auto rc = directory.arm().status; rc != STATUS_OK)
            sys::exit(rc);
        if (!again) {
            const auto deadline =
                flush.client && !flush.submitted
                    ? flush.client->session.queue()->request(flush.ticket)->deadline
                    : 0;
            const auto status = ev_wait(events, deadline).status;
            if (status != STATUS_OK && status != STATUS_TIMED_OUT)
                sys::exit(status);
        }
    }
}
