#pragma once
#include <protocols/queue.hpp>

// One aligned device read and the file bytes within it.
struct io_extent final {
    uint64_t offset{};
    size_t size{}, skip{}, bytes{};
};

struct io_read final {
    uint64_t object{}, offset{};
    uint8_t* output{};
    size_t size{}, done{};
    void* context{};
    void (*complete)(io_read&, status_t) noexcept {};
    bool (*cancelled)(const io_read&) noexcept {};
    bool active{}, submitted{};
};

// Filesystem mapping stays with its owner. This reader only schedules bounded
// physical reads and copies the authorized bytes from each aligned extent.
// Exact-id completion leaves metadata I/O and other owners' results untouched.
template <class Backend, class Mapper, size_t Credits = io_queue_depth> class io_reader final {
    static_assert(Credits && Credits <= io_queue_depth);
    struct flight {
        io_read* read{};
        uint64_t id{}, buffer{};
        io_extent extent{};
    };
    Backend& backend_;
    Mapper map_;
    std::array<flight, Credits> flights_{};

    static auto cancelled(const io_read& read) noexcept -> bool {
        return read.cancelled && read.cancelled(read);
    }
    static void finish(io_read& read, status_t status) noexcept {
        read.active = read.submitted = false;
        read.complete(read, status);
    }
    void complete(flight& slot, const io_done& result) noexcept {
        const auto flight = slot;
        slot = {};
        auto& read = *flight.read;
        read.submitted = false;
        if (cancelled(read))
            finish(read, STATUS_CANCELED);
        else if (result.status != STATUS_OK)
            finish(read, result.status);
        else if (result.bytes != flight.extent.size)
            finish(read, STATUS_PEER_FAULT);
        else {
            ::memcpy(read.output + read.done,
                     backend_.payload() + flight.buffer + flight.extent.skip, flight.extent.bytes);
            read.done += flight.extent.bytes;
            if (read.done == read.size)
                finish(read, STATUS_OK);
        }
    }

  public:
    io_reader(Backend& backend, Mapper map) noexcept : backend_(backend), map_(map) {}

    auto poll() noexcept -> std::expected<bool, status_t> {
        bool progress{};
        for (auto& slot : flights_)
            if (slot.read) {
                io_done result{};
                const auto status = backend_.completion(result, slot.id);
                if (status == STATUS_WOULD_BLOCK)
                    continue;
                if (status != STATUS_OK)
                    return std::unexpected(status);
                complete(slot, result);
                progress = true;
            }
        return progress;
    }
    auto wait() noexcept -> status_t {
        for (auto& slot : flights_)
            if (slot.read) {
                io_done result{};
                const auto status = backend_.wait(result, slot.id);
                if (status == STATUS_OK)
                    complete(slot, result);
                return status;
            }
        return STATUS_WOULD_BLOCK;
    }
    auto submit(io_read& read) noexcept -> std::expected<bool, status_t> {
        if (!read.active || read.submitted)
            return false;
        if (cancelled(read)) {
            finish(read, STATUS_CANCELED);
            return true;
        }
        if (read.done == read.size) {
            finish(read, STATUS_OK);
            return true;
        }
        for (auto& slot : flights_)
            if (!slot.read) {
                const auto io_extent = map_(read);
                if (!io_extent) {
                    finish(read, io_extent.error());
                    return true;
                }
                if (!io_extent->bytes || io_extent->bytes > read.size - read.done ||
                    io_extent->skip > io_extent->size ||
                    io_extent->bytes > io_extent->size - io_extent->skip)
                    return std::unexpected(STATUS_INTERNAL);
                io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                               .offset = io_extent->offset,
                               .length = io_extent->size};
                const auto status = backend_.submit(request);
                if (status == STATUS_BUSY)
                    return false;
                if (status != STATUS_OK) {
                    finish(read, status);
                    return true;
                }
                read.submitted = true;
                slot = {&read, request.id, request.buffer_offset, *io_extent};
                return true;
            }
        return false;
    }
};

// One lifetime per endpoint. Backend owners stop admission on closing(), end
// all backend access, then pass close_ready to flush(). The common transport
// owns page authority, control reply credit and final queue publication.

