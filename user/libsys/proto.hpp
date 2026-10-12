#pragma once
#include <string.h>

#include <start.hpp>
#include <io.hpp>
#include <util.hpp>
#include <libk/parse.hpp>


inline constexpr rt_req uart_mem{"uart.memory", 0x55415254, ObjMem};
inline constexpr rt_req uart_irq{"uart.irq", 0x55415254, ObjIrq};
inline constexpr rt_req con_output{"console.output", 0x434f4e53, ObjChannel, 2, 1};
inline constexpr rt_req con_input{"console.input", 0x434f4e53, ObjChannel, 2};
inline constexpr rt_req rt_stdin{"stdin", 0x5354524d, ObjChannel, 2};
inline constexpr rt_req rt_stdout{"stdout", 0x5354524d, ObjChannel, 2};
inline constexpr rt_req rt_stderr{"stderr", 0x5354524d, ObjChannel, 2};
inline constexpr rt_req fs_service{"files", 0x46494c45, ObjChannel, 4};
inline constexpr rt_req blk_service{"block", 0x424c4f43, ObjChannel, 3, 1};
inline constexpr rt_req st_service{"store", 0x53544f52, ObjChannel, 2, 1};
inline constexpr rt_req st_admin{"store.admin", 0x53544f52, ObjChannel, 2, 1};
inline constexpr rt_req vfs_service{"vfs", 0x56465320, ObjChannel, 2};
inline constexpr rt_req vfs_read{"vfs.read", 0x56465320, ObjChannel, 2};
inline constexpr rt_req sv_service{"service.control", 0x53564354, ObjChannel, 3};
inline constexpr rt_req sv_wake{"service.wake", 0x53564357, ObjNotification};

enum class sv_op : uint64_t { Restart, List };
inline constexpr uint64_t sv_running = 1, sv_recovering = 2;

// The UART queue alone can place a prompt after output from every writer.
enum class con_operation : uint64_t { Bytes, Prompt };

// One execution owns this queue. Requests retain independent payload slots
// until completion is taken; dependent operations wait for overlapping writes.
class blk_client final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto connect(cap_t directory, cap_t pool,
        cap_t cspace, cap_t events, cap_t vspace,
        uintptr_t address) noexcept -> status_t {
        auto status = clock_.open();
        if (status != STATUS_OK) return status;
        return session_.connect(directory, pool, cspace, events, vspace,
            address, capacity_, true);
    }
    [[nodiscard]] auto capacity() const noexcept -> uint64_t { return capacity_; }

    [[nodiscard]] auto submit(io_cmd& request, void* data = nullptr) noexcept -> status_t {
        const auto op = static_cast<io_operation>(request.operation);
        if (request.object || request.buffer || request.flags
            || (request.length && !data && op != io_operation::Read)) return STATUS_BAD_ARGS;
        if (op == io_operation::Flush) {
            if (request.offset || request.length) return STATUS_BAD_ARGS;
        } else if (op == io_operation::Identify) {
            if (request.offset || request.length != 20) return STATUS_BAD_ARGS;
        } else if ((op != io_operation::Read && op != io_operation::Write)
            || !request.length || request.length > io_buffer_size
            || request.length % 512 || request.offset % 512
            || request.offset > capacity_ || request.length > capacity_ - request.offset)
            return STATUS_BAD_ARGS;
        return session_.submit(request, data, true);
    }
    [[nodiscard]] auto completion(io_done& result, uint64_t id = 0) noexcept -> status_t {
        return session_.completion(result, id);
    }
    [[nodiscard]] auto payload() const noexcept -> const uint8_t* { return session_.payload(); }
    [[nodiscard]] auto wait(io_done& result, uint64_t id = 0) noexcept -> status_t {
        const auto deadline = clock_.after_ms(5'000);
        return deadline ? session_.wait(result, id, *deadline) : STATUS_INTERNAL;
    }
    [[nodiscard]] auto cancel(uint64_t id) noexcept -> status_t {
        if (session_.requests().error()) return session_.requests().error();
        ch_msg request{.operation = static_cast<uint64_t>(io_control::Cancel), .value = id};
        return session_.exchange(request);
    }
    // Retire a specific dependency without consuming unrelated completions.
    [[nodiscard]] auto wait(uint64_t id) noexcept -> status_t {
        io_done result{};
        const auto status = wait(result, id);
        return status == STATUS_OK ? result.status : status;
    }
    // Read results belong to their reader; barriers retire only filesystem writes.
    [[nodiscard]] auto drain_writes() noexcept -> status_t {
        auto status = session_.requests().error();
        uint64_t after{};
        while (const auto* request = session_.requests().next(after)) {
            after = request->id;
            if (request->operation != static_cast<uint64_t>(io_operation::Write)) continue;
            const auto result = wait(after);
            if (status == STATUS_OK) status = result;
        }
        return status;
    }
    [[nodiscard]] auto dependencies(uint64_t offset, size_t size) noexcept -> status_t {
        uint64_t after{};
        while (const auto* request = session_.requests().next(after)) {
            after = request->id;
            if (request->operation == static_cast<uint64_t>(io_operation::Write)
                && request->offset < offset + size && offset < request->offset + request->length) {
                const auto status = wait(after);
                if (status != STATUS_OK) return status;
            }
        }
        return session_.requests().error();
    }
    [[nodiscard]] auto read(uint64_t offset, uint8_t* data, size_t size) noexcept -> status_t {
        if ((!data && size) || size % 512 || offset % 512
            || offset > capacity_ || size > capacity_ - offset) return STATUS_BAD_ARGS;
        const auto dependency = dependencies(offset, size);
        if (dependency != STATUS_OK) return dependency;
        while (size) {
            const auto count = size < io_buffer_size ? size : io_buffer_size;
            const auto status = command(io_operation::Read, offset, data, count);
            if (status != STATUS_OK) return status;
            offset += count;
            data += count;
            size -= count;
        }
        return STATUS_OK;
    }
    [[nodiscard]] auto flush() noexcept -> status_t {
        const auto status = drain_writes();
        return status == STATUS_OK ? command(io_operation::Flush, 0, nullptr, 0) : status;
    }
    [[nodiscard]] auto identify(uint8_t (&id)[20]) noexcept -> status_t {
        const auto status = drain_writes();
        return status == STATUS_OK ? command(io_operation::Identify, 0, id, sizeof(id)) : status;
    }

private:
    [[nodiscard]] auto command(io_operation op, uint64_t offset, void* data, size_t size) noexcept -> status_t {
        io_cmd request{.operation = static_cast<uint64_t>(op), .offset = offset, .length = size};
        const auto status = submit(request, data);
        return status == STATUS_OK ? wait(request.id) : status;
    }
    io_client session_{};
    rt_clock clock_{};
    uint64_t capacity_{};
};

// The session control Channel authenticates file handles. Open returns a
// session-local handle in value and the byte size as eight little-endian bytes.
// List returns newline-separated short names and a next cursor (zero at end).
enum class fs_control : uint64_t { List = 16, Open, Close, Map, OpenObject };
inline constexpr size_t fs_handle_count = 32;
// Directory sender badges identify policy, independent of the client-minted
// private session endpoint. Opening a session freezes these permission bounds.
inline constexpr uint64_t fs_read_directory = 1;
inline constexpr uint64_t fs_execute_directory = 3;

inline auto fs_file_size(const auto& reply) noexcept -> uint64_t {
    uint64_t value{};
    for (size_t i = 0; i < 8; ++i) value |= uint64_t{static_cast<uint8_t>(reply.data[i])} << (i * 8);
    return value;
}

struct fs_file final { uint64_t handle{}, size{}; };
struct fs_file_memory final { cap_owner memory{}; uint64_t size{}, identity{}; };

[[nodiscard]] inline auto fs_list(io_client& session, ch_msg& message) noexcept -> status_t {
    message.operation = static_cast<uint64_t>(fs_control::List);
    message.size = 0;
    return session.exchange(message);
}
[[nodiscard]] inline auto fs_open(io_client& session, const char* path, size_t size, fs_file& file) noexcept -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(fs_control::Open), .size = size};
    if (size > sizeof(message.data)) return STATUS_BAD_ARGS;
    ::memcpy(message.data, path, size);
    const auto status = session.exchange(message);
    if (status != STATUS_OK) return status;
    if (message.size != 8) return STATUS_PEER_FAULT;
    file = {message.value, fs_file_size(message)};
    return STATUS_OK;
}
[[nodiscard]] inline auto fs_open_object(io_client& session, const char* path, size_t size,
    cap_owner& object, uint64_t& length, uint64_t& generation) noexcept -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(fs_control::OpenObject), .size = size};
    if (size == 0 || size > sizeof(message.data)) return STATUS_BAD_ARGS;
    ::memcpy(message.data, path, size);
    ch_packet packet;
    const auto status = session.exchange(message, packet);
    if (status != STATUS_OK) return status;
    if (packet.count != 1 || message.size != sizeof(uint64_t) || !message.value)
        return STATUS_PEER_FAULT;
    length = fs_file_size(message);
    generation = message.value;
    object = std::move(packet.capabilities[0]);
    return STATUS_OK;
}
[[nodiscard]] inline auto fs_close(io_client& session, fs_file file) noexcept -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(fs_control::Close), .value = file.handle};
    return session.exchange(message);
}

[[nodiscard]] inline auto fs_backing(io_client& session, fs_file file, word_t perms = VM_READ) noexcept
    -> std::expected<fs_file_memory, status_t> {
    if (perms != VM_READ && perms != (VM_READ | VM_EXECUTE))
        return std::unexpected(STATUS_BAD_ARGS);
    ch_msg message{.operation = static_cast<uint64_t>(fs_control::Map),
        .value = file.handle, .size = 1};
    message.data[0] = perms;
    ch_packet packet;
    const auto status = session.exchange(message, packet);
    if (status != STATUS_OK) return std::unexpected(status);
    if (packet.count != 1 || message.size != 8 || message.value == 0
        || fs_file_size(message) != file.size) return std::unexpected(STATUS_PEER_FAULT);
    return fs_file_memory{std::move(packet.capabilities[0]), file.size, message.value};
}

// Bounded pipelining with ordered consumption, suitable for console output
// and private package snapshots. Every submitted operation is drained even
// when one backend read fails; return never leaves a borrowed payload live.
template<class Consumer>
[[nodiscard]] inline auto fs_read(io_client& session, fs_file file, Consumer&& consume) noexcept -> status_t {
    uint64_t offset{};
    while (offset < file.size) {
        std::array<io_cmd, io_queue_depth> batch{};
        size_t count{}, bytes{};
        status_t failure{};
        while (count < batch.size() && offset + bytes < file.size) {
            const auto available = file.size - offset - bytes;
            const size_t length = available < io_buffer_size ? available : io_buffer_size;
            auto& request = batch[count];
            request = {.operation = static_cast<uint64_t>(io_operation::Read),
                .object = file.handle, .offset = offset + bytes, .length = length};
            failure = session.submit(request, nullptr, true);
            if (failure != STATUS_OK) break;
            ++count;
            bytes += length;
        }
        for (size_t i = 0; i < count; ++i) {
            io_done result{};
            const auto taken = session.wait(result, batch[i].id);
            const auto status = taken == STATUS_OK ? result.status : taken;
            if (failure == STATUS_OK) failure = status;
        }
        if (failure != STATUS_OK) return failure;
        for (size_t i = 0; i < count; ++i) {
            consume(offset, session.payload() + batch[i].buffer_offset, batch[i].length);
            offset += batch[i].length;
        }
    }
    return STATUS_OK;
}

inline constexpr size_t st_volume_id_size = 16;

[[nodiscard]] inline auto st_parse_volume_id(const char* text, size_t size,
    uint8_t (&id)[st_volume_id_size]) noexcept -> bool {
    if (text == nullptr || size != st_volume_id_size * 2) return false;
    for (size_t i = 0; i < st_volume_id_size; ++i) {
        const auto byte = libk::parse<uint8_t>({text + 2 * i, 2}, 16);
        if (!byte) return false;
        id[i] = *byte;
    }
    return true;
}

enum class st_control : uint64_t { List = 16, Format = 20, Remove, Mkdir, Rename, DeviceId, VolumeId, Open, Sync };
// List packs NUL-separated names; value is the next cursor, or zero at EOF.
enum st_open_flags : uint64_t { st_read = 1, st_write = 2, st_create = 4, st_truncate = 8, st_exclusive = 16, st_append = 32 };
inline constexpr uint64_t st_read_directory = 1;
inline constexpr uint64_t st_write_directory = 2;
inline constexpr uint64_t st_admin_directory = 3;

[[nodiscard]] inline auto st_path_data(ch_msg& message,
    const char* path) noexcept -> bool {
    message.size = ::strlen(path);
    if (message.size == 0 || message.size > sizeof(message.data)) return false;
    ::memcpy(message.data, path, message.size);
    return true;
}
template<class Session>
[[nodiscard]] inline auto st_path_operation(Session& session, st_control operation, const char* path) noexcept
    -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(operation)};
    if (!st_path_data(message, path)) return STATUS_BAD_ARGS;
    return session.exchange(message);
}

template<class Session>
[[nodiscard]] inline auto st_volume_id(Session& session, uint8_t (&id)[st_volume_id_size]) noexcept -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(st_control::VolumeId)};
    const auto status = session.exchange(message);
    if (status != STATUS_OK) return status;
    if (message.size != sizeof(id)) return STATUS_PEER_FAULT;
    ::memcpy(id, message.data, sizeof(id));
    return STATUS_OK;
}
template<class Session>
[[nodiscard]] inline auto st_device_id(Session& session, uint8_t (&id)[20]) noexcept -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(st_control::DeviceId)};
    const auto status = session.exchange(message);
    if (status != STATUS_OK) return status;
    if (message.size != sizeof(id)) return STATUS_PEER_FAULT;
    ::memcpy(id, message.data, sizeof(id));
    return STATUS_OK;
}
template<class Session>
[[nodiscard]] inline auto st_list(Session& session, ch_msg& message,
    const char* path = nullptr) noexcept -> status_t {
    message.operation = static_cast<uint64_t>(st_control::List);
    if (path != nullptr && path[0] != '\0') {
        if (!st_path_data(message, path)) return STATUS_BAD_ARGS;
    } else message.size = 0;
    return session.exchange(message);
}
template<class Session>
[[nodiscard]] inline auto st_open(Session& session, const char* path, uint64_t flags,
    cap_owner& object, uint64_t& length, uint64_t& generation) noexcept -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(st_control::Open),
        .value = flags};
    if (!st_path_data(message, path)) return STATUS_BAD_ARGS;
    ch_packet packet;
    const auto status = session.exchange(message, packet);
    if (status != STATUS_OK) return status;
    if (packet.count != 1 || message.size != sizeof(length) || !message.value)
        return STATUS_PEER_FAULT;
    ::memcpy(&length, message.data, sizeof(length));
    generation = message.value;
    object = std::move(packet.capabilities[0]);
    return STATUS_OK;
}
template<class Session>
[[nodiscard]] inline auto st_sync(Session& session) noexcept -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(st_control::Sync)};
    return session.exchange(message);
}
template<class Session>
[[nodiscard]] inline auto st_remove(Session& session, const char* path) noexcept -> status_t {
    return st_path_operation(session, st_control::Remove, path);
}
template<class Session>
[[nodiscard]] inline auto st_mkdir(Session& session, const char* path) noexcept -> status_t {
    return st_path_operation(session, st_control::Mkdir, path);
}
template<class Session>
[[nodiscard]] inline auto st_rename(Session& session, const char* from, const char* to) noexcept -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(st_control::Rename)};
    const size_t left = ::strlen(from), right = ::strlen(to);
    if (left == 0 || right == 0 || left + right + 1 > sizeof(message.data))
        return STATUS_BAD_ARGS;
    ::memcpy(message.data, from, left);
    message.data[left] = '\0';
    ::memcpy(message.data + left + 1, to, right);
    message.size = left + right + 1;
    return session.exchange(message);
}

template<class Session>
[[nodiscard]] inline auto st_format(Session& session, const uint8_t* id = nullptr) noexcept -> status_t {
    ch_msg message{.operation = static_cast<uint64_t>(st_control::Format)};
    if (id != nullptr) {
        message.size = st_volume_id_size;
        ::memcpy(message.data, id, message.size);
    }
    return session.exchange(message);
}
