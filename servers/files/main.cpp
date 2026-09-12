#include <servers/files/fat32.hpp>
#include <user/lib/file_protocol.hpp>

namespace {
using namespace myos;
constexpr size_t Clients = 2;
io::ClientSession backend;
files::fat32::Volume volume;
MappedMemory metadata;
myos_cap_t events;

struct Handle final { uint64_t generation{}; size_t file{}; bool live{}; };
struct Pending final { io::Ticket ticket{}; size_t file{}, done{}, total{}; bool active{}, submitted{}; };
struct Client final {
    io::ServerSession session;
    Handle handles[files::HandleCount]{};
    Pending pending[io::QueueDepth]{};

    auto resolve(uint64_t value) const noexcept -> size_t {
        const auto& handle = handles[value % files::HandleCount];
        return handle.live && handle.generation == value / files::HandleCount
            ? handle.file : volume.count();
    }
    void control(const io::ControlMessage& request, io::ControlMessage& reply) noexcept {
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
        } else if (operation == files::Control::Open) {
            const auto file = volume.find(request.data, request.size);
            if (file == volume.count()) { reply.status = MYOS_STATUS_NOT_FOUND; return; }
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
        } else if (operation == files::Control::Close) {
            if (request.size != 0) reply.status = MYOS_STATUS_BAD_ARGS;
            else if (resolve(request.value) == volume.count()) reply.status = MYOS_STATUS_NOT_FOUND;
            else handles[request.value % files::HandleCount].live = false;
        } else reply.status = MYOS_STATUS_INVALID_OP;
    }
    void finish(Pending& pending, myos_status_t status) noexcept {
        if (!session.queue()->finish(pending.ticket, status, pending.done)) exit(MYOS_STATUS_PEER_FAULT);
        pending = {};
    }
};
Client clients[Clients];
struct Forward final {
    Pending* pending{};
    size_t client{};
    uint64_t id{};
    files::fat32::Extent extent{};
};
Forward forwards[io::QueueDepth];

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
    uint64_t capacity{};
    service::require(backend.open(service::capability(info, MYOS_BOOTSTRAP_CAP_BLOCK_CHANNEL),
        events, vspace, 0x70000000, capacity));
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
    constexpr uint32_t roles[Clients] = {MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, MYOS_BOOTSTRAP_CAP_FILE_CHANNEL};
    for (size_t i = 0; i < Clients; ++i)
        service::require(clients[i].session.open(service::capability(info, roles[i]), events,
            pool, vspace, cspace, 0x71000000 + i * 0x100000));
    size_t turn{};
    for (;;) {
        for (auto& client : clients)
            service::require(client.session.poll([&](const io::ControlMessage& request, io::ControlMessage& reply) {
                client.control(request, reply);
            }));
        for (;;) {
            io::Completion completion;
            const auto result = backend.queue().take(completion);
            if (result == libk::RingResult::Empty) break;
            if (result != libk::RingResult::Ready) exit(MYOS_STATUS_PEER_FAULT);
            size_t slot{};
            while (slot < io::QueueDepth && forwards[slot].id != completion.id) ++slot;
            if (slot == io::QueueDepth || forwards[slot].pending == nullptr) exit(MYOS_STATUS_PEER_FAULT);
            auto& forward = forwards[slot];
            auto& pending = *forward.pending;
            auto& client = clients[forward.client];
            const auto* request = client.session.queue()->request(pending.ticket);
            if (request == nullptr || completion.flags != 0) exit(MYOS_STATUS_PEER_FAULT);
            pending.submitted = false;
            if (client.session.queue()->cancelled(pending.ticket)) client.finish(pending, MYOS_STATUS_CANCELED);
            else if (completion.status != MYOS_STATUS_OK) client.finish(pending, completion.status);
            else {
                if (completion.bytes != forward.extent.size) exit(MYOS_STATUS_PEER_FAULT);
                service::copy(client.session.payload() + request->buffer_offset + pending.done,
                    backend.payload() + slot * io::BufferSize + forward.extent.skip, forward.extent.bytes);
                pending.done += forward.extent.bytes;
                if (pending.done == pending.total) client.finish(pending, MYOS_STATUS_OK);
            }
            forward = {};
        }
        for (auto& client : clients) {
            auto* queue = client.session.queue();
            if (queue == nullptr || client.session.closing()) continue;
            for (size_t count = 0; count < io::QueueDepth; ++count) {
                io::Ticket ticket;
                const auto admitted = queue->admit(ticket);
                if (admitted == io::Admission::Empty || admitted == io::Admission::Backpressure) break;
                if (admitted != io::Admission::Ready) exit(MYOS_STATUS_PEER_FAULT);
                const auto& request = *queue->request(ticket);
                const auto file = client.resolve(request.object);
                const auto valid = request.operation == static_cast<uint64_t>(io::Operation::Read)
                    && request.buffer == 0 && request.flags == 0 && request.length <= io::BufferSize
                    && request.buffer_offset <= io::PayloadSize && request.length <= io::PayloadSize - request.buffer_offset;
                if (!valid || file == volume.count()) {
                    if (!queue->finish(ticket, valid ? MYOS_STATUS_NOT_FOUND : MYOS_STATUS_BAD_ARGS, 0)) exit(MYOS_STATUS_PEER_FAULT);
                    continue;
                }
                const auto available = request.offset >= volume.file(file).size ? 0 : volume.file(file).size - request.offset;
                const size_t length = request.length < available ? request.length : available;
                auto& pending = client.pending[ticket.slot];
                pending = {ticket, file, 0, length, true, false};
                if (length == 0) client.finish(pending, MYOS_STATUS_OK);
            }
        }
        // Rotate client priority per batch; a fragmented file cannot monopolize
        // all downstream slots while another client's work waits for admission.
        for (size_t row = 0; row < io::QueueDepth; ++row) {
            for (size_t n = 0; n < Clients; ++n) {
                const size_t owner = (turn + n) % Clients;
                auto& client = clients[owner];
                auto& pending = client.pending[row];
                if (!pending.active || pending.submitted) continue;
                if (client.session.queue()->cancelled(pending.ticket)) {
                    client.finish(pending, MYOS_STATUS_CANCELED); continue;
                }
                size_t slot{};
                while (slot < io::QueueDepth && forwards[slot].pending != nullptr) ++slot;
                if (slot == io::QueueDepth) continue;
                const auto& request = *client.session.queue()->request(pending.ticket);
                const auto extent = volume.extent(pending.file, request.offset + pending.done, pending.total - pending.done);
                io::Request downstream{.operation = static_cast<uint64_t>(io::Operation::Read),
                    .offset = extent.offset, .buffer_offset = slot * io::BufferSize, .length = extent.size};
                const auto submitted = backend.queue().submit(downstream);
                if (submitted == libk::RingResult::Full) continue;
                if (submitted != libk::RingResult::Ready) exit(MYOS_STATUS_PEER_FAULT);
                pending.submitted = true;
                forwards[slot] = {&pending, owner, downstream.id, extent};
            }
        }
        turn = (turn + 1) % Clients;
        service::require(backend.flush());
        for (auto& client : clients) {
            service::require(client.session.flush(true));
            service::require(client.session.arm());
        }
        service::require(backend.arm());
        service::require(notification_wait(events).status);
    }
}
