#pragma once

#include <user/lib/file_protocol.hpp>

namespace myos::files {

struct File final { uint64_t handle{}, size{}; };

class Client final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto connect(const bootstrap::BootstrapView& info, uintptr_t address = 0x70000000) noexcept
        -> myos_status_t {
        events_ = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
        uint64_t value{};
        return session_.open(service::capability(info, MYOS_BOOTSTRAP_CAP_FILE_CHANNEL), events_,
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
    [[nodiscard]] auto close(File file) noexcept -> myos_status_t {
        io::ControlMessage message{.operation = static_cast<uint64_t>(Control::Close), .value = file.handle};
        return session_.exchange(message);
    }

    // Bounded pipelining with ordered consumption, suitable for console output
    // and private package snapshots. Every submitted operation is drained even
    // when one backend read fails; return never leaves a borrowed payload live.
    template<class Consumer>
    [[nodiscard]] auto read(File file, Consumer&& consume) noexcept -> myos_status_t {
        uint64_t offset{};
        while (offset < file.size) {
            uint64_t ids[io::QueueDepth]{};
            size_t lengths[io::QueueDepth]{};
            bool completed[io::QueueDepth]{};
            size_t count{}, bytes{};
            while (count < io::QueueDepth && offset + bytes < file.size) {
                const auto available = file.size - offset - bytes;
                const size_t length = available < io::BufferSize ? available : io::BufferSize;
                io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
                    .object = file.handle, .offset = offset + bytes,
                    .buffer_offset = count * io::BufferSize, .length = length};
                if (session_.queue().submit(request) != libk::RingResult::Ready) return MYOS_STATUS_PEER_FAULT;
                ids[count] = request.id; lengths[count++] = length; bytes += length;
            }
            auto status = session_.flush();
            if (status != MYOS_STATUS_OK) return status;
            myos_status_t failure = MYOS_STATUS_OK;
            size_t remaining = count;
            while (remaining != 0) {
                for (;;) {
                    io::Completion completion;
                    const auto result = session_.queue().take(completion);
                    if (result == libk::RingResult::Empty) break;
                    if (result != libk::RingResult::Ready) return MYOS_STATUS_PEER_FAULT;
                    size_t slot{};
                    while (slot < count && ids[slot] != completion.id) ++slot;
                    if (slot == count || completed[slot] || completion.flags != 0) return MYOS_STATUS_PEER_FAULT;
                    completed[slot] = true; --remaining;
                    if (failure == MYOS_STATUS_OK) {
                        if (completion.status != MYOS_STATUS_OK) failure = completion.status;
                        else if (completion.bytes != lengths[slot]) failure = MYOS_STATUS_BACKING_FAILED;
                    }
                }
                status = session_.flush();
                if (status != MYOS_STATUS_OK) return status;
                if (remaining != 0) {
                    status = session_.arm();
                    if (status != MYOS_STATUS_OK) return status;
                    status = notification_wait(events_).status;
                    if (status != MYOS_STATUS_OK) return status;
                }
            }
            if (failure != MYOS_STATUS_OK) return failure;
            for (size_t slot = 0; slot < count; ++slot) {
                consume(offset, session_.payload() + slot * io::BufferSize, lengths[slot]);
                offset += lengths[slot];
            }
        }
        return MYOS_STATUS_OK;
    }

private:
    io::ClientSession session_;
    myos_cap_t events_{};
};

} // namespace myos::files
