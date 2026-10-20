#include <lib/fs/fs.hpp>
#include <lib/io/io.hpp>
#include <libk/cpio.hpp>
#include <vector>

struct bf_file { std::string name; std::span<const std::byte> bytes; fs_image backing; };
struct bf_open { io_server io; bf_file* file; uint64_t offset{}; };
static std::vector<bf_file> files;
static std::vector<std::unique_ptr<bf_open>> opens;
static CreateAuth image_pool;
static cap_desc descriptor;
static ubus_object directory{"myos.files"};

static auto map(ubus_request& request, ubus_msg& response) -> status_t {
    const auto perms = *request.data["perms"].as<word_t>();
    if (perms != VM_READ && perms != (VM_READ | VM_EXECUTE)) return STATUS_BAD_ARGS;
    if ((perms & VM_EXECUTE) && !(request.facet() & fs_execute)) return STATUS_DENIED;
    auto path = *request.data["path"].as<std::string_view>();
    while (path.starts_with('/')) path.remove_prefix(1);
    auto file = std::ranges::find(files, path, &bf_file::name);
    if (file == files.end()) return STATUS_NOT_FOUND;
    if (!file->backing.memory) {
        fs_image image{.size = file->bytes.size()};
        auto status = image.create(image_pool, VM_READ | VM_WRITE | VM_EXECUTE);
        if (status != STATUS_OK) return status;
        for (size_t offset = 0; offset < file->bytes.size();) {
            const auto count = std::min(file->bytes.size() - offset, size_t{4096});
            ::memcpy(reinterpret_cast<void*>(rt_startup().ipc), file->bytes.data() + offset, count);
            status = sys::memory_write(image.memory.selector(), offset, 0, count).status;
            if (status != STATUS_OK) return status;
            offset += count;
        }
        status = sys::memory_seal(image.memory.selector()).status;
        if (status != STATUS_OK) return status;
        file->backing = std::move(image);
    }
    return file->backing.offer(response, descriptor, perms, image_pool.account);
}
static auto open(ubus_request& request, ubus_msg& response) -> status_t {
    if (*request.data["flags"].as<uint64_t>() != fs_read) return STATUS_DENIED;
    auto path = *request.data["path"].as<std::string_view>();
    while (path.starts_with('/')) path.remove_prefix(1);
    auto file = std::ranges::find(files, path, &bf_file::name);
    if (file == files.end()) return STATUS_NOT_FOUND;
    auto opened = std::make_unique<bf_open>(); opened->file = &*file;
    auto status = opened->io.open(file->bytes.size(), io_read);
    if (status != STATUS_OK) return status;
    status = opened->io.object().add("Stat", {}, {{"size", ubus_type::U64}}, [size=file->bytes.size()](auto&, auto& result) {
        result.data.put("size", size); return STATUS_OK;
    });
    if (status != STATUS_OK) return status;
    status = opened->io.object().add("Sync", {}, {}, [](auto&, auto&) { return STATUS_OK; });
    if (status != STATUS_OK) return status;
    auto connector = opened->io.object().handle(0, RIGHT_SEND);
    if (!connector) return connector.error();
    response.data.cap("handle", *response.give(std::move(*connector)));
    opens.push_back(std::move(opened)); return STATUS_OK;
}
static auto list(ubus_request& request, ubus_msg& response) -> status_t {
    const auto path = *request.data["path"].as<std::string_view>();
    if (path != "/" && !path.empty()) return fs_not_dir;
    const auto state = request.data["cursor"];
    uint64_t cursor{};
    if (state && !state.data().empty()) {
        const auto epoch = state["epoch"].as<uint64_t>(), index = state["index"].as<uint64_t>();
        if (!epoch || !index) return STATUS_BAD_ARGS;
        if (*epoch != 1) return STATUS_STALE;
        cursor = *index;
    }
    if (cursor > files.size()) return STATUS_BAD_ARGS;
    ubus_data items{ubus_type::Array};
    const auto end = std::min(files.size(), static_cast<size_t>(cursor) + 64);
    for (size_t i = cursor; i < end; ++i) {
        ubus_data item; item.put("name", files[i].name).put("type", "file"); items.push(item);
    }
    response.data.put("epoch", uint64_t{1}).put("items", items);
    if (end != files.size()) { ubus_data next; next.put("epoch", uint64_t{1}).put("index", end); response.data.put("cursor", next); }
    return STATUS_OK;
}
int main() {
    const auto& env = rt_startup();
    image_pool = {env.selector({"image.pool", ObjAccount}), env.selector({"image.scope", ObjScope})};
    if (!image_pool.account || !image_pool.scope) return STATUS_DENIED;
    const auto* image = env.find(fs_archive);
    if (!image) return STATUS_INVALID_CAP;
    auto copied = sys::cap_duplicate(image->cap, env.cspace, RIGHT_MAP);
    if (copied.status != STATUS_OK) return copied.status;
    auto mapping = vm_area::map(rt_vm(), cap_owner{copied.value}, 0, (image->bytes + 4095) & ~size_t{4095}, VM_READ);
    if (!mapping) return mapping.error();
    const auto archive = libk::cpio_view::parse({reinterpret_cast<const std::byte*>(mapping->address), image->bytes});
    if (!archive) return STATUS_BAD_ARGS;
    size_t cursor{};
    while (const auto file = archive.next(cursor)) {
        if (file->name == "TRAILER!!!") break;
        if (file->name.starts_with("boot/")) files.push_back({std::string{file->name.substr(5)}, file->data, {}});
    }
    for (const auto status : {
        directory.add("Map", {{"path", ubus_type::String}, {"perms", ubus_type::U64}}, {{"buffer", ubus_type::Buffer}}, map, fs_lookup),
        directory.add("Open", {{"path", ubus_type::String}, {"flags", ubus_type::U64}}, {{"handle", ubus_type::Cap}}, open, fs_lookup),
        directory.add("List", {{"path", ubus_type::String}, {"cursor", ubus_type::Record, true}}, {{"epoch", ubus_type::U64}, {"cursor", ubus_type::Record, true}, {"items", ubus_type::Array}}, list, fs_lookup)})
        if (status != STATUS_OK) return status;
    auto status = directory.open();
    if (status != STATUS_OK) return status;
    ubus_conn publisher; status = publisher.bind(env.selector(ubus_publisher));
    if (status != STATUS_OK) return status;
    const auto publication = directory.publish(publisher, "files", fs_lookup | fs_execute);
    if (!publication) return publication.error();
    if (const auto ready = env.selector(ubus_ready)) {
        status = sys::notification_signal(ready).status;
        if (status != STATUS_OK) return status;
    }
    for (;;) {
        bool progress{};
        for (size_t n = 0; n < io_queue_depth; ++n) {
            status = directory.poll();
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) return status;
            progress = true;
        }
        for (auto opened = opens.begin(); opened != opens.end();) {
            auto& file = **opened;
            if (file.io.poll() != STATUS_OK) file.io.abort();
            io_ticket ticket;
            while (file.io.queue()->admit(ticket) == io_admission::Ready) {
                const auto request = *file.io.queue()->request(ticket);
                status = file.io.queue()->cancelled(ticket) ? STATUS_CANCELED : rt_admit(request.deadline);
                if (status == STATUS_OK && request.operation != io_operation::Read) status = STATUS_BAD_ARGS;
                const auto offset = request.sequential ? file.offset : request.offset;
                const auto count = status != STATUS_OK || offset >= file.file->bytes.size() ? size_t{0}
                    : std::min<uint64_t>(request.length, file.file->bytes.size() - offset);
                if (count) ::memcpy(file.io.payload() + request.buffer_offset, file.file->bytes.data() + offset, count);
                if (status == STATUS_OK && request.sequential) file.offset += count;
                if (!file.io.queue()->finish(ticket, status, count)) return STATUS_INTERNAL;
                progress = true;
            }
            status = file.io.flush(true);
            if (status != STATUS_OK) return status;
            if (file.io.done()) opened = opens.erase(opened); else ++opened;
        }
        if (!progress && (status = ev_wait(env.events).status) != STATUS_OK) return status;
    }
}
