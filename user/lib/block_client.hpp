#pragma once

#include <user/lib/clock.hpp>
#include <user/lib/io_session.hpp>

namespace myos::block {

// One owner submits one command at a time. The backend owns the DMA buffer;
// this client retains the shared payload until the exact completion arrives.
class Client final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto connect(myos_cap_t directory, myos_cap_t pool,
        myos_cap_t cspace, myos_cap_t events, myos_cap_t vspace,
        uintptr_t address) noexcept -> myos_status_t {
        events_ = events;
        auto status = clock_.open();
        if (status != MYOS_STATUS_OK) return status;
        return session_.connect(directory, pool, cspace, events, vspace,
            address, capacity_, true);
    }

    [[nodiscard]] auto capacity() const noexcept -> uint64_t { return capacity_; }

    [[nodiscard]] auto read(uint64_t offset, uint8_t* data, size_t size) noexcept
        -> myos_status_t {
        return transfer(io::Operation::Read, offset, data, size);
    }
    [[nodiscard]] auto write(uint64_t offset, const uint8_t* data, size_t size) noexcept
        -> myos_status_t {
        return transfer(io::Operation::Write, offset, const_cast<uint8_t*>(data), size);
    }
    [[nodiscard]] auto flush() noexcept -> myos_status_t {
        return command(io::Operation::Flush, 0, nullptr, 0);
    }
    [[nodiscard]] auto identify(uint8_t (&id)[20]) noexcept -> myos_status_t {
        return command(io::Operation::Identify, 0, id, sizeof(id));
    }

private:
    [[nodiscard]] auto command(io::Operation operation, uint64_t offset,
        uint8_t* data, size_t size) noexcept -> myos_status_t {
        if (operation == io::Operation::Identify && (offset != 0 || size != 20))
            return MYOS_STATUS_BAD_ARGS;
        if (operation != io::Operation::Flush && operation != io::Operation::Identify
            && (size == 0 || size > io::BufferSize
            || (size % 512) != 0 || (offset % 512) != 0
            || offset > capacity_ || size > capacity_ - offset)) return MYOS_STATUS_BAD_ARGS;
        auto* payload = session_.writable_payload();
        if (payload == nullptr) return MYOS_STATUS_PEER_FAULT;
        if (operation == io::Operation::Write)
            service::copy(payload, data, size);
        io::Request request{.operation = static_cast<uint64_t>(operation),
            .offset = offset, .length = size};
        if (session_.queue().submit(request) != libk::RingResult::Ready)
            return MYOS_STATUS_PEER_FAULT;
        auto status = session_.flush();
        if (status != MYOS_STATUS_OK) return status;
        const auto deadline = clock_.after_ms(5'000);
        if (!deadline) return MYOS_STATUS_INTERNAL;
        for (;;) {
            io::Completion completion{};
            const auto taken = session_.queue().take(completion);
            if (taken == libk::RingResult::Ready) {
                if (completion.id != request.id || completion.flags != 0
                    || (completion.status == MYOS_STATUS_OK
                        && completion.bytes != (operation == io::Operation::Flush ? 0 : size)))
                    return MYOS_STATUS_PEER_FAULT;
                if (completion.status == MYOS_STATUS_OK
                    && (operation == io::Operation::Read || operation == io::Operation::Identify))
                    service::copy(data, session_.payload(), size);
                const auto released = session_.flush();
                return released == MYOS_STATUS_OK ? completion.status : released;
            }
            if (taken != libk::RingResult::Empty) return MYOS_STATUS_PEER_FAULT;
            status = session_.arm();
            if (status != MYOS_STATUS_OK) return status;
            status = notification_wait(events_, *deadline).status;
            if (status != MYOS_STATUS_OK) return status;
        }
    }

    [[nodiscard]] auto transfer(io::Operation operation, uint64_t offset,
        uint8_t* data, size_t size) noexcept -> myos_status_t {
        if ((data == nullptr && size != 0) || size % 512 != 0 || offset % 512 != 0
            || offset > capacity_ || size > capacity_ - offset) return MYOS_STATUS_BAD_ARGS;
        while (size != 0) {
            const auto chunk = size < io::BufferSize ? size : io::BufferSize;
            const auto status = command(operation, offset, data, chunk);
            if (status != MYOS_STATUS_OK) return status;
            offset += chunk;
            data += chunk;
            size -= chunk;
        }
        return MYOS_STATUS_OK;
    }

    io::ClientSession session_{};
    Clock clock_{};
    myos_cap_t events_{};
    uint64_t capacity_{};
};

} // namespace myos::block
