#pragma once
#include <uapi/abi.h>
#include <uapi/cap.h>
#include <array>

// Explicit opt-in: take ownership of startup streams; resolve files lazily through ubus.
auto px_init() noexcept -> status_t;

// Native extensions: preserve native status values, outside the POSIX ABI.
extern "C" {
int32_t px_sync();
int32_t px_mkdir(const char* path);
int32_t px_size(int32_t fd, uint64_t* size);
typedef uint64_t request_id;
enum { IO_READ = 1, IO_WRITE = 2, IO_WAIT = 1, IO_LIMIT = 4096, IO_DEPTH = 32 };

struct io_request {
    int32_t fd;
    uint32_t operation;
    void* data;
    uint64_t length;
    uint64_t offset;
};
struct io_completion {
    request_id id;
    int64_t result;
};

// Explicit offsets never change the open object's implicit offset. Each request
// transfers at most IO_LIMIT bytes. Keep data alive until completion is taken.
int32_t px_submit(const struct io_request* request, request_id* id);
// Returns 1 for a result, 0 if none is ready, or a negative error.
int32_t px_completion(struct io_completion* result, uint32_t flags);
// Success accepts cancellation; the request still needs its completion taken.
// busy means the backend cannot cancel it (including an already finished I/O).
int32_t px_cancel(request_id id);

}
