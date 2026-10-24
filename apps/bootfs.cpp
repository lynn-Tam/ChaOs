#include <ubus/ubus.hpp>
#include <vm.hpp>
#include <libk/cpio.hpp>

struct bf_file { std::string name; std::span<const std::byte> bytes; vm_area backing; };
struct bf_open { ubus_object object; bf_file* file; uint64_t offset{}, sequence{}; bool closed{}; };
static std::vector<bf_file> files;
static std::vector<vm_ptr<bf_open>> opens;
static CreateAuth images;
static cap_desc descriptor;
static ubus_object directory{"io.files"};

static auto find(std::string_view path) -> bf_file* {
    while (path.starts_with('/')) path.remove_prefix(1);
    const auto file = std::ranges::find(files, path, &bf_file::name);
    return file == files.end() ? nullptr : &*file;
}
static auto map(ubus_request& request, ubus_msg& response) -> status_t {
    const auto perms = *request.data["perms"].as<word_t>();
    if (perms != VM_READ && perms != (VM_READ | VM_EXECUTE)) return STATUS_BAD_ARGS;
    if ((perms & VM_EXECUTE) && !(request.facet() & 4)) return STATUS_DENIED;
    auto* file = find(*request.data["path"].as<std::string_view>()); if (!file) return STATUS_NOT_FOUND;
    const auto size = (std::max(file->bytes.size(), size_t{1}) + 4095) & ~size_t{4095};
    if (!file->backing.memory) {
        auto backing = vm_area::create(images, rt_vm(), 0, size, VM_READ | VM_WRITE | VM_EXECUTE, VM_READ | VM_WRITE);
        if (!backing) return backing.error();
        ::memcpy(reinterpret_cast<void*>(backing->address), file->bytes.data(), file->bytes.size());
        auto status = backing->detach();
        if (status == STATUS_OK) status = sys::memory_seal(backing->memory.selector()).status;
        if (status != STATUS_OK) return status;
        file->backing = std::move(*backing);
    }
    const CapView view{.version=CAP_ATTENUATION_VERSION_CURRENT, .kind=ObjMem, .size=CAP_ATTENUATION_SIZE,
        .rights=RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_DELEGATE, .words={0, size / 4096, perms}};
    auto exported = descriptor.derive(file->backing.memory.selector(), view, images.quota);
    if (!exported) return exported.error();
    const auto index = response.offer(std::move(*exported), view.rights); if (!index) return STATUS_NO_MEMORY;
    response.data.put("buffer", ubus_buffer{*index, 0, file->bytes.size()}); return STATUS_OK;
}
static auto open(ubus_request& request, ubus_msg& response) -> status_t {
    if (request.data["flags"].as<uint64_t>().value_or(1) != 1) return STATUS_DENIED;
    auto* file = find(*request.data["path"].as<std::string_view>()); if (!file) return STATUS_NOT_FOUND;
    if (opens.size() >= 64) return STATUS_EXHAUSTED;
    const auto budget = request.capability(request.data["budget"]);
    auto storage = vm_new<bf_open>({budget ? budget : rt_startup().quota, rt_startup().scope});
    if (!storage) return storage.error();
    auto opened = std::move(*storage); opened->file = file;
    auto& object = opened->object;
    auto status = object.add("Read", {{"id", ubus_type::U64}, {"offset", ubus_type::U64, true}, {"buffer", ubus_type::Buffer}},
        {{"bytes", ubus_type::U64}}, [state=opened.get()](auto& q, auto& r) {
            if (state->closed) return STATUS_CLOSED;
            const auto buffer = *q.data["buffer"].template as<ubus_buffer>();
            const auto id = *q.data["id"].template as<uint64_t>();
            if (buffer.size > 65536 || id <= state->sequence) return STATUS_BAD_ARGS;
            const auto explicit_offset = q.data["offset"].template as<uint64_t>();
            const auto offset = explicit_offset.value_or(state->offset);
            const auto count = offset >= state->file->bytes.size() ? size_t{0}
                : std::min<uint64_t>(buffer.size, state->file->bytes.size() - offset);
            const auto status = mem_write(q.capability(q.data["buffer"]), buffer.offset, state->file->bytes.subspan(std::min<uint64_t>(offset, state->file->bytes.size()), count));
            if (status != STATUS_OK) return status;
            state->sequence = id; if (!explicit_offset) state->offset += count;
            r.data.put("bytes", count); return STATUS_OK;
        });
    if (status == STATUS_OK) status = object.add("Stat", {}, {{"size", ubus_type::U64}, {"sequence", ubus_type::U64}}, [state=opened.get()](auto&, auto& r) {
        r.data.put("size", state->file->bytes.size()).put("sequence", state->sequence); return STATUS_OK;
    });
    if (status == STATUS_OK) status = object.add("Sync", {}, {}, [](auto&, auto&) { return STATUS_OK; });
    if (status == STATUS_OK) status = object.add("Close", {}, {}, [state=opened.get()](auto&, auto&) { state->closed = true; return STATUS_OK; });
    if (status == STATUS_OK) status = object.open(32, request.capability(request.data["budget"]));
    if (status != STATUS_OK) return status;
    auto handle = object.handle(0, RIGHT_SEND); if (!handle) return handle.error();
    const auto handle_idx = response.give(std::move(*handle));
    if (!handle_idx) return handle_idx.error();
    response.data.cap("handle", *handle_idx);
    opens.push_back(std::move(opened)); return STATUS_OK;
}
static auto list(ubus_request& request, ubus_msg& response) -> status_t {
    const auto path = *request.data["path"].as<std::string_view>();
    if (path != "/" && !path.empty()) return -255;
    const auto state = request.data["cursor"];
    const auto cursor = state["index"].as<uint64_t>().value_or(0);
    if (state && !state.data().empty() && state["epoch"].as<uint64_t>() != 1) return STATUS_STALE;
    if (cursor > files.size()) return STATUS_BAD_ARGS;
    ubus_data items{ubus_type::Array};
    const auto end = std::min<uint64_t>(files.size(), cursor + 64);
    for (size_t i = cursor; i < end; ++i) { ubus_data row; row.put("name", files[i].name).put("type", "file"); items.push(row); }
    response.data.put("epoch", uint64_t{1}).put("items", items);
    if (end != files.size()) { ubus_data next; next.put("epoch", uint64_t{1}).put("index", end); response.data.put("cursor", next); }
    return STATUS_OK;
}
int main() {
    const auto& env = rt_startup();
    ubus_conn parent;
    auto status = parent.bind(env.environment); if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = parent.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    const auto message = setup.data["message"];
    images = {setup.capability(message["image.quota"]), setup.capability(message["image.scope"])};
    const auto buffer = message["archive"].as<ubus_buffer>(); if (!buffer || buffer->offset) return STATUS_BAD_ARGS;
    auto mapping = vm_area::map(rt_vm(), setup.take(message["archive"]), 0, (buffer->size + 4095) & ~size_t{4095}, VM_READ);
    if (!mapping) return mapping.error();
    const auto archive = libk::cpio_view::parse({reinterpret_cast<const std::byte*>(mapping->address), size_t(buffer->size)});
    if (!archive) return STATUS_BAD_ARGS;
    size_t cursor{};
    while (const auto file = archive.next(cursor)) {
        if (file->name == "TRAILER!!!") break;
        if (file->name.starts_with("boot/")) files.push_back({std::string{file->name.substr(5)}, file->data, {}});
    }
    for (const auto status : {
        directory.add("Map", {{"path", ubus_type::String}, {"perms", ubus_type::U64}}, {{"buffer", ubus_type::Buffer}}, map, 1),
        directory.add("Document", {{"path", ubus_type::String}, {"writable", ubus_type::Bool, true}, {"budget", ubus_type::Cap, true}}, {{"handle", ubus_type::Cap}}, open, 1),
        directory.add("Open", {{"path", ubus_type::String}, {"flags", ubus_type::U64}, {"budget", ubus_type::Cap, true}}, {{"handle", ubus_type::Cap}}, open, 1),
        directory.add("List", {{"path", ubus_type::String}, {"cursor", ubus_type::Record, true}},
            {{"epoch", ubus_type::U64}, {"items", ubus_type::Array}, {"cursor", ubus_type::Record, true}}, list, 1)}) if (status != STATUS_OK) return status;
    status = directory.open(); if (status != STATUS_OK) return status;
    auto connector = directory.connector(1 | 4); if (!connector) return connector.error();
    ubus_msg publication, result;
    const auto peer_idx = publication.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
    if (!peer_idx) return peer_idx.error();
    publication.data.put("name", "files").put("interface", "io.files").put("version", uint64_t{1})
        .cap("connector", *peer_idx);
    status = parent.invoke("Publish", std::move(publication), result); if (status != STATUS_OK) return status;
    for (;;) {
        bool progress{};
        for (size_t n = 0; n < 32; ++n) {
            status = directory.poll(); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) return status;
            progress = true;
        }
        for (auto it = opens.begin(); it != opens.end();) {
            auto& file = **it;
            status = file.object.poll();
            if (status == STATUS_PEER_CLOSED || status == STATUS_CLOSED || file.closed) { it = opens.erase(it); progress = true; continue; }
            if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) return status;
            progress |= status == STATUS_OK; ++it;
        }
        if (!progress && (status = ev_wait(rt_events()).status) != STATUS_OK) return status;
    }
}
