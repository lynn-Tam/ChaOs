#pragma once

#include <servers/files/fat32.hpp>
#include <user/lib/io_session.hpp>

namespace myos::files {

// The caller owns a Read until completion. Accepted identity and output stay
// fixed even when its originating file handle closes.
struct Read final {
    size_t file{};
    uint64_t offset{};
    uint8_t* output{};
    size_t size{}, done{};
    void* context{};
    void (*complete)(Read&, myos_status_t) noexcept{};
    bool (*cancelled)(const Read&) noexcept{};
    bool active{}, submitted{};
};

class Reader final {
    struct Forward final {
        Read* read{};
        uint64_t id{};
        fat32::Extent extent{};
    };
    io::ClientSession& backend_;
    const fat32::Volume& volume_;
    Forward forwards_[io::QueueDepth]{};

    static auto cancelled(const Read& read) noexcept -> bool {
        return read.cancelled != nullptr && read.cancelled(read);
    }
    static void finish(Read& read, myos_status_t status) noexcept {
        read.active = false;
        read.submitted = false;
        read.complete(read, status);
    }

public:
    Reader(io::ClientSession& backend, const fat32::Volume& volume) noexcept
        : backend_(backend), volume_(volume) {}

    auto poll() noexcept -> myos_status_t {
        for (;;) {
            io::Completion completion;
            const auto result = backend_.queue().take(completion);
            if (result == libk::RingResult::Empty) return MYOS_STATUS_OK;
            if (result != libk::RingResult::Ready) return MYOS_STATUS_PEER_FAULT;
            size_t slot{};
            while (slot < io::QueueDepth && forwards_[slot].id != completion.id) ++slot;
            if (slot == io::QueueDepth || forwards_[slot].read == nullptr || completion.flags != 0)
                return MYOS_STATUS_PEER_FAULT;
            const auto forward = forwards_[slot];
            forwards_[slot] = {};
            auto& read = *forward.read;
            read.submitted = false;
            if (cancelled(read)) finish(read, MYOS_STATUS_CANCELED);
            else if (completion.status != MYOS_STATUS_OK) finish(read, completion.status);
            else {
                if (completion.bytes != forward.extent.size) return MYOS_STATUS_PEER_FAULT;
                service::copy(read.output + read.done,
                    backend_.payload() + slot * io::BufferSize + forward.extent.skip, forward.extent.bytes);
                read.done += forward.extent.bytes;
                if (read.done == read.size) finish(read, MYOS_STATUS_OK);
            }
        }
    }

    auto submit(Read& read) noexcept -> myos_status_t {
        if (!read.active || read.submitted) return MYOS_STATUS_OK;
        if (cancelled(read)) { finish(read, MYOS_STATUS_CANCELED); return MYOS_STATUS_OK; }
        if (read.done == read.size) { finish(read, MYOS_STATUS_OK); return MYOS_STATUS_OK; }
        size_t slot{};
        while (slot < io::QueueDepth && forwards_[slot].read != nullptr) ++slot;
        if (slot == io::QueueDepth) return MYOS_STATUS_OK;
        const auto extent = volume_.extent(read.file, read.offset + read.done, read.size - read.done);
        io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
            .offset = extent.offset, .buffer_offset = slot * io::BufferSize, .length = extent.size};
        const auto result = backend_.queue().submit(request);
        if (result == libk::RingResult::Full) return MYOS_STATUS_OK;
        if (result != libk::RingResult::Ready) return MYOS_STATUS_PEER_FAULT;
        read.submitted = true;
        forwards_[slot] = {&read, request.id, extent};
        return MYOS_STATUS_OK;
    }
};

} // namespace myos::files
