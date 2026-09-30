#include <user/server_rt/service.hpp>
#include <user/server_rt/io.hpp>
#include <user/abi/startup.hpp>
#include <servers/files/backing.hpp>
#include <servers/files/fat32.hpp>
#include <user/ipc/storage.hpp>

namespace {
using namespace myos;
constexpr size_t Clients = 8;
constexpr size_t Opens = 8;
io::ControlPort directory;
io::ClientSession backend;
files::fat32::Volume volume;
MappedMemory metadata;
myos_cap_t events;
myos_cap_t files_pool, files_cspace, files_vspace;
cap::OwnedCap descriptor;
files::Backing backings[files::fat32::MaxFiles];
io::reader reader{backend, [](const io::read& read) noexcept {
    return std::expected<io::extent, myos_status_t>{
        volume.extent(read.object, read.offset + read.done, read.size - read.done)};
}};

struct Handle final { uint64_t generation{}; size_t file{}; bool live{}; };
struct Open;
struct Pending final {
    io::Ticket ticket{};
    io::ServerSession* session{};
    Open* open{};
    io::read read{};
    size_t length{};
    bool waiting{}, implicit{};
};
struct Open final {
    cap::OwnedCap channel;
    io::ServerSession session;
    Pending pending[io::QueueDepth]{};
    size_t file{};
    uint64_t offset{}, generation{};
    bool live{}, busy{}, prepared{};

    [[nodiscard]] auto create(size_t index, size_t slot,
        io::ControlReply& reply) noexcept -> myos_status_t {
        if (generation == UINT64_MAX) return MYOS_STATUS_NO_MEMORY;
        const auto pair = channel_create(files_pool, 1, MYOS_CHANNEL_MAX_WORDS, 4, 2);
        if (pair.status != MYOS_STATUS_OK) return pair.status;
        cap::OwnedCap client_root{{pair.value, 0}}, server_root{{pair.value2, 0}};
        constexpr auto common = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_CLOSE;
        const auto client = channel_mint(client_root.selector(), files_cspace, 1,
            common | MYOS_RIGHT_DESTROY | MYOS_RIGHT_DUPLICATE);
        const auto server = channel_mint(server_root.selector(), files_cspace, 1, common);
        if (client.status != MYOS_STATUS_OK || server.status != MYOS_STATUS_OK) {
            (void)object_destroy(client_root.selector());
            return client.status != MYOS_STATUS_OK ? client.status : server.status;
        }
        cap::OwnedCap endpoint{{client.value, 0}};
        channel = cap::OwnedCap{{server.value, 0}};
        auto status = MYOS_STATUS_OK;
        if (!prepared) {
            status = session.prepare(files_pool, files_vspace, files_cspace,
                0x72000000 + slot * 0x100000);
            if (status == MYOS_STATUS_OK) prepared = true;
        }
        if (status == MYOS_STATUS_OK) status = session.bind(channel.selector(), events, generation + 1);
        if (status != MYOS_STATUS_OK) {
            (void)object_destroy(endpoint.selector());
            channel = {};
            return status;
        }
        if (!reply.offer(libk::move(endpoint), common | MYOS_RIGHT_DESTROY
            | MYOS_RIGHT_DUPLICATE)) {
            (void)object_destroy(endpoint.selector());
            channel = {};
            return MYOS_STATUS_INTERNAL;
        }
        file = index;
        offset = 0;
        ++generation;
        live = true;
        reply.message.value = generation;
        reply.message.size = sizeof(uint64_t);
        const uint64_t size = volume.file(index).size;
        service::copy(reply.message.data, &size, sizeof(size));
        return MYOS_STATUS_OK;
    }
};
Open opens[Opens];
struct Client final {
    cap::OwnedCap channel;
    myos_word_t access{};
    io::ServerSession session;
    Handle handles[files::HandleCount]{};
    Pending pending[io::QueueDepth]{};

    auto resolve(uint64_t value) const noexcept -> size_t {
        const auto& handle = handles[value % files::HandleCount];
        return handle.live && handle.generation == value / files::HandleCount
            ? handle.file : volume.count();
    }
    void control(const io::ControlMessage& request, io::ControlReply& response) noexcept {
        auto& reply = response.message;
        const auto operation = static_cast<files::Control>(request.operation);
        if (operation == files::Control::List) {
            if (request.size != 0 || request.value > volume.count()) { reply.status = MYOS_STATUS_BAD_ARGS; return; }
            size_t cursor = request.value;
            while (cursor < volume.count()) {
                const auto* name = volume.file(cursor).name;
                const auto length = service::length(name);
                if (length + 1 > sizeof(reply.data) - reply.size) break;
                service::copy(reply.data + reply.size, name, length);
                reply.size += length;
                reply.data[reply.size++] = '\n';
                ++cursor;
            }
            reply.value = cursor == volume.count() ? 0 : cursor;
        } else if (operation == files::Control::Open
            || operation == files::Control::OpenObject) {
            const auto file = volume.find(request.data, request.size);
            if (file == volume.count()) { reply.status = MYOS_STATUS_NOT_FOUND; return; }
            if (operation == files::Control::OpenObject) {
                size_t slot{};
                while (slot < Opens && opens[slot].live) ++slot;
                reply.status = slot == Opens ? MYOS_STATUS_NO_MEMORY
                    : opens[slot].create(file, slot, response);
                return;
            }
            for (size_t i = 0; i < files::HandleCount; ++i) {
                auto& handle = handles[i];
                if (handle.live || handle.generation == UINT64_MAX / files::HandleCount) continue;
                handle = {handle.generation + 1, file, true};
                reply.value = handle.generation * files::HandleCount + i;
                reply.size = 8;
                const uint64_t bytes = volume.file(file).size;
                for (size_t b = 0; b < 8; ++b) reply.data[b] = bytes >> (b * 8);
                return;
            }
            reply.status = MYOS_STATUS_NO_MEMORY;
        } else if (operation == files::Control::Map) {
            const auto file = resolve(request.value);
            const auto requested = static_cast<uint8_t>(request.data[0]);
            if (request.size != 1 || (requested != MYOS_VM_READ
                && requested != (MYOS_VM_READ | MYOS_VM_EXECUTE))) reply.status = MYOS_STATUS_BAD_ARGS;
            else if ((requested & ~access) != 0) reply.status = MYOS_STATUS_DENIED;
            else if (file == volume.count()) reply.status = MYOS_STATUS_NOT_FOUND;
            else {
                reply.status = backings[file].open(files_pool, events, file, volume.file(file).size);
                if (reply.status == MYOS_STATUS_OK)
                    reply.status = backings[file].export_view(files_cspace, descriptor.selector(), requested, response);
            }
        } else if (operation == files::Control::Close) {
            if (request.size != 0) reply.status = MYOS_STATUS_BAD_ARGS;
            else if (resolve(request.value) == volume.count()) reply.status = MYOS_STATUS_NOT_FOUND;
            else handles[request.value % files::HandleCount].live = false;
        } else reply.status = MYOS_STATUS_INVALID_OP;
    }
    static void completed(io::read& read, myos_status_t status) noexcept {
        auto& pending = *static_cast<Pending*>(read.context);
        auto& session = *pending.session;
        if (pending.open != nullptr && pending.implicit) {
            if (status == MYOS_STATUS_OK) pending.open->offset += read.done;
            pending.open->busy = false;
        }
        if (session.failed() || !session.queue()->finish(pending.ticket, status, read.done)) {
            session.abort();
            if (!session.queue()->abandon(pending.ticket)) exit(MYOS_STATUS_INTERNAL);
        }
        pending = {};
    }
    static auto cancelled(const io::read& read) noexcept -> bool {
        const auto& pending = *static_cast<const Pending*>(read.context);
        return pending.session->failed() || pending.session->queue()->cancelled(pending.ticket);
    }
};
Client clients[Clients];

void schedule(Open& open) noexcept {
    if (open.busy || open.session.failed()) return;
    Pending* first{};
    for (auto& pending : open.pending)
        if (pending.waiting && (first == nullptr || pending.ticket.id < first->ticket.id))
            first = &pending;
    if (first == nullptr) return;
    first->waiting = false;
    const auto available = open.offset >= volume.file(open.file).size
        ? 0 : volume.file(open.file).size - open.offset;
    first->read.offset = open.offset;
    first->read.size = first->length < available ? first->length : available;
    first->read.active = true;
    open.busy = true;
}

// Mount may wait before clients are served. Batch metadata reads through the
// same queue used later for asynchronous forwarding, with no alternate I/O path.
auto read_metadata(uint64_t offset, size_t size, uint8_t* output) noexcept -> myos_status_t {
    size_t done{};
    while (done < size) {
        uint64_t ids[io::QueueDepth]{};
        size_t offsets[io::QueueDepth]{}, lengths[io::QueueDepth]{};
        size_t count{};
        while (count < io::QueueDepth && done < size) {
            const auto length = size - done < io::BufferSize ? size - done : io::BufferSize;
            io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
                .offset = offset + done, .buffer_offset = count * io::BufferSize, .length = length};
            if (backend.queue().submit(request) != libk::RingResult::Ready) return MYOS_STATUS_INTERNAL;
            ids[count] = request.id; offsets[count] = done; lengths[count] = length;
            done += length; ++count;
        }
        service::require(backend.flush());
        size_t remaining = count;
        while (remaining != 0) {
            io::Completion completion;
            for (;;) {
                const auto result = backend.queue().take(completion);
                if (result == libk::RingResult::Empty) break;
                if (result != libk::RingResult::Ready) return MYOS_STATUS_PEER_FAULT;
                size_t slot{};
                while (slot < count && ids[slot] != completion.id) ++slot;
                if (slot == count || completion.flags != 0)
                    return MYOS_STATUS_PEER_FAULT;
                if (completion.status != MYOS_STATUS_OK) return completion.status;
                if (completion.bytes != lengths[slot]) return MYOS_STATUS_PEER_FAULT;
                service::copy(output + offsets[slot], backend.payload() + slot * io::BufferSize, lengths[slot]);
                ids[slot] = 0; --remaining;
            }
            service::require(backend.flush());
            if (remaining != 0) {
                service::require(backend.arm());
                service::require(notification_wait(events).status);
            }
        }
    }
    return MYOS_STATUS_OK;
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = service::bootstrap(address, size);
    events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    const auto pool = service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const auto vspace = service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    const auto cspace = service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE);
    files_pool = pool;
    files_cspace = cspace;
    files_vspace = vspace;
    const auto scratch = memory_create(pool, 4096, MYOS_VM_READ | MYOS_VM_WRITE);
    service::require(scratch.status);
    descriptor = cap::OwnedCap{{scratch.value, 0}};
    uint64_t capacity{};
    service::require(backend.connect(service::capability(info, myos::bootstrap::imports::Block),
        pool, cspace, events, vspace, 0x70000000, capacity));
    uint8_t boot[512]{};
    service::require(read_metadata(0, sizeof(boot), boot));
    files::fat32::Geometry geometry;
    if (!files::fat32::Geometry::parse(boot, capacity, geometry)) exit(MYOS_STATUS_BACKING_FAILED);
    const size_t fat_size = geometry.fat_bytes();
    const size_t index_size = size_t{geometry.clusters} * sizeof(uint32_t);
    const size_t metadata_size = (fat_size + index_size + geometry.bitmap_bytes() + 4095) & ~size_t{4095};
    auto mapped = MappedMemory::create(pool, vspace, 0x74000000, metadata_size);
    if (!mapped) exit(mapped.error());
    metadata = libk::move(mapped).value();
    auto* fat = reinterpret_cast<uint8_t*>(metadata.address);
    auto* index = reinterpret_cast<uint32_t*>(fat + fat_size);
    auto* bitmap = reinterpret_cast<uint8_t*>(index) + index_size;
    service::require(volume.mount(geometry, fat, index, bitmap, read_metadata));
    service::require(directory.open(service::capability(info, bootstrap::imports::Files), events));
    for (size_t i = 0; i < Clients; ++i)
        service::require(clients[i].session.prepare(pool, vspace, cspace, 0x71000000 + i * 0x100000));
    size_t turn{};
    for (;;) {
        bool again{};
        for (size_t count = 0; count < Clients; ++count) {
            io::ControlPacket packet;
            const auto status = directory.receive(packet);
            if (status == MYOS_STATUS_WOULD_BLOCK || status == MYOS_STATUS_BUSY) break;
            if (status != MYOS_STATUS_OK || packet.count != 2) continue;
            cap::OwnedCap endpoint = libk::move(packet.capabilities[0]);
            if (packet.badge != files::ReadDirectory && packet.badge != files::ExecuteDirectory) {
                const io::ControlMessage reply{.operation = packet.message.operation, .id = packet.message.id,
                    .status = MYOS_STATUS_DENIED};
                (void)io::ControlPort::send_to(endpoint.selector(), reply);
                (void)channel_close(endpoint.selector());
                continue;
            }
            packet.capabilities[0] = libk::move(packet.capabilities[1]);
            packet.count = 1;
            Client* available{};
            for (auto& client : clients) if (!client.channel) { available = &client; break; }
            if (available == nullptr) {
                const io::ControlMessage reply{.operation = packet.message.operation, .id = packet.message.id,
                    .status = MYOS_STATUS_NO_MEMORY};
                (void)io::ControlPort::send_to(endpoint.selector(), reply);
                (void)channel_close(endpoint.selector());
                continue;
            }
            auto& client = *available;
            client.access = MYOS_VM_READ | (packet.badge == files::ExecuteDirectory ? MYOS_VM_EXECUTE : 0);
            client.channel = libk::move(endpoint);
            const auto bound = client.session.bind(client.channel.selector(), events);
            const auto accepted = bound == MYOS_STATUS_OK
                ? client.session.accept(packet, [&](const io::ControlMessage& request, io::ControlReply& reply) {
                    client.control(request, reply);
                }) : bound;
            if (accepted != MYOS_STATUS_OK) client.session.abort();
        }
        for (auto& client : clients) if (client.channel) {
            const auto status = client.session.poll([&](const io::ControlMessage& request, io::ControlReply& reply) {
                client.control(request, reply);
            });
            if (status != MYOS_STATUS_OK) { client.session.abort(); again = true; }
        }
        for (auto& open : opens) if (open.live)
            if (open.session.poll([&](const io::ControlMessage& request, io::ControlReply& reply) {
                if (request.operation == static_cast<uint64_t>(io::Control::Stat)
                    && request.size == 0 && request.value == 0) {
                    reply.message.value = volume.file(open.file).size;
                    return;
                }
                reply.message.status = request.operation == static_cast<uint64_t>(io::Control::Sync)
                    && request.size == 0 && request.value == 0
                    ? MYOS_STATUS_OK : MYOS_STATUS_INVALID_OP;
            }) != MYOS_STATUS_OK) { open.session.abort(); again = true; }
        const auto polled = reader.poll();
        if (!polled) exit(polled.error());
        again |= *polled;
        const auto submit = [&](io::read& read) {
            const auto result = reader.submit(read);
            if (!result) exit(result.error());
            again |= *result;
        };
        for (size_t file = 0; file < volume.count(); ++file) service::require(backings[file].poll());
        for (auto& client : clients) {
            auto* queue = client.session.queue();
            if (queue == nullptr || client.session.closing()) continue;
            for (size_t count = 0; count < io::QueueDepth; ++count) {
                io::Ticket ticket;
                const auto admitted = queue->admit(ticket);
                if (admitted == io::Admission::Empty || admitted == io::Admission::Backpressure) break;
                if (admitted != io::Admission::Ready) { client.session.abort(); again = true; break; }
                const auto& request = *queue->request(ticket);
                const auto file = client.resolve(request.object);
                const auto valid = request.operation == static_cast<uint64_t>(io::Operation::Read)
                    && request.buffer == 0 && request.flags == 0 && request.length <= io::BufferSize
                    && request.buffer_offset <= io::PayloadSize && request.length <= io::PayloadSize - request.buffer_offset;
                if (!valid || file == volume.count()) {
                    if (!queue->finish(ticket, valid ? MYOS_STATUS_NOT_FOUND : MYOS_STATUS_BAD_ARGS, 0)) {
                        client.session.abort();
                        if (!queue->abandon(ticket)) exit(MYOS_STATUS_INTERNAL);
                        again = true;
                        break;
                    }
                    continue;
                }
                const auto available = request.offset >= volume.file(file).size ? 0 : volume.file(file).size - request.offset;
                const size_t length = request.length < available ? request.length : available;
                auto& pending = client.pending[ticket.slot];
                pending = {.ticket = ticket, .session = &client.session,
                    .read = {.object = file, .offset = request.offset,
                        .output = client.session.payload() + request.buffer_offset,
                        .size = length, .context = &pending,
                        .complete = Client::completed, .cancelled = Client::cancelled, .active = true}};
            }
        }
        for (auto& open : opens) if (open.live) {
            auto* queue = open.session.queue();
            if (open.session.failed()) {
                for (auto& pending : open.pending) if (pending.waiting) {
                    pending.waiting = false;
                    pending.read.active = true;
                }
            }
            if (queue == nullptr) continue;
            if (open.session.closing()) { schedule(open); continue; }
            for (size_t count = 0; count < io::QueueDepth; ++count) {
                io::Ticket ticket;
                const auto admitted = queue->admit(ticket);
                if (admitted == io::Admission::Empty || admitted == io::Admission::Backpressure) break;
                if (admitted != io::Admission::Ready) { open.session.abort(); again = true; break; }
                const auto& request = *queue->request(ticket);
                const bool valid = request.operation == static_cast<uint64_t>(io::Operation::Read)
                    && request.object == 0 && request.buffer == 0 && request.flags <= 1
                    && request.length != 0 && request.length <= io::BufferSize
                    && request.buffer_offset <= io::PayloadSize
                    && request.length <= io::PayloadSize - request.buffer_offset;
                if (!valid) {
                    if (!queue->finish(ticket, MYOS_STATUS_BAD_ARGS, 0)) {
                        open.session.abort();
                        if (!queue->abandon(ticket)) exit(MYOS_STATUS_INTERNAL);
                        again = true; break;
                    }
                    continue;
                }
                auto& pending = open.pending[ticket.slot];
                pending = {.ticket = ticket, .session = &open.session, .open = &open,
                    .read = {.object = open.file, .offset = request.offset,
                        .output = open.session.payload() + request.buffer_offset,
                        .context = &pending, .complete = Client::completed,
                        .cancelled = Client::cancelled},
                    .length = static_cast<size_t>(request.length),
                    .waiting = request.flags == 1, .implicit = request.flags == 1};
                if (!pending.waiting) {
                    const auto available = request.offset >= volume.file(open.file).size
                        ? 0 : volume.file(open.file).size - request.offset;
                    pending.read.size = pending.length < available ? pending.length : available;
                    pending.read.active = true;
                }
            }
            schedule(open);
        }
        // Rotate the start of each bounded batch. Pager and ordinary reads
        // share downstream credit, completion handling and immutable identity.
        for (size_t row = 0; row < files::fat32::MaxFiles; ++row) {
            if (row < io::QueueDepth) {
                for (size_t n = 0; n < Clients; ++n) {
                    auto& client = clients[(turn + n) % Clients];
                    submit(client.pending[row].read);
                }
                for (size_t n = 0; n < Opens; ++n)
                    submit(opens[(turn + n) % Opens].pending[row].read);
            }
            submit(backings[(turn + row) % files::fat32::MaxFiles].read());
        }
        turn = (turn + 1) % files::fat32::MaxFiles;
        service::require(backend.flush());
        for (auto& client : clients) if (client.channel) {
            const auto status = client.session.flush(true);
            if (status != MYOS_STATUS_OK) { client.session.abort(); again = true; }
            if (client.session.done()) {
                (void)channel_close(client.channel.selector());
                client.channel = {};
                client.session.reset();
                for (auto& handle : client.handles) handle.live = false;
            } else {
                const auto armed = client.session.arm();
                if (armed != MYOS_STATUS_OK) { client.session.abort(); again = true; }
            }
        }
        for (auto& open : opens) if (open.live) {
            if (open.session.flush(true) != MYOS_STATUS_OK) { open.session.abort(); again = true; }
            if (open.session.done()) {
                (void)channel_close(open.channel.selector());
                open.channel = {};
                open.session.reset();
                open.live = false;
            } else if (open.session.arm() != MYOS_STATUS_OK) {
                open.session.abort(); again = true;
            }
            if (!open.busy)
                for (const auto& pending : open.pending) if (pending.waiting) again = true;
        }
        service::require(directory.arm());
        service::require(backend.arm());
        if (!again) service::require(notification_wait(events).status);
    }
}
