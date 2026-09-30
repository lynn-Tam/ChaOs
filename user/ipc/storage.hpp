#pragma once

#include <user/ipc/io.hpp>
#include <libk/parse.hpp>
#include <stdint.h>
#include <user/abi/time.hpp>
#include <user/abi/startup.hpp>

namespace myos::files {

// The session control Channel authenticates file handles. Open returns a
// session-local handle in value and the byte size as eight little-endian bytes.
// List returns newline-separated short names and a next cursor (zero at end).
enum class Control : uint64_t { List = 16, Open, Close, Map, OpenObject };
inline constexpr size_t HandleCount = 32;
// Directory sender badges identify policy, independent of the client-minted
// private session endpoint. Opening a session freezes this authority ceiling.
inline constexpr uint64_t ReadDirectory = 1;
inline constexpr uint64_t ExecuteDirectory = 3;

inline auto file_size(const io::ControlMessage& reply) noexcept -> uint64_t {
    uint64_t value{};
    for (size_t i = 0; i < 8; ++i) value |= uint64_t{static_cast<uint8_t>(reply.data[i])} << (i * 8);
    return value;
}

} // namespace myos::files

namespace myos::store {

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

} // namespace myos::store

namespace myos::vfs {
inline constexpr uint64_t ReadDirectory = 1;
inline constexpr uint64_t WriteDirectory = 2;
} // namespace myos::vfs

namespace myos::block {

// One execution owns this queue. Requests retain independent payload slots
// until completion is taken; dependent operations wait for overlapping writes.
class client final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto connect(myos_cap_t directory, myos_cap_t pool,
        myos_cap_t cspace, myos_cap_t events, myos_cap_t vspace,
        uintptr_t address) noexcept -> myos_status_t {
        auto status = clock_.open();
        if (status != MYOS_STATUS_OK) return status;
        return session_.connect(directory, pool, cspace, events, vspace,
            address, capacity_, true);
    }
    [[nodiscard]] auto capacity() const noexcept -> uint64_t { return capacity_; }

    [[nodiscard]] auto submit(io::Request& request, void* data = nullptr) noexcept -> myos_status_t {
        const auto op = static_cast<io::Operation>(request.operation);
        if (request.object || request.buffer || request.flags
            || (request.length && !data && op != io::Operation::Read)) return MYOS_STATUS_BAD_ARGS;
        if (op == io::Operation::Flush) {
            if (request.offset || request.length) return MYOS_STATUS_BAD_ARGS;
        } else if (op == io::Operation::Identify) {
            if (request.offset || request.length != 20) return MYOS_STATUS_BAD_ARGS;
        } else if ((op != io::Operation::Read && op != io::Operation::Write)
            || !request.length || request.length > io::BufferSize
            || request.length % 512 || request.offset % 512
            || request.offset > capacity_ || request.length > capacity_ - request.offset)
            return MYOS_STATUS_BAD_ARGS;
        return session_.submit(request, data, true);
    }
    [[nodiscard]] auto completion(io::Completion& result, uint64_t id = 0) noexcept -> myos_status_t {
        return session_.completion(result, id);
    }
    [[nodiscard]] auto payload() const noexcept -> const uint8_t* { return session_.payload(); }
    [[nodiscard]] auto wait(io::Completion& result, uint64_t id = 0) noexcept -> myos_status_t {
        const auto deadline = clock_.after_ms(5'000);
        return deadline ? session_.wait(result, id, *deadline) : MYOS_STATUS_INTERNAL;
    }
    [[nodiscard]] auto cancel(uint64_t id) noexcept -> myos_status_t {
        if (session_.requests().error()) return session_.requests().error();
        io::ControlMessage request{.operation = static_cast<uint64_t>(io::Control::Cancel), .value = id};
        return session_.exchange(request);
    }
    // Retire a specific dependency without consuming unrelated completions.
    [[nodiscard]] auto wait(uint64_t id) noexcept -> myos_status_t {
        io::Completion result{};
        const auto status = wait(result, id);
        return status == MYOS_STATUS_OK ? result.status : status;
    }
    // Read results belong to their reader; barriers retire only filesystem writes.
    [[nodiscard]] auto drain_writes() noexcept -> myos_status_t {
        auto status = session_.requests().error();
        uint64_t after{};
        while (const auto* request = session_.requests().next(after)) {
            after = request->id;
            if (request->operation != static_cast<uint64_t>(io::Operation::Write)) continue;
            const auto result = wait(after);
            if (status == MYOS_STATUS_OK) status = result;
        }
        return status;
    }
    [[nodiscard]] auto dependencies(uint64_t offset, size_t size) noexcept -> myos_status_t {
        uint64_t after{};
        while (const auto* request = session_.requests().next(after)) {
            after = request->id;
            if (request->operation == static_cast<uint64_t>(io::Operation::Write)
                && request->offset < offset + size && offset < request->offset + request->length) {
                const auto status = wait(after);
                if (status != MYOS_STATUS_OK) return status;
            }
        }
        return session_.requests().error();
    }
    [[nodiscard]] auto read(uint64_t offset, uint8_t* data, size_t size) noexcept -> myos_status_t {
        if ((!data && size) || size % 512 || offset % 512
            || offset > capacity_ || size > capacity_ - offset) return MYOS_STATUS_BAD_ARGS;
        const auto dependency = dependencies(offset, size);
        if (dependency != MYOS_STATUS_OK) return dependency;
        while (size) {
            const auto count = size < io::BufferSize ? size : io::BufferSize;
            const auto status = command(io::Operation::Read, offset, data, count);
            if (status != MYOS_STATUS_OK) return status;
            offset += count;
            data += count;
            size -= count;
        }
        return MYOS_STATUS_OK;
    }
    [[nodiscard]] auto flush() noexcept -> myos_status_t {
        const auto status = drain_writes();
        return status == MYOS_STATUS_OK ? command(io::Operation::Flush, 0, nullptr, 0) : status;
    }
    [[nodiscard]] auto identify(uint8_t (&id)[20]) noexcept -> myos_status_t {
        const auto status = drain_writes();
        return status == MYOS_STATUS_OK ? command(io::Operation::Identify, 0, id, sizeof(id)) : status;
    }

private:
    [[nodiscard]] auto command(io::Operation op, uint64_t offset, void* data, size_t size) noexcept -> myos_status_t {
        io::Request request{.operation = static_cast<uint64_t>(op), .offset = offset, .length = size};
        const auto status = submit(request, data);
        return status == MYOS_STATUS_OK ? wait(request.id) : status;
    }
    io::ClientSession session_{};
    Clock clock_{};
    uint64_t capacity_{};
};

} // namespace myos::block

namespace myos::files {

struct File final { uint64_t handle{}, size{}; };
struct FileMemory final { cap::OwnedCap memory{}; uint64_t size{}, identity{}; };

class Client final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto connect(const bootstrap::BootstrapView& info, uintptr_t address = 0x70000000, myos_cap_t events = 0) noexcept
        -> myos_status_t {
        events_ = events != 0 ? events : service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
        uint64_t value{};
        return session_.connect(service::capability(info, myos::bootstrap::imports::Files),
            service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
            service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), events_,
            service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE), address, value);
    }
    [[nodiscard]] auto list(io::ControlMessage& message) noexcept -> myos_status_t {
        message.operation = static_cast<uint64_t>(Control::List);
        message.size = 0;
        return session_.exchange(message);
    }
    [[nodiscard]] auto open(const char* path, size_t size, File& file) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Open), .size = size};
        if (size > sizeof(message.data)) return MYOS_STATUS_BAD_ARGS;
        service::copy(message.data, path, size);
        const auto status = session_.exchange(message);
        if (status != MYOS_STATUS_OK) return status;
        if (message.size != 8) return MYOS_STATUS_PEER_FAULT;
        file = {message.value, file_size(message)};
        return MYOS_STATUS_OK;
    }
    [[nodiscard]] auto open_object(const char* path, size_t size,
        cap::OwnedCap& object, uint64_t& length, uint64_t& generation) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::OpenObject), .size = size};
        if (size == 0 || size > sizeof(message.data)) return MYOS_STATUS_BAD_ARGS;
        service::copy(message.data, path, size);
        io::ControlPacket packet;
        const auto status = session_.exchange(message, packet);
        if (status != MYOS_STATUS_OK) return status;
        if (packet.count != 1 || message.size != sizeof(uint64_t) || !message.value)
            return MYOS_STATUS_PEER_FAULT;
        length = file_size(message);
        generation = message.value;
        object = libk::move(packet.capabilities[0]);
        return MYOS_STATUS_OK;
    }
    [[nodiscard]] auto close(File file) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Close), .value = file.handle};
        return session_.exchange(message);
    }
    [[nodiscard]] auto close() noexcept -> myos_status_t { return session_.close(); }

    [[nodiscard]] auto backing(File file, myos_word_t access = MYOS_VM_READ) noexcept
        -> std::expected<FileMemory, myos_status_t> {
        if (access != MYOS_VM_READ && access != (MYOS_VM_READ | MYOS_VM_EXECUTE))
            return std::unexpected(MYOS_STATUS_BAD_ARGS);
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Map),
            .value = file.handle, .size = 1};
        message.data[0] = access;
        io::ControlPacket packet;
        const auto status = session_.exchange(message, packet);
        if (status != MYOS_STATUS_OK) return std::unexpected(status);
        if (packet.count != 1 || message.size != 8 || message.value == 0
            || file_size(message) != file.size) return std::unexpected(MYOS_STATUS_PEER_FAULT);
        return FileMemory{libk::move(packet.capabilities[0]), file.size, message.value};
    }

    // Bounded pipelining with ordered consumption, suitable for console output
    // and private package snapshots. Every submitted operation is drained even
    // when one backend read fails; return never leaves a borrowed payload live.
    template<class Consumer>
    [[nodiscard]] auto read(File file, Consumer&& consume) noexcept -> myos_status_t {
        uint64_t offset{};
        while (offset < file.size) {
            std::array<io::Request, io::QueueDepth> batch{};
            size_t count{}, bytes{};
            myos_status_t failure{};
            while (count < batch.size() && offset + bytes < file.size) {
                const auto available = file.size - offset - bytes;
                const size_t length = available < io::BufferSize ? available : io::BufferSize;
                auto& request = batch[count];
                request = {.operation = static_cast<uint64_t>(io::Operation::Read),
                    .object = file.handle, .offset = offset + bytes, .length = length};
                failure = session_.submit(request, nullptr, true);
                if (failure != MYOS_STATUS_OK) break;
                ++count;
                bytes += length;
            }
            for (size_t i = 0; i < count; ++i) {
                io::Completion result{};
                const auto taken = session_.wait(result, batch[i].id);
                const auto status = taken == MYOS_STATUS_OK ? result.status : taken;
                if (failure == MYOS_STATUS_OK) failure = status;
            }
            if (failure != MYOS_STATUS_OK) return failure;
            for (size_t i = 0; i < count; ++i) {
                consume(offset, session_.payload() + batch[i].buffer_offset, batch[i].length);
                offset += batch[i].length;
            }
        }
        return MYOS_STATUS_OK;
    }

private:
    io::ClientSession session_;
    myos_cap_t events_{};
};

} // namespace myos::files

namespace myos::store {

class client final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto connect(const bootstrap::BootstrapView& info,
        bootstrap::Import directory = bootstrap::imports::Store,
        uintptr_t address = 0x76000000) noexcept -> myos_status_t {
        const auto events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
        uint64_t ignored{};
        return session_.connect(service::capability(info, directory),
            service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
            service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), events,
            service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE), address, ignored, true);
    }

    [[nodiscard]] auto volume_id(uint8_t (&id)[VolumeIdSize]) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::VolumeId)};
        const auto status = session_.exchange(message);
        if (status != MYOS_STATUS_OK) return status;
        if (message.size != sizeof(id)) return MYOS_STATUS_PEER_FAULT;
        service::copy(id, message.data, sizeof(id));
        return MYOS_STATUS_OK;
    }
    [[nodiscard]] auto device_id(uint8_t (&id)[20]) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::DeviceId)};
        const auto status = session_.exchange(message);
        if (status != MYOS_STATUS_OK) return status;
        if (message.size != sizeof(id)) return MYOS_STATUS_PEER_FAULT;
        service::copy(id, message.data, sizeof(id));
        return MYOS_STATUS_OK;
    }
    [[nodiscard]] auto list(io::ControlMessage& message,
        const char* path = nullptr) noexcept -> myos_status_t {
        message.operation = static_cast<uint64_t>(Control::List);
        if (path != nullptr && path[0] != '\0') {
            if (!path_data(message, path)) return MYOS_STATUS_BAD_ARGS;
        } else message.size = 0;
        return session_.exchange(message);
    }
    [[nodiscard]] auto open(const char* path, uint64_t flags,
        cap::OwnedCap& object, uint64_t& length, uint64_t& generation) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Open),
            .value = flags};
        if (!path_data(message, path)) return MYOS_STATUS_BAD_ARGS;
        io::ControlPacket packet;
        const auto status = session_.exchange(message, packet);
        if (status != MYOS_STATUS_OK) return status;
        if (packet.count != 1 || message.size != sizeof(length) || !message.value)
            return MYOS_STATUS_PEER_FAULT;
        service::copy(&length, message.data, sizeof(length));
        generation = message.value;
        object = libk::move(packet.capabilities[0]);
        return MYOS_STATUS_OK;
    }
    [[nodiscard]] auto sync() noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Sync)};
        return session_.exchange(message);
    }
    [[nodiscard]] auto close() noexcept -> myos_status_t { return session_.close(); }
    [[nodiscard]] auto remove(const char* path) noexcept -> myos_status_t {
        return path_operation(Control::Remove, path);
    }
    [[nodiscard]] auto mkdir(const char* path) noexcept -> myos_status_t {
        return path_operation(Control::Mkdir, path);
    }
    [[nodiscard]] auto rename(const char* from, const char* to) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Rename)};
        const size_t left = service::length(from), right = service::length(to);
        if (left == 0 || right == 0 || left + right + 1 > sizeof(message.data))
            return MYOS_STATUS_BAD_ARGS;
        service::copy(message.data, from, left);
        message.data[left] = '\0';
        service::copy(message.data + left + 1, to, right);
        message.size = left + right + 1;
        return session_.exchange(message);
    }

    [[nodiscard]] auto format(const uint8_t* id = nullptr) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Format)};
        if (id != nullptr) {
            message.size = VolumeIdSize;
            service::copy(message.data, id, message.size);
        }
        return session_.exchange(message);
    }

private:
    [[nodiscard]] static auto path_data(io::ControlMessage& message,
        const char* path) noexcept -> bool {
        message.size = service::length(path);
        if (message.size == 0 || message.size > sizeof(message.data)) return false;
        service::copy(message.data, path, message.size);
        return true;
    }
    [[nodiscard]] auto path_operation(Control operation, const char* path) noexcept
        -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(operation)};
        if (!path_data(message, path)) return MYOS_STATUS_BAD_ARGS;
        return session_.exchange(message);
    }
    io::ClientSession session_;
};

} // namespace myos::store
