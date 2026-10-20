#pragma once
#include <ubus/ubus.hpp>
#include <vm.hpp>

inline constexpr size_t io_queue_depth = 32, io_buffer_size = 4096;
inline constexpr size_t io_payload_size = io_queue_depth * io_buffer_size;
// These are local backend snapshots, never serialized C++ structures.
enum : uint64_t { io_read = 1, io_write = 2 };
enum class io_operation : uint8_t { Read = 1, Write, Flush, Identify };
struct io_cmd {
    uint64_t id{};
    io_operation operation{};
    uint64_t offset{}, buffer_offset{}, length{};
    bool sequential{};
    uint64_t deadline{};
};
struct io_done { uint64_t id{}; status_t status{}; uint64_t bytes{}; };
struct io_ticket { uint32_t slot{}; uint64_t id{}; };
enum class io_admission { Ready, Empty, Closed };

class io_rx {
    struct entry {
        io_cmd command;
        ubus_request call;
        bool admitted{}, committed{}, cancelled{};
    };
    std::array<entry, io_queue_depth> entries_;
    uint64_t last_{};
    size_t active_{};
    bool stopping_{};
    auto valid(io_ticket) const -> bool;
public:
    auto accept(io_cmd, ubus_request&) -> status_t;
    auto admit(io_ticket&) -> io_admission;
    auto request(io_ticket) const -> const io_cmd*;
    auto cancel(uint64_t) -> status_t;
    auto commit(io_ticket) -> bool;
    auto cancelled(io_ticket) const -> bool;
    auto finish(io_ticket, status_t, uint64_t) -> bool;
    auto abandon(io_ticket) -> bool;
    void stop();
    auto active() const -> size_t { return active_; }
    auto sequence() const -> uint64_t { return last_; }
};

// Native CALL reserves completion storage before admission. No private shared
// command/completion format or notification protocol is layered over the bus.
class io_server {
    ubus_object object_;
    io_rx queue_;
    vm_area memory_;
    cap_owner export_;
    cap_desc descriptor_;
    std::optional<ubus_request> closing_call_;
    uint64_t object_id_{}, value_{};
    bool closing_{}, failed_{}, done_{};
    auto retire() -> status_t;
    auto accept(io_operation, ubus_request&, ubus_msg&) -> status_t;
public:
    ~io_server();
    auto open(uint64_t value, uint64_t mode) -> status_t;
    auto object() -> ubus_object& { return object_; }
    auto poll() -> status_t;
    void abort();
    auto queue() -> io_rx* { return &queue_; }
    auto payload() -> uint8_t* { return reinterpret_cast<uint8_t*>(memory_.address); }
    auto closing() const -> bool { return closing_; }
    auto failed() const -> bool { return failed_; }
    auto done() const -> bool { return done_; }
    auto flush(bool close_ready, status_t close_status = STATUS_OK) -> status_t;
};

class io_client {
    struct entry {
        io_cmd command;
        io_done result;
        ubus_call call;
        void* destination{};
        bool ready{}, exact{};
    };
    ubus_conn connection_;
    cap_owner memory_;
    std::array<std::unique_ptr<entry>, io_queue_depth> entries_;
    cap_t events_{};
    uint64_t next_{};
    status_t error_{};
    bool writable_{}, opened_{};
    void poll();
public:
    ~io_client() { if (opened_) (void)close(); }
    auto open(cap_owner endpoint, bool writable = false, uint64_t* capacity = nullptr) -> status_t;
    auto close() -> status_t;
    // Only a drained handle can move. Re-adoption refreshes its buffer grant
    // and resumes the server's sequence; no backend operation is replayed.
    auto release() -> std::expected<cap_owner, status_t>;
    auto submit(io_cmd&, void* data = nullptr, bool exact = false) -> status_t;
    auto completion(io_done&, uint64_t id = 0) -> status_t;
    auto wait(io_done&, uint64_t id = 0, uint64_t deadline = 0) -> status_t;
    auto cancel(uint64_t id) -> status_t;
    auto invoke(std::string_view method, ubus_msg request, ubus_msg& response) -> status_t {
        return connection_.invoke(method, std::move(request), response);
    }
    auto copy(uint64_t offset, std::span<std::byte> output) const -> status_t {
        return mem_read(memory_.selector(), offset, output);
    }
    auto next(uint64_t after = 0) const -> const io_cmd*;
    auto contains(uint64_t id) const -> bool;
    auto error() -> status_t { poll(); return error_; }
    auto fail(status_t) -> status_t;
};
