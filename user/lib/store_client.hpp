#pragma once

#include <user/lib/imports.hpp>
#include <user/lib/store_protocol.hpp>

namespace myos::store {

struct File final { uint64_t handle{}, size{}; };

class Session : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto connect(const bootstrap::BootstrapView& info,
        bootstrap::Import directory = bootstrap::imports::Store,
        uintptr_t address = 0x76000000) noexcept -> myos_status_t {
        events_ = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
        uint64_t ignored{};
        return session_.connect(service::capability(info, directory),
            service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
            service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), events_,
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
    [[nodiscard]] auto open(const char* path, uint64_t flags, File& file) noexcept
        -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Open),
            .value = flags};
        if (!path_data(message, path)) return MYOS_STATUS_BAD_ARGS;
        const auto status = session_.exchange(message);
        if (status != MYOS_STATUS_OK) return status;
        if (message.size != sizeof(uint64_t)) return MYOS_STATUS_PEER_FAULT;
        file.handle = message.value;
        service::copy(&file.size, message.data, sizeof(file.size));
        return MYOS_STATUS_OK;
    }
    [[nodiscard]] auto sync(File file) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Sync),
            .value = file.handle};
        return session_.exchange(message);
    }
    [[nodiscard]] auto close(File file) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Close),
            .value = file.handle};
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

    [[nodiscard]] auto write(File file, uint64_t offset,
        const uint8_t* data, size_t size) noexcept -> myos_status_t {
        if (data == nullptr && size != 0) return MYOS_STATUS_BAD_ARGS;
        while (size != 0) {
            const size_t chunk = size < io::BufferSize ? size : io::BufferSize;
            service::copy(session_.writable_payload(), data, chunk);
            uint64_t written{};
            const auto result = transfer(io::Operation::Write, file.handle, offset, chunk, written);
            if (result != MYOS_STATUS_OK) return result;
            if (written != chunk) return MYOS_STATUS_BACKING_FAILED;
            data += chunk;
            offset += chunk;
            size -= chunk;
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto read_at(File file, uint64_t offset, uint8_t* data,
        size_t size, uint64_t& bytes) noexcept -> myos_status_t {
        if (size > io::BufferSize || (size != 0 && data == nullptr)) return MYOS_STATUS_BAD_ARGS;
        if (size == 0) { bytes = 0; return MYOS_STATUS_OK; }
        const auto status = transfer(io::Operation::Read, file.handle, offset, size, bytes);
        if (status == MYOS_STATUS_OK) service::copy(data, session_.payload(), bytes);
        return status;
    }

    template<class Consumer>
    [[nodiscard]] auto read(File file, Consumer&& consume) noexcept -> myos_status_t {
        uint64_t offset{};
        while (offset < file.size) {
            const size_t chunk = file.size - offset < io::BufferSize
                ? file.size - offset : io::BufferSize;
            uint64_t bytes{};
            const auto result = transfer(io::Operation::Read, file.handle, offset, chunk, bytes);
            if (result != MYOS_STATUS_OK) return result;
            if (bytes == 0) return MYOS_STATUS_BACKING_FAILED;
            consume(offset, session_.payload(), bytes);
            offset += bytes;
        }
        return MYOS_STATUS_OK;
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
    [[nodiscard]] auto transfer(io::Operation operation, uint64_t handle,
        uint64_t offset, size_t size, uint64_t& bytes) noexcept -> myos_status_t {
        io::Request request{.operation = static_cast<uint64_t>(operation),
            .object = handle, .offset = offset, .length = size};
        if (session_.queue().submit(request) != libk::RingResult::Ready)
            return MYOS_STATUS_PEER_FAULT;
        auto status = session_.flush();
        if (status != MYOS_STATUS_OK) return status;
        for (;;) {
            io::Completion completion{};
            const auto taken = session_.queue().take(completion);
            if (taken == libk::RingResult::Ready) {
                if (completion.id != request.id || completion.flags != 0
                    || completion.bytes > size) return MYOS_STATUS_PEER_FAULT;
                bytes = completion.bytes;
                status = session_.flush();
                return status == MYOS_STATUS_OK ? completion.status : status;
            }
            if (taken != libk::RingResult::Empty) return MYOS_STATUS_PEER_FAULT;
            status = session_.arm();
            if (status != MYOS_STATUS_OK) return status;
            status = notification_wait(events_).status;
            if (status != MYOS_STATUS_OK) return status;
        }
    }

protected:
    io::ClientSession session_{};
private:
    myos_cap_t events_{};
};

class Client final : public Session {
public:
    [[nodiscard]] auto format(const uint8_t* id = nullptr) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Format)};
        if (id != nullptr) {
            message.size = VolumeIdSize;
            service::copy(message.data, id, message.size);
        }
        return session_.exchange(message);
    }
};

} // namespace myos::store
