#include "virtio.hpp"

static blk_dev device;
static std::array<uint8_t, io_limit> scratch;
static ubus_object directory{"io.block"};
struct blk_cmd { blk_op kind; uint64_t offset{}; ubus_buffer buffer{}; uint64_t deadline{}; };
struct blk_client {
    vm_ptr<blk_client> next;
    ubus_object object;
    ubus_queue<blk_cmd, blk_dev::Depth> queue;
    std::optional<ubus_request> closing;
    bool stopped{}, failed{};
    void stop() { stopped = failed = true; queue.stop(); }
    void finish(ubus_ticket ticket, status_t status, uint64_t count = 0) {
        ubus_msg reply; reply.status = status; reply.data.put("bytes", count);
        if (!queue.finish(ticket, std::move(reply))) sys::exit(STATUS_INTERNAL);
    }
};
static vm_ptr<blk_client> clients;
struct blk_pending { blk_client* client{}; ubus_ticket ticket{}; uint64_t gen{}; };
static std::array<blk_pending, blk_dev::Depth> pending;
struct blk_flush { blk_client* client{}; ubus_ticket ticket{}; bool submitted{}; };
static blk_flush flush;

static auto open(ubus_request& request, ubus_msg& response) -> status_t {
    const auto budget = request.capability(request.data["budget"]);
    auto storage = vm_new<blk_client>({budget ? budget : rt_startup().quota, rt_startup().scope});
    if (!storage) return storage.error();
    auto client = std::move(*storage);
    auto& object = client->object;
    for (const auto& [name, kind] : std::array<std::pair<std::string_view, blk_op>, 4>{{
        {"Read", blk_op::Read}, {"Write", blk_op::Write}, {"Flush", blk_op::Flush}, {"Identify", blk_op::Identify}}}) {
        ubus_schema input{{"id", ubus_type::U64}};
        if (kind != blk_op::Flush) { input.push_back({"offset", ubus_type::U64}); input.push_back({"buffer", ubus_type::Buffer}); }
        const auto status = object.add(std::string{name}, std::move(input), {{"bytes", ubus_type::U64}},
            [state=client.get(), kind](auto& q, auto&) {
                if (state->stopped) return STATUS_CLOSED;
                blk_cmd command{.kind=kind, .offset=q.data["offset"].template as<uint64_t>().value_or(0), .deadline=q.deadline};
                if (kind != blk_op::Flush) {
                    command.buffer = *q.data["buffer"].template as<ubus_buffer>();
                    if (command.buffer.size > blk_dev::MaxTransfer || command.buffer.offset > UINT64_MAX - command.buffer.size) return STATUS_BAD_ARGS;
                }
                return state->queue.accept(*q.data["id"].template as<uint64_t>(), command, q);
            });
        if (status != STATUS_OK) return status;
    }
    for (const auto status : {
        object.add("Stat", {}, {{"size", ubus_type::U64}, {"sequence", ubus_type::U64}, {"limit", ubus_type::U64}, {"depth", ubus_type::U64}}, [state=client.get()](auto&, auto& r) {
            r.data.put("size", device.capacity()).put("sequence", state->queue.sequence()).put("limit", uint64_t{io_limit}).put("depth", uint64_t{io_depth}); return STATUS_OK;
        }),
        object.add("Cancel", {{"id", ubus_type::U64}}, {}, [state=client.get()](auto& q, auto&) { return state->queue.cancel(*q.data["id"].template as<uint64_t>()); }),
        object.add("Close", {}, {}, [state=client.get()](auto& q, auto&) {
            if (state->stopped) return STATUS_CLOSED;
            state->stopped = true; state->queue.stop(); state->closing.emplace(std::move(q)); return STATUS_PENDING;
        })}) if (status != STATUS_OK) return status;
    auto status = object.open(blk_dev::Depth, request.capability(request.data["budget"])); if (status != STATUS_OK) return status;
    auto handle = object.handle(0, RIGHT_SEND); if (!handle) return handle.error();
    const auto handle_idx = response.give(std::move(*handle));
    if (!handle_idx) return handle_idx.error();
    response.data.cap("handle", *handle_idx);
    client->next = std::move(clients); clients = std::move(client); return STATUS_OK;
}
static auto slot() -> size_t {
    size_t index{};
    while (index < device.depth() && (pending[index].client || pending[index].gen == UINT64_MAX)) ++index;
    return index;
}
int main() {
    const auto& env = rt_startup();
    ubus_conn parent;
    auto status = parent.bind(env.environment); if (status != STATUS_OK) return status;
    std::vector<ubus_msg> setup;
    bool more{};
    do {
        ubus_msg fragment;
        status = parent.invoke("Receive", {}, fragment); if (status != STATUS_OK) return status;
        ubus_data message; if (!message.assign(fragment.data["message"].bytes())) return STATUS_BAD_ARGS;
        more = message["more"].as<bool>().value_or(false); fragment.data = std::move(message);
        setup.push_back(std::move(fragment));
    } while (more);
    status = device.open(env, setup); if (status != STATUS_OK) return status;
    status = directory.add("Open", {{"budget", ubus_type::Cap, true}}, {{"handle", ubus_type::Cap}}, open);
    if (status == STATUS_OK) status = directory.open();
    if (status != STATUS_OK) return status;
    auto connector = directory.connector(); if (!connector) return connector.error();
    ubus_msg publication, response;
    const auto peer_idx = publication.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
    if (!peer_idx) return peer_idx.error();
    publication.data.put("name", "block").put("interface", "io.block").put("version", uint64_t{1})
        .cap("connector", *peer_idx);
    status = parent.invoke("Publish", std::move(publication), response); if (status != STATUS_OK) return status;
    for (;;) {
        bool again{};
        status = device.acknowledge(); if (status != STATUS_OK) return status;
        for (size_t n = 0; n < blk_dev::Depth; ++n) {
            status = directory.poll(); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) return status;
            again = true;
        }
        for (auto* client = clients.get(); client; client = client->next.get()) {
            for (size_t n = 0; n < blk_dev::Depth; ++n) {
                status = client->object.poll(); if (status == STATUS_WOULD_BLOCK) break;
                if (status == STATUS_PEER_CLOSED || status == STATUS_CLOSED) { client->stop(); break; }
                if (status != STATUS_OK) return status;
                again = true;
            }
        }
        for (size_t n = 0; device.active() && n < blk_dev::Depth; ++n) {
            blk_dev::Completion completion;
            status = device.take(completion); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) return status;
            if (completion.ticket.slot >= pending.size()) return STATUS_INTERNAL;
            auto& operation = pending[completion.ticket.slot];
            if (!operation.client || operation.gen != completion.ticket.id) return STATUS_INTERNAL;
            auto& client = *operation.client;
            const auto* command = client.queue.request(operation.ticket); if (!command) return STATUS_INTERNAL;
            status = completion.status;
            if (client.failed || client.queue.cancelled(operation.ticket)) status = STATUS_CANCELED;
            if (status == STATUS_OK && (command->kind == blk_op::Read || command->kind == blk_op::Identify)) {
                if (completion.size > command->buffer.size) return STATUS_INTERNAL;
                auto* call = client.queue.call(operation.ticket);
                status = mem_write(call->capability(call->data["buffer"]), command->buffer.offset,
                    std::as_bytes(std::span{completion.data, completion.size}));
            }
            client.finish(operation.ticket, status, status == STATUS_OK ? completion.size : 0);
            if (flush.client == operation.client && flush.submitted && flush.ticket.id == operation.ticket.id) flush = {};
            operation.client = nullptr; again = true;
        }
        if (flush.client && !flush.submitted) {
            auto& client = *flush.client;
            status = client.failed || client.queue.cancelled(flush.ticket) ? STATUS_CANCELED : rt_admit(client.queue.request(flush.ticket)->deadline);
            if (status != STATUS_OK) { client.finish(flush.ticket, status); flush = {}; }
            else if (!device.active()) {
                const auto index = slot();
                if (index != device.depth()) {
                    auto& operation = pending[index];
                    if (!client.queue.commit(flush.ticket)) status = STATUS_CANCELED;
                    else status = device.submit({index, ++operation.gen}, blk_op::Flush, 0, 0);
                    if (status == STATUS_OK) { operation.client = flush.client; operation.ticket = flush.ticket; flush.submitted = true; }
                    else { client.finish(flush.ticket, status); flush = {}; }
                }
            }
        }
        for (size_t row = 0; row < blk_dev::Depth && !flush.client; ++row) {
            for (auto* client = clients.get(); client && !flush.client; client = client->next.get()) {
                if (client->stopped) continue;
                const auto index = slot(); if (index == device.depth()) break;
                ubus_ticket ticket;
                if (client->queue.admit(ticket) != ubus_admission::Ready) continue;
                const auto& command = *client->queue.request(ticket);
                status = client->queue.cancelled(ticket) ? STATUS_CANCELED : rt_admit(command.deadline);
                if (status == STATUS_OK && command.kind == blk_op::Flush) { flush = {client, ticket, false}; again = true; break; }
                auto& operation = pending[index];
                if (status == STATUS_OK && !client->queue.commit(ticket)) status = STATUS_CANCELED;
                if (status == STATUS_OK && command.kind == blk_op::Write) {
                    auto* call = client->queue.call(ticket);
                    status = mem_read(call->capability(call->data["buffer"]), command.buffer.offset,
                        std::as_writable_bytes(std::span{scratch}.first(command.buffer.size)));
                }
                if (status == STATUS_OK) status = device.submit({index, ++operation.gen}, command.kind,
                    command.offset, command.buffer.size, command.kind == blk_op::Write ? scratch.data() : nullptr);
                if (status == STATUS_OK) { operation.client = client; operation.ticket = ticket; }
                else client->finish(ticket, status);
                again = true;
            }
        }
        device.publish();
        for (auto* link = &clients; *link;) {
            auto& client = **link;
            if (client.stopped && !client.queue.active()) {
                if (client.closing) (void)client.closing->respond();
                auto retired = std::move(*link); *link = std::move(retired->next); again = true;
            } else link = &client.next;
        }
        if (clients && clients->next) {
            auto first = std::move(clients); clients = std::move(first->next);
            auto* tail = &clients; while (*tail) tail = &(*tail)->next; *tail = std::move(first);
        }
        status = directory.arm(); if (status != STATUS_OK) return status;
        if (!again) {
            const auto due = flush.client && !flush.submitted ? flush.client->queue.request(flush.ticket)->deadline : 0;
            status = ev_wait(rt_events(), due).status;
            if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
        }
    }
}
