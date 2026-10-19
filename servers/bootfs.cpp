#include <protocols/msg.hpp>
#include <protocols/file.hpp>
#include <libk/cpio.hpp>
#include <vector>
#include <memory>
#include <string>
#include <new>

struct bf_file {
    std::string name;
    std::span<const std::byte> bytes;
    fl_memory backing;
};
struct bf_open {
    std::unique_ptr<bf_open> next;
    cap_owner channel;
    io_server session;
    bf_file* file;
    uint64_t offset{};
};
static std::vector<bf_file> files;
static std::unique_ptr<bf_open> opens;
static const rt_env* env;
static cap_owner descriptor;
static CreateAuth memory_pool;

static auto backing(bf_file& f, word_t perms, msg_reply& reply) -> status_t {
    if (f.bytes.empty())
        return STATUS_BAD_ARGS;
    const auto size = (f.bytes.size() + 4095) & ~size_t{4095};
    if (!f.backing.memory) {
        fl_memory file{.size = f.bytes.size()};
        auto status = file.create(memory_pool, VM_READ | VM_WRITE | VM_EXECUTE);
        if (status != STATUS_OK)
            return status;
        vm_area prepared;
        status = prepared.open(env->vspace, 0, size, VM_READ | VM_WRITE);
        if (status == STATUS_OK)
            status = prepared.attach(file.memory.selector(), 0, size, VM_READ | VM_WRITE);
        if (status != STATUS_OK)
            return status;
        ::memcpy(reinterpret_cast<void*>(prepared.address), f.bytes.data(), f.bytes.size());
        status = prepared.detach();
        if (status == STATUS_OK)
            status = sys::memory_seal(file.memory.selector()).status;
        if (status != STATUS_OK)
            return status;
        f.backing = std::move(file);
    }
    return f.backing.offer(*env, descriptor.selector(), memory_pool.account, perms, reply);
}

static void control(const msg_packet& packet, msg_reply& response) {
    const auto& req = packet.message;
    auto& reply = response.message;
    if (const auto status = rt_admit(req.deadline); status != STATUS_OK) {
        reply.status = status;
        return;
    }
    if (!(packet.badge & fl_read) || (packet.badge & ~(fl_read | fl_exec))) {
        reply.status = STATUS_DENIED;
        return;
    }
    const auto op = static_cast<st_control>(req.operation);
    if (op == st_control::List) {
        std::string_view path{req.data, req.size};
        if (!path.empty() && path != "/") {
            reply.status = file_not_dir;
            return;
        }
        if (req.value > files.size()) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        size_t i = req.value;
        for (; i < files.size(); ++i) {
            const auto& name = files[i].name;
            const auto bytes = name.size() + (reply.size ? 1 : 0);
            if (bytes > sizeof(reply.data) - reply.size)
                break;
            if (reply.size)
                reply.data[reply.size++] = '\0';
            ::memcpy(reply.data + reply.size, name.data(), name.size());
            reply.size += name.size();
        }
        reply.value = i == files.size() ? 0 : i;
        return;
    }
    if (op != st_control::Open && op != st_control::Map) {
        reply.status = STATUS_INVALID_OP;
        return;
    }
    std::string_view path{req.data, req.size};
    while (path.starts_with('/'))
        path.remove_prefix(1);
    const auto found = std::ranges::find(files, path, &bf_file::name);
    if (found == files.end()) {
        reply.status = STATUS_NOT_FOUND;
        return;
    }
    if (op == st_control::Map) {
        if (req.value != VM_READ && req.value != (VM_READ | VM_EXECUTE)) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        if ((req.value & VM_EXECUTE) && !(packet.badge & fl_exec)) {
            reply.status = STATUS_DENIED;
            return;
        }
        reply.status = backing(*found, req.value, response);
        return;
    }
    if (req.value != st_read) {
        reply.status = STATUS_DENIED;
        return;
    }
    auto open = std::unique_ptr<bf_open>{new (std::nothrow) bf_open};
    if (!open) {
        reply.status = STATUS_NO_MEMORY;
        return;
    }
    open->file = &*found;
    const auto pair = sys::channel_create(env->alloc(), 1, CHANNEL_MAX_WORDS, 4);
    cap_owner roots[]{cap_owner{pair.value}, cap_owner{pair.value2}};
    cap_owner client;
    auto status = pair.status;
    constexpr auto rights =
        RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE | RIGHT_DESTROY | RIGHT_DUPLICATE;
    if (status == STATUS_OK) {
        const auto c = sys::channel_mint(roots[0].selector(), env->cspace, 1, rights);
        client = cap_owner{c.value};
        status = c.status;
    }
    if (status == STATUS_OK) {
        const auto c = sys::channel_mint(roots[1].selector(), env->cspace, 1,
                                         RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE);
        open->channel = cap_owner{c.value};
        status = c.status;
    }
    if (status == STATUS_OK)
        status = open->session.open(open->channel.selector(), env->events, memory_pool, env->alloc(), env->vspace,
                                    env->cspace, 0, 0);
    if (status != STATUS_OK) {
        reply.status = status;
        return;
    }
    if (!response.offer(std::move(client), RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE |
                                               RIGHT_DESTROY | RIGHT_DUPLICATE)) {
        reply.status = STATUS_INTERNAL;
        return;
    }
    open->next = std::move(opens);
    opens = std::move(open);
}
int main() {
    env = &rt_startup();
    memory_pool = {env->selector(io_memory), env->selector(io_scope)};
    if (!memory_pool.account || !memory_pool.scope)
        sys::exit(STATUS_DENIED);
    const auto* image = env->find(fs_archive);
    if (!image)
        sys::exit(STATUS_INVALID_CAP);
    vm_area archive;
    auto status = archive.open(env->vspace, 0, (image->bytes + 4095) & ~size_t{4095}, VM_READ);
    if (status == STATUS_OK)
        status = archive.attach(image->cap, 0, archive.size, VM_READ);
    if (status != STATUS_OK)
        sys::exit(status);
    const auto view =
        libk::cpio_view::parse({reinterpret_cast<const std::byte*>(archive.address), image->bytes});
    if (!view)
        sys::exit(STATUS_BAD_ARGS);
    size_t off{};
    while (auto f = view.next(off)) {
        if (f->name == "TRAILER!!!")
            break;
        // The exported boot directory is an explicit subtree of the archive.
        if (f->name.starts_with("boot/"))
            files.emplace_back(std::string{f->name.substr(5)}, f->data, fl_memory{});
    }
    const auto desc = sys::memory_create(env->alloc(), 4096, VM_READ | VM_WRITE);
    if (desc.status != STATUS_OK)
        sys::exit(desc.status);
    descriptor = cap_owner{desc.value};
    msg_rx directory{env->selector(fs_service), env->events};
    for (;;) {
        bool again{};
        for (size_t i = 0; i < io_queue_depth; ++i) {
            msg_packet packet;
            const auto rc = directory.receive(packet);
            if (rc == STATUS_WOULD_BLOCK)
                break;
            if (rc != STATUS_OK || packet.count != 1)
                continue;
            msg_reply response;
            response.message = {.operation = packet.message.operation, .sequence = packet.message.sequence};
            control(packet, response);
            response.respond(packet.capabilities[0].selector());
        }
        for (auto* link = &opens; *link;) {
            auto& o = **link;
            if (o.session.poll([&](const msg_frame& req, msg_reply& response) {
                    if (req.size || req.value)
                        response.message.status = STATUS_BAD_ARGS;
                    else if (req.operation == static_cast<uint64_t>(io_control::Stat))
                        response.message.value = o.file->bytes.size();
                    else if (req.operation != static_cast<uint64_t>(io_control::Sync))
                        response.message.status = STATUS_INVALID_OP;
                }) != STATUS_OK)
                o.session.abort();
            auto* q = o.session.queue();
            if (q && !o.session.closing())
                for (size_t n = 0; n < io_queue_depth; ++n) {
                    io_ticket ticket;
                    const auto admitted = q->admit(ticket);
                    if (admitted == io_admission::Empty || admitted == io_admission::Backpressure)
                        break;
                    if (admitted != io_admission::Ready) {
                        o.session.abort();
                        break;
                    }
                    if (q->cancelled(ticket)) {
                        if (!q->finish(ticket, STATUS_CANCELED, 0))
                            sys::exit(STATUS_INTERNAL);
                        again = true;
                        continue;
                    }
                    const auto& r = *q->request(ticket);
                    if (const auto status = rt_admit(r.deadline); status != STATUS_OK) {
                        if (!q->finish(ticket, status, 0))
                            sys::exit(STATUS_INTERNAL);
                        again = true;
                        continue;
                    }
                    const bool valid = r.operation == static_cast<uint64_t>(io_operation::Read) &&
                                       r.flags <= 1 && r.length <= io_buffer_size &&
                                       r.buffer_offset <= io_payload_size &&
                                       r.length <= io_payload_size - r.buffer_offset;
                    const auto offset = r.flags ? o.offset : r.offset;
                    const size_t count =
                        offset >= o.file->bytes.size()
                            ? 0
                            : std::min<uint64_t>(r.length, o.file->bytes.size() - offset);
                    if (valid && count)
                        ::memcpy(o.session.payload() + r.buffer_offset,
                                 o.file->bytes.data() + offset, count);
                    if (valid && r.flags)
                        o.offset += count;
                    if (!q->finish(ticket, valid ? STATUS_OK : STATUS_BAD_ARGS, valid ? count : 0))
                        sys::exit(STATUS_INTERNAL);
                    again = true;
                }
            if (o.session.flush(true) != STATUS_OK)
                o.session.abort();
            if (o.session.done()) {
                o.session.reset();
                auto retired = std::move(*link);
                *link = std::move(retired->next);
            } else {
                if (o.session.arm() != STATUS_OK) {
                    o.session.abort();
                    again = true;
                }
                link = &o.next;
            }
        }
        if (const auto rc = directory.arm().status; rc != STATUS_OK)
            sys::exit(rc);
        if (!again)
            if (const auto rc = ev_wait(env->events).status; rc != STATUS_OK)
                sys::exit(rc);
    }
}
