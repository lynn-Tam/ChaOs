#pragma once
#include <protocols/msg.hpp>
#include <start.hpp>
#include <protocols/queue.hpp>
#include <libk/parse.hpp>

inline constexpr rt_req fs_service{"files", 0x46494c45, ObjChannel, 8};
inline constexpr rt_req st_service{"store", 0x53544f52, ObjChannel, 6};
inline constexpr rt_req st_admin{"store.admin", 0x53544f52, ObjChannel, 6};
inline constexpr rt_req vfs_service{"vfs", 0x56465320, ObjChannel, 6};
inline constexpr rt_req vfs_read{"vfs.read", 0x56465320, ObjChannel, 6};
inline constexpr rt_req fs_archive{"archive", 0, ObjMem};
inline constexpr rt_req vfs_exec{"vfs.exec", 0x56465320, ObjChannel, 6};
// Directory authority is independent of the requested file or Mem permissions.
enum : uint64_t { fl_read = 1, fl_write = 2, fl_exec = 4, fl_admin = 8 };
struct fl_memory {
    cap_owner memory{};
    uint64_t size{};
    // Prepopulate while failure can still be returned to the caller. Copying
    // into a demand mapping must not turn shared-budget admission into a fault.
    auto create(cap_t pool, word_t perms) -> status_t {
        const auto bytes = (size + 4095) & ~uint64_t{4095};
        const auto created = sys::memory_create(pool, bytes, perms);
        memory = cap_owner{created.value};
        if (created.status != STATUS_OK)
            return created.status;
        for (word_t page = 0; page < bytes / 4096; ++page)
            if (const auto status = sys::memory_populate(memory.selector(), page).status;
                status != STATUS_OK)
                return status;
        return STATUS_OK;
    }

    auto offer(const rt_env& env, cap_t descriptor, cap_t payer, word_t perms,
               msg_reply& reply) const -> status_t {
        const CapView view{.version = CAP_ATTENUATION_VERSION_CURRENT,
                           .kind = ObjMem,
                           .size = CAP_ATTENUATION_SIZE,
                           .rights = RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_DELEGATE,
                           .words = {0, (size + 4095) / 4096, perms}};
        std::array<uint8_t, CAP_ATTENUATION_SIZE> wire{};
        cap_encode(view, wire);
        ::memcpy(reinterpret_cast<void*>(env.ipc), wire.data(), wire.size());
        const auto written = sys::memory_write(descriptor, 0, 0, wire.size()).status;
        if (written != STATUS_OK)
            return written;
        const auto result =
            sys::cap_typed_delegate(memory.selector(), env.cspace, descriptor, 0, payer);
        cap_owner exported{result.value};
        if (result.status != STATUS_OK)
            return result.status;
        if (!reply.offer(std::move(exported), view.rights))
            return STATUS_INTERNAL;
        reply.message.size = sizeof(size);
        ::memcpy(reply.message.data, &size, sizeof(size));
        return STATUS_OK;
    }
};

enum : status_t {
    file_exists = -256,
    file_not_dir,
    file_is_dir,
    file_not_empty,
    file_name_long,
    file_too_large,
    file_no_space
};

inline constexpr size_t st_volume_id_size = 16;

[[nodiscard]] inline auto st_parse_volume_id(const char* text, size_t size,
                                             uint8_t (&id)[st_volume_id_size]) noexcept -> bool {
    if (text == nullptr || size != st_volume_id_size * 2)
        return false;
    for (size_t i = 0; i < st_volume_id_size; ++i) {
        const auto byte = libk::parse<uint8_t>({text + 2 * i, 2}, 16);
        if (!byte)
            return false;
        id[i] = *byte;
    }
    return true;
}

enum class st_control : uint64_t {
    List = 16,
    Format = 20,
    Remove,
    Mkdir,
    Rename,
    DeviceId,
    VolumeId,
    Open,
    Sync,
    Map
};
// List packs NUL-separated names; value is the next cursor, or zero at EOF.
enum st_open_flags : uint64_t {
    st_read = 1,
    st_write = 2,
    st_create = 4,
    st_truncate = 8,
    st_exclusive = 16,
    st_append = 32
};

[[nodiscard]] inline auto st_path_data(msg_frame& message, const char* path) noexcept -> bool {
    message.size = ::strlen(path);
    if (message.size == 0 || message.size > sizeof(message.data))
        return false;
    ::memcpy(message.data, path, message.size);
    return true;
}
template <class Session>
[[nodiscard]] inline auto st_path_operation(Session& session, st_control operation,
                                            const char* path, uint64_t deadline = 0) noexcept
    -> status_t {
    msg_frame message{.operation = static_cast<uint64_t>(operation), .deadline = deadline};
    if (!st_path_data(message, path))
        return STATUS_BAD_ARGS;
    return session.exchange(message);
}

template <class Session>
[[nodiscard]] inline auto st_volume_id(Session& session, uint8_t (&id)[st_volume_id_size]) noexcept
    -> status_t {
    msg_frame message{.operation = static_cast<uint64_t>(st_control::VolumeId)};
    const auto status = session.exchange(message);
    if (status != STATUS_OK)
        return status;
    if (message.size != sizeof(id))
        return STATUS_PEER_FAULT;
    ::memcpy(id, message.data, sizeof(id));
    return STATUS_OK;
}
template <class Session>
[[nodiscard]] inline auto st_device_id(Session& session, uint8_t (&id)[20]) noexcept -> status_t {
    msg_frame message{.operation = static_cast<uint64_t>(st_control::DeviceId)};
    const auto status = session.exchange(message);
    if (status != STATUS_OK)
        return status;
    if (message.size != sizeof(id))
        return STATUS_PEER_FAULT;
    ::memcpy(id, message.data, sizeof(id));
    return STATUS_OK;
}
template <class Session>
[[nodiscard]] inline auto st_list(Session& session, msg_frame& message,
                                  const char* path = nullptr) noexcept -> status_t {
    message.operation = static_cast<uint64_t>(st_control::List);
    if (path != nullptr && path[0] != '\0') {
        if (!st_path_data(message, path))
            return STATUS_BAD_ARGS;
    } else
        message.size = 0;
    return session.exchange(message);
}
// Open transfers one private channel; Stat queries its current size.
// Capability identity replaces session-local handle generations.
template <class Session>
[[nodiscard]] inline auto st_open(Session& session, const char* path, uint64_t flags,
                                  cap_owner& object, uint64_t deadline = 0) noexcept -> status_t {
    msg_frame message{
        .operation = static_cast<uint64_t>(st_control::Open), .value = flags, .deadline = deadline};
    if (!st_path_data(message, path))
        return STATUS_BAD_ARGS;
    msg_packet packet;
    const auto status = session.exchange(message, packet);
    if (status != STATUS_OK)
        return status;
    if (packet.count != 1 || message.size || message.value)
        return STATUS_PEER_FAULT;
    object = std::move(packet.capabilities[0]);
    return STATUS_OK;
}
template <class Session>
[[nodiscard]] inline auto st_sync(Session& session, uint64_t deadline = 0) noexcept -> status_t {
    msg_frame message{.operation = static_cast<uint64_t>(st_control::Sync), .deadline = deadline};
    return session.exchange(message);
}
template <class Session>
[[nodiscard]] inline auto st_remove(Session& session, const char* path) noexcept -> status_t {
    return st_path_operation(session, st_control::Remove, path);
}
template <class Session>
[[nodiscard]] inline auto st_mkdir(Session& session, const char* path) noexcept -> status_t {
    return st_path_operation(session, st_control::Mkdir, path);
}
template <class Session>
[[nodiscard]] inline auto st_rename(Session& session, const char* from, const char* to) noexcept
    -> status_t {
    msg_frame message{.operation = static_cast<uint64_t>(st_control::Rename)};
    const size_t left = ::strlen(from), right = ::strlen(to);
    if (left == 0 || right == 0 || left + right + 1 > sizeof(message.data))
        return STATUS_BAD_ARGS;
    ::memcpy(message.data, from, left);
    message.data[left] = '\0';
    ::memcpy(message.data + left + 1, to, right);
    message.size = left + right + 1;
    return session.exchange(message);
}

template <class Session>
[[nodiscard]] inline auto st_format(Session& session, const uint8_t* id = nullptr) noexcept
    -> status_t {
    msg_frame message{.operation = static_cast<uint64_t>(st_control::Format)};
    if (id != nullptr) {
        message.size = st_volume_id_size;
        ::memcpy(message.data, id, message.size);
    }
    return session.exchange(message);
}

inline auto fl_map(rpc_client& directory, const char* path, word_t perms = VM_READ,
                   uint64_t deadline = 0) noexcept -> std::expected<fl_memory, status_t> {
    msg_frame request{
        .operation = static_cast<uint64_t>(st_control::Map), .value = perms, .deadline = deadline};
    if (!st_path_data(request, path))
        return std::unexpected(STATUS_BAD_ARGS);
    msg_packet packet;
    const auto status = directory.exchange(request, packet);
    if (status != STATUS_OK)
        return std::unexpected(status);
    uint64_t size{};
    if (packet.count != 1 || request.size != sizeof(size))
        return std::unexpected(STATUS_PEER_FAULT);
    ::memcpy(&size, request.data, sizeof(size));
    return fl_memory{std::move(packet.capabilities[0]), size};
}
