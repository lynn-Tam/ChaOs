#pragma once
#include <ubus/ubus.hpp>
#include <libk/parse.hpp>

// File semantics are optional conveniences over ordinary bus methods/values.
// They define no wire structures, private codec, routing, or RPC engine.
enum : uint64_t { fs_lookup = 1, fs_modify = 2, fs_execute = 4, fs_admin = 8 };
enum : uint64_t { fs_read = 1, fs_write = 2, fs_create = 4, fs_truncate = 8, fs_exclusive = 16, fs_append = 32 };
enum : status_t { fs_exists = -256, fs_not_dir, fs_is_dir, fs_not_empty, fs_name_long, fs_too_large, fs_no_space, fs_cross_device };
inline constexpr size_t fs_volume_id_size = 16;

inline auto fs_path(std::string_view path, std::string_view base = "/") -> std::string {
    std::string result = path.starts_with('/') ? std::string{path} : std::string{base} + "/" + std::string{path};
    size_t size{}, cursor{};
    while (cursor < result.size()) {
        const auto begin = cursor;
        while (cursor < result.size() && result[cursor] != '/') ++cursor;
        const std::string_view part{result.data() + begin, cursor - begin};
        if (part == "..") { while (size && result[size - 1] != '/') --size; if (size) --size; }
        else if (!part.empty() && part != ".") {
            if (size) result[size++] = '/';
            ::memmove(result.data() + size, part.data(), part.size()); size += part.size();
        }
        ++cursor;
    }
    result.resize(size); result.insert(0, "/"); return result;
}
inline auto fs_parse_volume_id(std::string_view text, std::span<uint8_t, fs_volume_id_size> id) -> bool {
    if (text.size() != id.size() * 2) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const auto value = libk::parse<uint8_t>(text.substr(i * 2, 2), 16);
        if (!value) return false;
        id[i] = *value;
    }
    return true;
}
struct fs_image {
    cap_owner memory{};
    uint64_t size{}, offset{};
    auto create(CreateAuth pool, word_t perms) -> status_t {
        if (size > UINT64_MAX - 4095) return STATUS_BAD_ARGS;
        const auto bytes = (std::max(size, uint64_t{1}) + 4095) & ~uint64_t{4095};
        const auto created = sys::memory_create(pool, bytes, perms);
        if (created.status != STATUS_OK) return created.status;
        memory = cap_owner{created.value};
        for (size_t page = 0; page < bytes / 4096; ++page)
            if (const auto status = sys::memory_populate(memory.selector(), page).status; status != STATUS_OK) return status;
        return STATUS_OK;
    }
    auto offer(ubus_msg& reply, cap_desc& descriptor, word_t perms, cap_t payer = 0) const -> status_t {
        const CapView view{.version=CAP_ATTENUATION_VERSION_CURRENT, .kind=ObjMem, .size=CAP_ATTENUATION_SIZE,
            .rights=RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_DELEGATE,
            .words={0, (std::max(size, uint64_t{1}) + 4095) / 4096, perms}};
        auto exported = descriptor.derive(memory.selector(), view, payer);
        if (!exported) return exported.error();
        const auto index = reply.offer(std::move(*exported), view.rights);
        if (!index) return STATUS_NO_MEMORY;
        reply.data.put("buffer", ubus_buffer{*index, offset, size}); return STATUS_OK;
    }
};
inline auto fs_map(ubus_conn& directory, std::string_view path, word_t perms = VM_READ,
                   uint64_t deadline = 0) -> std::expected<fs_image, status_t> {
    ubus_msg request, response; request.deadline = deadline;
    request.data.put("path", path).put("perms", perms);
    const auto status = directory.invoke("Map", std::move(request), response);
    if (status != STATUS_OK) return std::unexpected(status);
    const auto buffer = response.data["buffer"].as<ubus_buffer>();
    auto memory = response.take(response.data["buffer"]);
    if (!buffer || !memory) return std::unexpected(STATUS_PEER_FAULT);
    return fs_image{std::move(memory), buffer->size, buffer->offset};
}
inline auto fs_open(ubus_conn& directory, std::string_view path, uint64_t flags,
                    cap_owner& object, uint64_t deadline = 0) -> status_t {
    ubus_msg request, response; request.deadline = deadline;
    request.data.put("path", path).put("flags", flags);
    const auto status = directory.invoke("Open", std::move(request), response);
    if (status != STATUS_OK) return status;
    object = response.take(response.data["handle"]);
    return object ? STATUS_OK : STATUS_PEER_FAULT;
}
inline auto fs_list(ubus_conn& directory, std::string_view path, ubus_data& page,
                    ubus_value cursor = {}, uint64_t deadline = 0) -> status_t {
    ubus_msg request, response; request.deadline = deadline;
    request.data.put("path", path);
    if (cursor) request.data.put("cursor", cursor);
    const auto status = directory.invoke("List", std::move(request), response);
    if (status == STATUS_OK) page = std::move(response.data);
    return status;
}
inline auto fs_sync(ubus_conn& directory, uint64_t deadline = 0) -> status_t {
    ubus_msg request, response; request.deadline = deadline;
    return directory.invoke("Sync", std::move(request), response);
}
inline auto fs_path_call(ubus_conn& directory, std::string_view method, std::string_view path) -> status_t {
    ubus_msg request, response; request.data.put("path", path);
    return directory.invoke(method, std::move(request), response);
}
inline auto fs_rename(ubus_conn& directory, std::string_view from, std::string_view to) -> status_t {
    ubus_msg request, response; request.data.put("from", from).put("to", to);
    return directory.invoke("Rename", std::move(request), response);
}
inline auto fs_format(ubus_conn& directory, std::span<const uint8_t> id = {}) -> status_t {
    ubus_msg request, response;
    if (!id.empty()) request.data.put("id", std::as_bytes(id));
    return directory.invoke("Format", std::move(request), response);
}
inline auto fs_id(ubus_conn& directory, std::string_view method, std::span<uint8_t> output) -> status_t {
    ubus_msg response;
    const auto status = directory.invoke(method, {}, response);
    if (status != STATUS_OK) return status;
    const auto id = response.data["id"].as<std::span<const std::byte>>();
    if (!id || id->size() != output.size()) return STATUS_PEER_FAULT;
    ::memcpy(output.data(), id->data(), output.size()); return STATUS_OK;
}

inline constexpr rt_key fs_archive{"archive", ObjMem};
