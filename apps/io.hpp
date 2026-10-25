#pragma once
#include <cstddef>
#include <cstdint>

// Public file/block protocol bounds. Stat negotiates each session's actual
// limit/depth; payloads use BufferSlice rather than the control message body.
inline constexpr size_t io_limit = 64 * 1024, io_depth = 32;
enum : uint64_t { io_lookup = 1, io_modify = 2, io_execute = 4, io_admin = 8 };
enum : uint64_t { io_read = 1, io_write = 2, io_create = 4, io_truncate = 8, io_exclusive = 16, io_append = 32 };
enum : int32_t { io_exists = -256, io_not_dir, io_is_dir, io_not_empty, io_name_long, io_too_large, io_no_space, io_cross_device };

#include <ubus/ubus.hpp>
#include <vm.hpp>

inline constexpr size_t io_stream_limit = 4096, io_inline = 48;
struct io_frame { status_t status{}; size_t size{}; char data[io_stream_limit]{}; };

// A bulk stream send retains its buffer until the receiver has copied it.
// Small events use the same FIFO without a Reply. One owner/thread advances a
// transmitter; nonblocking callers retain the current frame until completion.
// Dropping a canceled buffer preserves remote references; Scope retirement
// may revoke an unread payload. Receivers discard that frame,
// preserving the stream and distinguishing abandonment from EOF.
class io_tx {
    _cap_owner memory_;
    ubus_call call_;
public:
    auto active() const -> bool { return call_.active(); }
    auto cancel() -> status_t {
        const auto status = call_.cancel(); memory_ = {}; return status;
    }
    auto send(cap_t endpoint, const io_frame& frame, bool block = true, word_t tag = 0, uint64_t focus = 0) -> status_t {
        if (frame.size > io_stream_limit) return STATUS_BAD_ARGS;
        if (!call_.active()) {
            ubus_msg message; message.status = frame.status; message.tag = tag;
            if (tag) message.data.put("focus", focus);
            if (frame.size <= io_inline) {
                if (frame.size) message.data.put("data", std::as_bytes(std::span{frame.data, frame.size}));
                return tag ? ubus_notify(endpoint, message, block) : ubus_send(endpoint, message, block);
            }
            if (!memory_) {
                const auto created = _memory_create(_startup().alloc(), io_stream_limit, VM_READ | VM_WRITE);
                if (created.status != STATUS_OK) return created.status;
                memory_ = _cap_owner{created.value};
            }
            auto status = _mem_write(memory_.selector(), 0, std::as_bytes(std::span{frame.data, frame.size})).status;
            if (status != STATUS_OK) return status;
            const auto index = message.attach(memory_.selector(), RIGHT_MAP | RIGHT_DUPLICATE);
            if (!index) return index.error();
            message.data.put("buffer", ubus_buffer{*index, 0, frame.size});
            status = call_.begin(endpoint, std::move(message));
            if (status != STATUS_OK) return status;
        }
        if (!block && !call_.pending()) return STATUS_WOULD_BLOCK;
        ubus_msg reply;
        const auto status = block ? call_.wait(reply) : call_.poll(reply);
        if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) memory_ = {};
        return status == STATUS_OK ? reply.status : status;
    }
};
inline auto io_decode(ubus_request& request, io_frame& frame) -> status_t {
    frame.status = request.status; frame.size = 0;
    auto status = STATUS_OK;
    const auto buffer = request.data["buffer"].as<ubus_buffer>();
    const auto bytes = request.data["data"].as<std::span<const std::byte>>();
    if (buffer) {
        if (!request.reply || bytes || request.count != 1 || buffer->size > io_stream_limit
            || buffer->offset > UINT64_MAX - buffer->size) status = STATUS_BAD_ARGS;
        else {
            status = _mem_read(request.capability(request.data["buffer"]), buffer->offset,
                std::as_writable_bytes(std::span{frame.data, size_t(buffer->size)})).status;
            if (status == STATUS_OK) frame.size = buffer->size;
            else if (status == STATUS_CLOSED || status == STATUS_INVALID_CAP || status == STATUS_DENIED) status = STATUS_CANCELED;
        }
    } else if (request.count || request.data["buffer"] || (request.data["data"] && !bytes)
               || (bytes && bytes->size() > io_stream_limit)) status = STATUS_BAD_ARGS;
    else if (bytes) { frame.size = bytes->size(); ::memcpy(frame.data, bytes->data(), frame.size); }
    if (request.reply) { ubus_msg reply; reply.status = status; (void)request.respond(std::move(reply)); }
    return status;
}
inline auto io_recv(ubus_rx& input, io_frame& frame) -> status_t {
    for (size_t n = 0; n < 16; ++n) {
        ubus_request request; auto status = input.receive(request).status;
        if (status == STATUS_OK) status = io_decode(request, frame);
        if (status == STATUS_PEER_FAULT || status == STATUS_TRANSFER_FAILED) {
            if (request.reply) { ubus_msg reply; reply.status = status; (void)request.respond(std::move(reply)); }
            continue;
        }
        if (status != STATUS_CANCELED) return status;
    }
    const auto armed = input.arm(); return armed == STATUS_OK ? STATUS_WOULD_BLOCK : armed;
}
inline auto io_recv(cap_t input, io_frame& frame, bool block = true) -> status_t {
    for (size_t n = 0; block || n < 16; ++n) {
        ubus_request request; auto status = ubus_receive(input, request, block);
        if (status == STATUS_OK) status = io_decode(request, frame);
        if (status == STATUS_PEER_FAULT || status == STATUS_TRANSFER_FAILED) {
            if (request.reply) { ubus_msg reply; reply.status = status; (void)request.respond(std::move(reply)); }
            continue;
        }
        if (status != STATUS_CANCELED) return status;
    }
    return STATUS_WOULD_BLOCK;
}
