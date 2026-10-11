#pragma once
#include <string.h>

#include <sys/start.hpp>
#include <sys/queue.hpp>
#include <sys/clock.hpp>
#include <libk/parse.hpp>

namespace boot {
inline constexpr Import UartMem{"uart.memory", 0x55415254, ObjMem};
inline constexpr Import UartIrq{"uart.irq", 0x55415254, ObjIrq};
inline constexpr Import ConsoleOutput{"console.output", 0x434f4e53, ObjChannel, 1, 1};
inline constexpr Import ConsoleInput{"console.input", 0x434f4e53, ObjChannel};
inline constexpr Import Stdin{"stdin", 0x5354524d, ObjChannel};
inline constexpr Import Stdout{"stdout", 0x5354524d, ObjChannel};
inline constexpr Import Stderr{"stderr", 0x5354524d, ObjChannel};
inline constexpr Import Process{"process", 0x50524f43, ObjChannel, 4};
inline constexpr Import Files{"files", 0x46494c45, ObjChannel, 3};
inline constexpr Import Block{"block", 0x424c4f43, ObjChannel, 2, 1};
inline constexpr Import Store{"store", 0x53544f52, ObjChannel, 1, 1};
inline constexpr Import StoreAdmin{"store.admin", 0x53544f52, ObjChannel, 1, 1};
inline constexpr Import Vfs{"vfs", 0x56465320, ObjChannel};
inline constexpr Import VfsRead{"vfs.read", 0x56465320, ObjChannel};
inline constexpr Import ServiceControl{"service.control", 0x53564354, ObjChannel};
inline constexpr Import ServiceWake{"service.wake", 0x53564357, ObjNotification};
} // namespace boot

namespace proc {
// Spawn carries NUL-terminated argv bytes, with argv[0] selecting the package.
// id bits 0..2 select the transferred stdin/stdout/stderr caps in that order.
// end_stdout requests an ordered End after task retirement, including failure.
// Missing streams are closed. Foreground and pipelines are caller policy.
// Wait optionally carries an absolute clock deadline; timeout/CancelWait remove
// only the waiter. Stop terminates; the first exit result wins.
inline constexpr uint64_t end_stdout = 8;
enum class op : uint64_t { Spawn = 1, Wait, Stop, CancelWait };

}
namespace console {

// The UART queue alone can place a prompt after output from every writer.
enum class Operation : uint64_t { Bytes, Prompt };

} // namespace console

namespace block {

// One execution owns this queue. Requests retain independent payload slots
// until completion is taken; dependent operations wait for overlapping writes.
class client final : private libk::noncopyable_nonmovable {
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

    [[nodiscard]] auto submit(io::Request& request, void* data = nullptr) noexcept -> status_t {
        const auto op = static_cast<io::Operation>(request.operation);
        if (request.object || request.buffer || request.flags
            || (request.length && !data && op != io::Operation::Read)) return STATUS_BAD_ARGS;
        if (op == io::Operation::Flush) {
            if (request.offset || request.length) return STATUS_BAD_ARGS;
        } else if (op == io::Operation::Identify) {
            if (request.offset || request.length != 20) return STATUS_BAD_ARGS;
        } else if ((op != io::Operation::Read && op != io::Operation::Write)
            || !request.length || request.length > io::BufferSize
            || request.length % 512 || request.offset % 512
            || request.offset > capacity_ || request.length > capacity_ - request.offset)
            return STATUS_BAD_ARGS;
        return session_.submit(request, data, true);
    }
    [[nodiscard]] auto completion(io::Completion& result, uint64_t id = 0) noexcept -> status_t {
        return session_.completion(result, id);
    }
    [[nodiscard]] auto payload() const noexcept -> const uint8_t* { return session_.payload(); }
    [[nodiscard]] auto wait(io::Completion& result, uint64_t id = 0) noexcept -> status_t {
        const auto deadline = clock_.after_ms(5'000);
        return deadline ? session_.wait(result, id, *deadline) : STATUS_INTERNAL;
    }
    [[nodiscard]] auto cancel(uint64_t id) noexcept -> status_t {
        if (session_.requests().error()) return session_.requests().error();
        io::ControlMessage request{.operation = static_cast<uint64_t>(io::Control::Cancel), .value = id};
        return session_.exchange(request);
    }
    // Retire a specific dependency without consuming unrelated completions.
    [[nodiscard]] auto wait(uint64_t id) noexcept -> status_t {
        io::Completion result{};
        const auto status = wait(result, id);
        return status == STATUS_OK ? result.status : status;
    }
    // Read results belong to their reader; barriers retire only filesystem writes.
    [[nodiscard]] auto drain_writes() noexcept -> status_t {
        auto status = session_.requests().error();
        uint64_t after{};
        while (const auto* request = session_.requests().next(after)) {
            after = request->id;
            if (request->operation != static_cast<uint64_t>(io::Operation::Write)) continue;
            const auto result = wait(after);
            if (status == STATUS_OK) status = result;
        }
        return status;
    }
    [[nodiscard]] auto dependencies(uint64_t offset, size_t size) noexcept -> status_t {
        uint64_t after{};
        while (const auto* request = session_.requests().next(after)) {
            after = request->id;
            if (request->operation == static_cast<uint64_t>(io::Operation::Write)
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
            const auto count = size < io::BufferSize ? size : io::BufferSize;
            const auto status = command(io::Operation::Read, offset, data, count);
            if (status != STATUS_OK) return status;
            offset += count;
            data += count;
            size -= count;
        }
        return STATUS_OK;
    }
    [[nodiscard]] auto flush() noexcept -> status_t {
        const auto status = drain_writes();
        return status == STATUS_OK ? command(io::Operation::Flush, 0, nullptr, 0) : status;
    }
    [[nodiscard]] auto identify(uint8_t (&id)[20]) noexcept -> status_t {
        const auto status = drain_writes();
        return status == STATUS_OK ? command(io::Operation::Identify, 0, id, sizeof(id)) : status;
    }

private:
    [[nodiscard]] auto command(io::Operation op, uint64_t offset, void* data, size_t size) noexcept -> status_t {
        io::Request request{.operation = static_cast<uint64_t>(op), .offset = offset, .length = size};
        const auto status = submit(request, data);
        return status == STATUS_OK ? wait(request.id) : status;
    }
    io::ClientSession session_{};
    sys::Clock clock_{};
    uint64_t capacity_{};
};

} // namespace block

namespace fs {

// The session control Channel authenticates file handles. Open returns a
// session-local handle in value and the byte size as eight little-endian bytes.
// List returns newline-separated short names and a next cursor (zero at end).
enum class Control : uint64_t { List = 16, Open, Close, Map, OpenObject };
inline constexpr size_t HandleCount = 32;
// Directory sender badges identify policy, independent of the client-minted
// private session endpoint. Opening a session freezes these permission bounds.
inline constexpr uint64_t ReadDirectory = 1;
inline constexpr uint64_t ExecuteDirectory = 3;

inline auto file_size(const auto& reply) noexcept -> uint64_t {
    uint64_t value{};
    for (size_t i = 0; i < 8; ++i) value |= uint64_t{static_cast<uint8_t>(reply.data[i])} << (i * 8);
    return value;
}

struct File final { uint64_t handle{}, size{}; };
struct FileMemory final { sys::cap_owner memory{}; uint64_t size{}, identity{}; };

[[nodiscard]] inline auto list(io::ClientSession& session, io::ControlMessage& message) noexcept -> status_t {
    message.operation = static_cast<uint64_t>(Control::List);
    message.size = 0;
    return session.exchange(message);
}
[[nodiscard]] inline auto open(io::ClientSession& session, const char* path, size_t size, File& file) noexcept -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Open), .size = size};
    if (size > sizeof(message.data)) return STATUS_BAD_ARGS;
    ::memcpy(message.data, path, size);
    const auto status = session.exchange(message);
    if (status != STATUS_OK) return status;
    if (message.size != 8) return STATUS_PEER_FAULT;
    file = {message.value, file_size(message)};
    return STATUS_OK;
}
[[nodiscard]] inline auto open_object(io::ClientSession& session, const char* path, size_t size,
    sys::cap_owner& object, uint64_t& length, uint64_t& generation) noexcept -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::OpenObject), .size = size};
    if (size == 0 || size > sizeof(message.data)) return STATUS_BAD_ARGS;
    ::memcpy(message.data, path, size);
    io::ControlPacket packet;
    const auto status = session.exchange(message, packet);
    if (status != STATUS_OK) return status;
    if (packet.count != 1 || message.size != sizeof(uint64_t) || !message.value)
        return STATUS_PEER_FAULT;
    length = file_size(message);
    generation = message.value;
    object = std::move(packet.capabilities[0]);
    return STATUS_OK;
}
[[nodiscard]] inline auto close(io::ClientSession& session, File file) noexcept -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Close), .value = file.handle};
    return session.exchange(message);
}

[[nodiscard]] inline auto backing(io::ClientSession& session, File file, word_t perms = VM_READ) noexcept
    -> std::expected<FileMemory, status_t> {
    if (perms != VM_READ && perms != (VM_READ | VM_EXECUTE))
        return std::unexpected(STATUS_BAD_ARGS);
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Map),
        .value = file.handle, .size = 1};
    message.data[0] = perms;
    io::ControlPacket packet;
    const auto status = session.exchange(message, packet);
    if (status != STATUS_OK) return std::unexpected(status);
    if (packet.count != 1 || message.size != 8 || message.value == 0
        || file_size(message) != file.size) return std::unexpected(STATUS_PEER_FAULT);
    return FileMemory{std::move(packet.capabilities[0]), file.size, message.value};
}

// Bounded pipelining with ordered consumption, suitable for console output
// and private package snapshots. Every submitted operation is drained even
// when one backend read fails; return never leaves a borrowed payload live.
template<class Consumer>
[[nodiscard]] inline auto read(io::ClientSession& session, File file, Consumer&& consume) noexcept -> status_t {
    uint64_t offset{};
    while (offset < file.size) {
        std::array<io::Request, io::QueueDepth> batch{};
        size_t count{}, bytes{};
        status_t failure{};
        while (count < batch.size() && offset + bytes < file.size) {
            const auto available = file.size - offset - bytes;
            const size_t length = available < io::BufferSize ? available : io::BufferSize;
            auto& request = batch[count];
            request = {.operation = static_cast<uint64_t>(io::Operation::Read),
                .object = file.handle, .offset = offset + bytes, .length = length};
            failure = session.submit(request, nullptr, true);
            if (failure != STATUS_OK) break;
            ++count;
            bytes += length;
        }
        for (size_t i = 0; i < count; ++i) {
            io::Completion result{};
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

} // namespace fs

namespace store {

inline constexpr size_t VolumeIdSize = 16;

[[nodiscard]] inline auto parse_volume_id(const char* text, size_t size,
    uint8_t (&id)[VolumeIdSize]) noexcept -> bool {
    if (text == nullptr || size != VolumeIdSize * 2) return false;
    for (size_t i = 0; i < VolumeIdSize; ++i) {
        const auto byte = libk::parse<uint8_t>({text + 2 * i, 2}, 16);
        if (!byte) return false;
        id[i] = *byte;
    }
    return true;
}

enum class Control : uint64_t { List = 16, Format = 20, Remove, Mkdir, Rename, DeviceId, VolumeId, Open, Sync };
// List packs NUL-separated names; value is the next cursor, or zero at EOF.
enum OpenFlags : uint64_t { Read = 1, Write = 2, Create = 4, Truncate = 8, Exclusive = 16, Append = 32 };
inline constexpr uint64_t ReadDirectory = 1;
inline constexpr uint64_t WriteDirectory = 2;
inline constexpr uint64_t AdminDirectory = 3;

[[nodiscard]] inline auto path_data(io::ControlMessage& message,
    const char* path) noexcept -> bool {
    message.size = ::strlen(path);
    if (message.size == 0 || message.size > sizeof(message.data)) return false;
    ::memcpy(message.data, path, message.size);
    return true;
}
[[nodiscard]] inline auto path_operation(io::ClientSession& session, Control operation, const char* path) noexcept
    -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(operation)};
    if (!path_data(message, path)) return STATUS_BAD_ARGS;
    return session.exchange(message);
}

[[nodiscard]] inline auto volume_id(io::ClientSession& session, uint8_t (&id)[VolumeIdSize]) noexcept -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::VolumeId)};
    const auto status = session.exchange(message);
    if (status != STATUS_OK) return status;
    if (message.size != sizeof(id)) return STATUS_PEER_FAULT;
    ::memcpy(id, message.data, sizeof(id));
    return STATUS_OK;
}
[[nodiscard]] inline auto device_id(io::ClientSession& session, uint8_t (&id)[20]) noexcept -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::DeviceId)};
    const auto status = session.exchange(message);
    if (status != STATUS_OK) return status;
    if (message.size != sizeof(id)) return STATUS_PEER_FAULT;
    ::memcpy(id, message.data, sizeof(id));
    return STATUS_OK;
}
[[nodiscard]] inline auto list(io::ClientSession& session, io::ControlMessage& message,
    const char* path = nullptr) noexcept -> status_t {
    message.operation = static_cast<uint64_t>(Control::List);
    if (path != nullptr && path[0] != '\0') {
        if (!path_data(message, path)) return STATUS_BAD_ARGS;
    } else message.size = 0;
    return session.exchange(message);
}
[[nodiscard]] inline auto open(io::ClientSession& session, const char* path, uint64_t flags,
    sys::cap_owner& object, uint64_t& length, uint64_t& generation) noexcept -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Open),
        .value = flags};
    if (!path_data(message, path)) return STATUS_BAD_ARGS;
    io::ControlPacket packet;
    const auto status = session.exchange(message, packet);
    if (status != STATUS_OK) return status;
    if (packet.count != 1 || message.size != sizeof(length) || !message.value)
        return STATUS_PEER_FAULT;
    ::memcpy(&length, message.data, sizeof(length));
    generation = message.value;
    object = std::move(packet.capabilities[0]);
    return STATUS_OK;
}
[[nodiscard]] inline auto sync(io::ClientSession& session) noexcept -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Sync)};
    return session.exchange(message);
}
[[nodiscard]] inline auto remove(io::ClientSession& session, const char* path) noexcept -> status_t {
    return path_operation(session, Control::Remove, path);
}
[[nodiscard]] inline auto mkdir(io::ClientSession& session, const char* path) noexcept -> status_t {
    return path_operation(session, Control::Mkdir, path);
}
[[nodiscard]] inline auto rename(io::ClientSession& session, const char* from, const char* to) noexcept -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Rename)};
    const size_t left = ::strlen(from), right = ::strlen(to);
    if (left == 0 || right == 0 || left + right + 1 > sizeof(message.data))
        return STATUS_BAD_ARGS;
    ::memcpy(message.data, from, left);
    message.data[left] = '\0';
    ::memcpy(message.data + left + 1, to, right);
    message.size = left + right + 1;
    return session.exchange(message);
}

[[nodiscard]] inline auto format(io::ClientSession& session, const uint8_t* id = nullptr) noexcept -> status_t {
    io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Format)};
    if (id != nullptr) {
        message.size = VolumeIdSize;
        ::memcpy(message.data, id, message.size);
    }
    return session.exchange(message);
}

} // namespace store
