#include <util.hpp>
#include <string.h>
#include <uapi/mem.h>
#include <expected>
#include <utility>
#include <ch.hpp>
#include <io.hpp>
#include <start.hpp>
#include <servers/fat32.hpp>
#include <proto.hpp>

// A mount owns one backing per immutable file, shared by all issued views.
// Only MemoryObject owns resident-page state; this record owns an in-flight
// disk read and its exact Pager claim, never a second page cache.
class Backing final {
    cap_owner pager_, memory_, staging_;
    size_t file_{};
    uint64_t size_{};
    PagerReq claim_{};
    uint8_t bytes_[io_buffer_size]{};
    io_read read_{};

    void discard() noexcept {
        cap_owner* objects[]{&memory_, &pager_, &staging_};
        for (auto* object : objects) {
            if (*object) {
                if (const auto rc = sys::object_destroy(object->selector()).status; rc != STATUS_OK)
                    sys::exit(rc);
                *object = {};
            }
        }
    }
    static void completed(io_read& read, status_t status) noexcept {
        auto& self = *static_cast<Backing*>(read.context);
        if (status == STATUS_OK) {
            status = sys::memory_populate(self.staging_.selector(), 0).status;
        }
        if (status == STATUS_OK) {
            ::memcpy(reinterpret_cast<void*>(rt_startup().ipc), self.bytes_, sizeof(self.bytes_));
            status = sys::memory_write(self.staging_.selector(), 0, 0, sizeof(self.bytes_)).status;
        }
        if (status == STATUS_OK)
            status = sys::pager_supply(self.pager_.selector(), self.memory_.selector(),
                                       self.staging_.selector(), 0, self.claim_.id)
                         .status;
        if (status != STATUS_OK)
            if (const auto rc =
                    sys::pager_fail(self.pager_.selector(), self.memory_.selector(), self.claim_.id)
                        .status;
                rc != STATUS_OK)
                sys::exit(rc);
        self.claim_ = {};
    }

  public:
    auto open(cap_t pool, cap_t events, size_t file, uint64_t size) noexcept -> status_t {
        if (memory_)
            return STATUS_OK;
        if (size == 0)
            return STATUS_BAD_ARGS;
        const auto pager = sys::pager_create(pool);
        if (pager.status != STATUS_OK)
            return pager.status;
        pager_ = cap_owner{pager.value};
        const uint64_t rounded = (size + 4095) & ~uint64_t{4095};
        const auto memory =
            sys::memory_create_pager(pool, rounded, VM_READ | VM_EXECUTE, pager_.selector());
        if (memory.status != STATUS_OK) {
            discard();
            return memory.status;
        }
        memory_ = cap_owner{memory.value};
        const auto staging = sys::memory_create(pool, 4096, VM_READ | VM_WRITE);
        if (staging.status != STATUS_OK) {
            discard();
            return staging.status;
        }
        staging_ = cap_owner{staging.value};
        auto status = sys::memory_seal(memory_.selector()).status;
        if (status == STATUS_OK)
            status = sys::pager_bind(pager_.selector(), events, rt_event_badge).status;
        if (status != STATUS_OK) {
            discard();
            return status;
        }
        file_ = file;
        size_ = size;
        return STATUS_OK;
    }

    auto export_view(cap_t cspace, cap_t descriptor, word_t access, ch_reply& reply) noexcept
        -> status_t {
        const word_t rights = RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_DELEGATE;
        const CapView view{.version = CAP_ATTENUATION_VERSION_CURRENT,
                           .kind = ObjMem,
                           .size = CAP_ATTENUATION_SIZE,
                           .rights = rights,
                           .words = {0, (size_ + 4095) / 4096, access}};
        auto& wire = *reinterpret_cast<uint8_t (*)[CAP_ATTENUATION_SIZE]>(rt_startup().ipc);
        cap_encode(view, wire);
        const auto written = sys::memory_write(descriptor, 0, 0, sizeof(wire));
        if (written.status != STATUS_OK)
            return written.status;
        const auto exported = sys::cap_typed_delegate(memory_.selector(), cspace, descriptor);
        if (exported.status != STATUS_OK)
            return exported.status;
        if (!reply.offer(cap_owner{exported.value}, rights))
            return STATUS_INTERNAL;
        reply.message.value =
            file_ + 1; // identity within this mount authority, never an access token
        reply.message.size = 8;
        for (size_t b = 0; b < 8; ++b)
            reply.message.data[b] = size_ >> (b * 8);
        return STATUS_OK;
    }

    auto poll() noexcept -> status_t {
        if (!memory_ || read_.active)
            return STATUS_OK;
        const auto result = sys::pager_claim(pager_.selector());
        if (result.status == STATUS_WOULD_BLOCK)
            return STATUS_OK;
        if (result.status != STATUS_OK)
            return result.status;
        ::memcpy(&claim_, reinterpret_cast<void*>(rt_startup().ipc), sizeof(claim_));
        if (!claim_.id || claim_.kind != PAGER_REQUEST_PAGE_IN ||
            claim_.payload.page_in.count != 1 || claim_.page_index >= (size_ + 4095) / 4096)
            return STATUS_PEER_FAULT;
        for (auto& byte : bytes_)
            byte = 0;
        const auto offset = claim_.page_index * uint64_t{4096};
        const auto remaining = size_ - offset;
        read_ = {.object = file_,
                 .offset = offset,
                 .output = bytes_,
                 .size = static_cast<size_t>(remaining < 4096 ? remaining : 4096),
                 .context = this,
                 .complete = completed,
                 .active = true};
        return STATUS_OK;
    }
    auto read() noexcept -> io_read& { return read_; }
};

constexpr size_t Clients = 8;
constexpr size_t Opens = 8;
static ch_port directory;
static io_client backend;
static fat_vol volume;
static vm_area metadata;
static cap_t events;
static cap_t files_pool, files_cspace, files_vspace;
static cap_owner descriptor;
Backing backings[fat_max_files];
static io_reader reader{backend, [](const io_read& read) noexcept {
                            return std::expected<io_extent, status_t>{volume.extent(
                                read.object, read.offset + read.done, read.size - read.done)};
                        }};

struct Handle final {
    uint64_t generation{};
    size_t file{};
    bool live{};
};
struct Open;
struct Pending final {
    io_ticket ticket{};
    io_server* session{};
    Open* open{};
    io_read read{};
    size_t length{};
    bool waiting{}, implicit{};
};
struct Open final {
    cap_owner channel;
    io_server session;
    Pending pending[io_queue_depth]{};
    size_t file{};
    uint64_t offset{}, generation{};
    bool live{}, busy{}, prepared{};

    [[nodiscard]] auto create(size_t index, size_t slot, ch_reply& reply) noexcept -> status_t {
        if (generation == UINT64_MAX)
            return STATUS_NO_MEMORY;
        const auto pair = sys::channel_create(files_pool, 1, CHANNEL_MAX_WORDS, 4, 2);
        if (pair.status != STATUS_OK)
            return pair.status;
        cap_owner client_root{pair.value}, server_root{pair.value2};
        constexpr auto common = RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE;
        const auto client = sys::channel_mint(client_root.selector(), files_cspace, 1,
                                              common | RIGHT_DESTROY | RIGHT_DUPLICATE);
        const auto server = sys::channel_mint(server_root.selector(), files_cspace, 1, common);
        if (client.status != STATUS_OK || server.status != STATUS_OK) {
            (void) sys::object_destroy(client_root.selector());
            return client.status != STATUS_OK ? client.status : server.status;
        }
        cap_owner endpoint{client.value};
        channel = cap_owner{server.value};
        auto status = STATUS_OK;
        if (!prepared) {
            status = session.prepare(files_pool, files_vspace, files_cspace,
                                     0x72000000 + slot * 0x100000);
            if (status == STATUS_OK)
                prepared = true;
        }
        if (status == STATUS_OK)
            status = session.bind(channel.selector(), events, generation + 1);
        if (status != STATUS_OK) {
            (void) sys::object_destroy(endpoint.selector());
            session.abort();
            if (session.done())
                session.reset();
            channel = {};
            return status;
        }
        if (!reply.offer(std::move(endpoint), common | RIGHT_DESTROY | RIGHT_DUPLICATE)) {
            (void) sys::object_destroy(endpoint.selector());
            session.abort();
            if (session.done())
                session.reset();
            channel = {};
            return STATUS_INTERNAL;
        }
        file = index;
        offset = 0;
        ++generation;
        live = true;
        reply.message.value = generation;
        reply.message.size = sizeof(uint64_t);
        const uint64_t size = volume.file(index).size;
        ::memcpy(reply.message.data, &size, sizeof(size));
        return STATUS_OK;
    }
};
Open opens[Opens];
struct Client final {
    cap_owner channel;
    word_t access{};
    io_server session;
    Handle handles[fs_handle_count]{};
    Pending pending[io_queue_depth]{};

    auto resolve(uint64_t value) const noexcept -> size_t {
        const auto& handle = handles[value % fs_handle_count];
        return handle.live && handle.generation == value / fs_handle_count ? handle.file
                                                                           : volume.count();
    }
    void control(const ch_msg& request, ch_reply& response) noexcept {
        auto& reply = response.message;
        const auto operation = static_cast<fs_control>(request.operation);
        if (operation == fs_control::List) {
            if (request.size != 0 || request.value > volume.count()) {
                reply.status = STATUS_BAD_ARGS;
                return;
            }
            size_t cursor = request.value;
            while (cursor < volume.count()) {
                const auto* name = volume.file(cursor).name;
                const auto length = ::strlen(name);
                if (length + 1 > sizeof(reply.data) - reply.size)
                    break;
                ::memcpy(reply.data + reply.size, name, length);
                reply.size += length;
                reply.data[reply.size++] = '\n';
                ++cursor;
            }
            reply.value = cursor == volume.count() ? 0 : cursor;
        } else if (operation == fs_control::Open || operation == fs_control::OpenObject) {
            const auto file = volume.find(request.data, request.size);
            if (file == volume.count()) {
                reply.status = STATUS_NOT_FOUND;
                return;
            }
            if (operation == fs_control::OpenObject) {
                size_t slot{};
                while (slot < Opens && opens[slot].live)
                    ++slot;
                reply.status =
                    slot == Opens ? STATUS_NO_MEMORY : opens[slot].create(file, slot, response);
                return;
            }
            for (size_t i = 0; i < fs_handle_count; ++i) {
                auto& handle = handles[i];
                if (handle.live || handle.generation == UINT64_MAX / fs_handle_count)
                    continue;
                handle = {handle.generation + 1, file, true};
                reply.value = handle.generation * fs_handle_count + i;
                reply.size = 8;
                const uint64_t bytes = volume.file(file).size;
                for (size_t b = 0; b < 8; ++b)
                    reply.data[b] = bytes >> (b * 8);
                return;
            }
            reply.status = STATUS_NO_MEMORY;
        } else if (operation == fs_control::Map) {
            const auto file = resolve(request.value);
            const auto requested = static_cast<uint8_t>(request.data[0]);
            if (request.size != 1 || (requested != VM_READ && requested != (VM_READ | VM_EXECUTE)))
                reply.status = STATUS_BAD_ARGS;
            else if ((requested & ~access) != 0)
                reply.status = STATUS_DENIED;
            else if (file == volume.count())
                reply.status = STATUS_NOT_FOUND;
            else {
                reply.status =
                    backings[file].open(files_pool, events, file, volume.file(file).size);
                if (reply.status == STATUS_OK)
                    reply.status = backings[file].export_view(files_cspace, descriptor.selector(),
                                                              requested, response);
            }
        } else if (operation == fs_control::Close) {
            if (request.size != 0)
                reply.status = STATUS_BAD_ARGS;
            else if (resolve(request.value) == volume.count())
                reply.status = STATUS_NOT_FOUND;
            else
                handles[request.value % fs_handle_count].live = false;
        } else
            reply.status = STATUS_INVALID_OP;
    }
    static void completed(io_read& read, status_t status) noexcept {
        auto& pending = *static_cast<Pending*>(read.context);
        auto& session = *pending.session;
        if (pending.open != nullptr && pending.implicit) {
            if (status == STATUS_OK)
                pending.open->offset += read.done;
            pending.open->busy = false;
        }
        if (session.failed() || !session.queue()->finish(pending.ticket, status, read.done)) {
            session.abort();
            if (!session.queue()->abandon(pending.ticket))
                sys::exit(STATUS_INTERNAL);
        }
        pending = {};
    }
    static auto cancelled(const io_read& read) noexcept -> bool {
        const auto& pending = *static_cast<const Pending*>(read.context);
        return pending.session->failed() || pending.session->queue()->cancelled(pending.ticket);
    }
};
Client clients[Clients];

static void schedule(Open& open) noexcept {
    if (open.busy || open.session.failed())
        return;
    Pending* first{};
    for (auto& pending : open.pending)
        if (pending.waiting && (first == nullptr || pending.ticket.id < first->ticket.id))
            first = &pending;
    if (first == nullptr)
        return;
    first->waiting = false;
    const auto available =
        open.offset >= volume.file(open.file).size ? 0 : volume.file(open.file).size - open.offset;
    first->read.offset = open.offset;
    first->read.size = first->length < available ? first->length : available;
    first->read.active = true;
    open.busy = true;
}

// Mount may wait before clients are served. Batch metadata reads through the
// same queue used later for asynchronous forwarding, with no alternate I/O path.
static auto read_metadata(uint64_t offset, size_t size, uint8_t* output) noexcept -> status_t {
    size_t done{};
    while (done < size) {
        uint64_t ids[io_queue_depth]{};
        size_t offsets[io_queue_depth]{}, lengths[io_queue_depth]{};
        size_t count{};
        while (count < io_queue_depth && done < size) {
            const auto length = size - done < io_buffer_size ? size - done : io_buffer_size;
            io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                           .offset = offset + done,
                           .buffer_offset = count * io_buffer_size,
                           .length = length};
            if (backend.queue().submit(request) != libk::ring_status::ready)
                return STATUS_INTERNAL;
            ids[count] = request.id;
            offsets[count] = done;
            lengths[count] = length;
            done += length;
            ++count;
        }
        if (const auto rc = backend.flush(); rc != STATUS_OK)
            sys::exit(rc);
        size_t remaining = count;
        while (remaining != 0) {
            io_done completion;
            for (;;) {
                const auto result = backend.queue().take(completion);
                if (result == libk::ring_status::empty)
                    break;
                if (result != libk::ring_status::ready)
                    return STATUS_PEER_FAULT;
                size_t slot{};
                while (slot < count && ids[slot] != completion.id)
                    ++slot;
                if (slot == count || completion.flags != 0)
                    return STATUS_PEER_FAULT;
                if (completion.status != STATUS_OK)
                    return completion.status;
                if (completion.bytes != lengths[slot])
                    return STATUS_PEER_FAULT;
                ::memcpy(output + offsets[slot], backend.payload() + slot * io_buffer_size,
                         lengths[slot]);
                ids[slot] = 0;
                --remaining;
            }
            if (const auto rc = backend.flush(); rc != STATUS_OK)
                sys::exit(rc);
            if (remaining != 0) {
                if (const auto rc = backend.arm(); rc != STATUS_OK)
                    sys::exit(rc);
                if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
                    sys::exit(rc);
            }
        }
    }
    return STATUS_OK;
}

int main() {
    const auto& info = rt_startup();
    events = info.events;
    const auto pool = info.pool;
    const auto vspace = info.vspace;
    const auto cspace = info.cspace;
    files_pool = pool;
    files_cspace = cspace;
    files_vspace = vspace;
    const auto scratch = sys::memory_create(pool, 4096, VM_READ | VM_WRITE);
    if (scratch.status != STATUS_OK) sys::exit(scratch.status);
    descriptor = cap_owner{scratch.value};
    uint64_t capacity{};
    if (const auto rc = backend.connect(info.selector(blk_service), pool, cspace, events, vspace,
                                        0x70000000, capacity);
        rc != STATUS_OK)
        sys::exit(rc);
    uint8_t boot[512]{};
    if (const auto rc = read_metadata(0, sizeof(boot), boot); rc != STATUS_OK)
        sys::exit(rc);
    fat_geometry geometry;
    if (!fat_geometry::parse(boot, capacity, geometry))
        sys::exit(STATUS_BACKING_FAILED);
    const size_t fat_size = geometry.fat_bytes();
    const size_t index_size = size_t{geometry.clusters} * sizeof(uint32_t);
    const size_t metadata_size =
        (fat_size + index_size + geometry.bitmap_bytes() + 4095) & ~size_t{4095};
    auto mapped = vm_area::create(pool, vspace, 0x74000000, metadata_size);
    if (!mapped)
        sys::exit(mapped.error());
    metadata = std::move(*mapped);
    auto* fat = reinterpret_cast<uint8_t*>(metadata.address);
    auto* index = reinterpret_cast<uint32_t*>(fat + fat_size);
    auto* bitmap = reinterpret_cast<uint8_t*>(index) + index_size;
    if (const auto rc = volume.mount(geometry, fat, index, bitmap, read_metadata); rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = directory.open(info.selector(fs_service), events); rc != STATUS_OK)
        sys::exit(rc);
    for (size_t i = 0; i < Clients; ++i)
        if (const auto rc =
                clients[i].session.prepare(pool, vspace, cspace, 0x71000000 + i * 0x100000);
            rc != STATUS_OK)
            sys::exit(rc);
    size_t turn{};
    for (;;) {
        bool again{};
        for (size_t count = 0; count < Clients; ++count) {
            ch_packet packet;
            const auto status = directory.receive(packet);
            if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY)
                break;
            if (status != STATUS_OK || packet.count != 2)
                continue;
            cap_owner endpoint = std::move(packet.capabilities[0]);
            if (packet.badge != fs_read_directory && packet.badge != fs_execute_directory) {
                const ch_msg reply{.operation = packet.message.operation,
                                   .id = packet.message.id,
                                   .status = STATUS_DENIED};
                (void) ch_port::send_to(endpoint.selector(), reply);
                (void) sys::channel_close(endpoint.selector());
                continue;
            }
            packet.capabilities[0] = std::move(packet.capabilities[1]);
            packet.count = 1;
            Client* available{};
            for (auto& client : clients)
                if (!client.channel) {
                    available = &client;
                    break;
                }
            if (available == nullptr) {
                const ch_msg reply{.operation = packet.message.operation,
                                   .id = packet.message.id,
                                   .status = STATUS_NO_MEMORY};
                (void) ch_port::send_to(endpoint.selector(), reply);
                (void) sys::channel_close(endpoint.selector());
                continue;
            }
            auto& client = *available;
            client.access = VM_READ | (packet.badge == fs_execute_directory ? VM_EXECUTE : 0);
            client.channel = std::move(endpoint);
            const auto bound = client.session.bind(client.channel.selector(), events);
            const auto accepted =
                bound == STATUS_OK
                    ? client.session.accept(packet,
                                            [&](const ch_msg& request, ch_reply& reply) {
                                                client.control(request, reply);
                                            })
                    : bound;
            if (accepted != STATUS_OK)
                client.session.abort();
        }
        for (auto& client : clients)
            if (client.channel) {
                const auto status =
                    client.session.poll([&](const ch_msg& request, ch_reply& reply) {
                        client.control(request, reply);
                    });
                if (status != STATUS_OK) {
                    client.session.abort();
                    again = true;
                }
            }
        for (auto& open : opens)
            if (open.live)
                if (open.session.poll([&](const ch_msg& request, ch_reply& reply) {
                        if (request.operation == static_cast<uint64_t>(io_control::Stat) &&
                            request.size == 0 && request.value == 0) {
                            reply.message.value = volume.file(open.file).size;
                            return;
                        }
                        reply.message.status =
                            request.operation == static_cast<uint64_t>(io_control::Sync) &&
                                    request.size == 0 && request.value == 0
                                ? STATUS_OK
                                : STATUS_INVALID_OP;
                    }) != STATUS_OK) {
                    open.session.abort();
                    again = true;
                }
        const auto polled = reader.poll();
        if (!polled)
            sys::exit(polled.error());
        again |= *polled;
        const auto submit = [&](io_read& read) {
            const auto result = reader.submit(read);
            if (!result)
                sys::exit(result.error());
            again |= *result;
        };
        for (size_t file = 0; file < volume.count(); ++file)
            if (const auto rc = backings[file].poll(); rc != STATUS_OK)
                sys::exit(rc);
        for (auto& client : clients) {
            auto* queue = client.session.queue();
            if (queue == nullptr || client.session.closing())
                continue;
            for (size_t count = 0; count < io_queue_depth; ++count) {
                io_ticket ticket;
                const auto admitted = queue->admit(ticket);
                if (admitted == io_admission::Empty || admitted == io_admission::Backpressure)
                    break;
                if (admitted != io_admission::Ready) {
                    client.session.abort();
                    again = true;
                    break;
                }
                const auto& request = *queue->request(ticket);
                const auto file = client.resolve(request.object);
                const auto valid = request.operation == static_cast<uint64_t>(io_operation::Read) &&
                                   request.buffer == 0 && request.flags == 0 &&
                                   request.length <= io_buffer_size &&
                                   request.buffer_offset <= io_payload_size &&
                                   request.length <= io_payload_size - request.buffer_offset;
                if (!valid || file == volume.count()) {
                    if (!queue->finish(ticket, valid ? STATUS_NOT_FOUND : STATUS_BAD_ARGS, 0)) {
                        client.session.abort();
                        if (!queue->abandon(ticket))
                            sys::exit(STATUS_INTERNAL);
                        again = true;
                        break;
                    }
                    continue;
                }
                const auto available = request.offset >= volume.file(file).size
                                           ? 0
                                           : volume.file(file).size - request.offset;
                const size_t length = request.length < available ? request.length : available;
                auto& pending = client.pending[ticket.slot];
                pending = {.ticket = ticket,
                           .session = &client.session,
                           .read = {.object = file,
                                    .offset = request.offset,
                                    .output = client.session.payload() + request.buffer_offset,
                                    .size = length,
                                    .context = &pending,
                                    .complete = Client::completed,
                                    .cancelled = Client::cancelled,
                                    .active = true}};
            }
        }
        for (auto& open : opens)
            if (open.live) {
                auto* queue = open.session.queue();
                if (open.session.failed()) {
                    for (auto& pending : open.pending)
                        if (pending.waiting) {
                            pending.waiting = false;
                            pending.read.active = true;
                        }
                }
                if (queue == nullptr)
                    continue;
                if (open.session.closing()) {
                    schedule(open);
                    continue;
                }
                for (size_t count = 0; count < io_queue_depth; ++count) {
                    io_ticket ticket;
                    const auto admitted = queue->admit(ticket);
                    if (admitted == io_admission::Empty || admitted == io_admission::Backpressure)
                        break;
                    if (admitted != io_admission::Ready) {
                        open.session.abort();
                        again = true;
                        break;
                    }
                    const auto& request = *queue->request(ticket);
                    const bool valid =
                        request.operation == static_cast<uint64_t>(io_operation::Read) &&
                        request.object == 0 && request.buffer == 0 && request.flags <= 1 &&
                        request.length != 0 && request.length <= io_buffer_size &&
                        request.buffer_offset <= io_payload_size &&
                        request.length <= io_payload_size - request.buffer_offset;
                    if (!valid) {
                        if (!queue->finish(ticket, STATUS_BAD_ARGS, 0)) {
                            open.session.abort();
                            if (!queue->abandon(ticket))
                                sys::exit(STATUS_INTERNAL);
                            again = true;
                            break;
                        }
                        continue;
                    }
                    auto& pending = open.pending[ticket.slot];
                    pending = {.ticket = ticket,
                               .session = &open.session,
                               .open = &open,
                               .read = {.object = open.file,
                                        .offset = request.offset,
                                        .output = open.session.payload() + request.buffer_offset,
                                        .context = &pending,
                                        .complete = Client::completed,
                                        .cancelled = Client::cancelled},
                               .length = static_cast<size_t>(request.length),
                               .waiting = request.flags == 1,
                               .implicit = request.flags == 1};
                    if (!pending.waiting) {
                        const auto available = request.offset >= volume.file(open.file).size
                                                   ? 0
                                                   : volume.file(open.file).size - request.offset;
                        pending.read.size = pending.length < available ? pending.length : available;
                        pending.read.active = true;
                    }
                }
                schedule(open);
            }
        // Rotate the start of each bounded batch. Pager and ordinary reads
        // share downstream credit, completion handling and immutable identity.
        for (size_t row = 0; row < fat_max_files; ++row) {
            if (row < io_queue_depth) {
                for (size_t n = 0; n < Clients; ++n) {
                    auto& client = clients[(turn + n) % Clients];
                    submit(client.pending[row].read);
                }
                for (size_t n = 0; n < Opens; ++n)
                    submit(opens[(turn + n) % Opens].pending[row].read);
            }
            submit(backings[(turn + row) % fat_max_files].read());
        }
        turn = (turn + 1) % fat_max_files;
        if (const auto rc = backend.flush(); rc != STATUS_OK)
            sys::exit(rc);
        for (auto& client : clients)
            if (client.channel) {
                const auto status = client.session.flush(true);
                if (status != STATUS_OK) {
                    client.session.abort();
                    again = true;
                }
                if (client.session.done()) {
                    (void) sys::channel_close(client.channel.selector());
                    client.session.reset();
                    client.channel = {};
                    for (auto& handle : client.handles)
                        handle.live = false;
                } else {
                    const auto armed = client.session.arm();
                    if (armed != STATUS_OK) {
                        client.session.abort();
                        again = true;
                    }
                }
            }
        for (auto& open : opens)
            if (open.live) {
                if (open.session.flush(true) != STATUS_OK) {
                    open.session.abort();
                    again = true;
                }
                if (open.session.done()) {
                    (void) sys::channel_close(open.channel.selector());
                    open.session.reset();
                    open.channel = {};
                    open.live = false;
                } else if (open.session.arm() != STATUS_OK) {
                    open.session.abort();
                    again = true;
                }
                if (!open.busy)
                    for (const auto& pending : open.pending)
                        if (pending.waiting)
                            again = true;
            }
        if (const auto rc = directory.arm().status; rc != STATUS_OK)
            sys::exit(rc);
        if (const auto rc = backend.arm(); rc != STATUS_OK)
            sys::exit(rc);
        if (!again)
            if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
                sys::exit(rc);
    }
}
