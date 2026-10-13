#include <util.hpp>
#include <protocols/block.hpp>
#include <ipc.hpp>
#include <start.hpp>
#include <protocols/queue.hpp>

constexpr uint64_t Offset = 64 * 512;
constexpr size_t Size = 4096;

static auto pattern(size_t index) noexcept -> uint8_t {
    return static_cast<uint8_t>((index * 37 + 19) & 255);
}

int main() {

    const auto& info = rt_startup();
    const auto pool = info.pool;
    const auto events = sys::notification_create(pool, 1);
    if (events.status != STATUS_OK) sys::exit(events.status);
    cap_owner event{events.value};
    io_client session;
    uint64_t capacity{};
    if (const auto rc = session.connect(info.selector(blk_service), pool, info.cspace,
                                        event.selector(), info.vspace, 0x70000000,
                                        capacity, true);
        rc != STATUS_OK)
        sys::exit(rc);
    if (capacity < Offset + Size || session.writable_payload() == nullptr)
        sys::exit(STATUS_BAD_ARGS);

    auto run = [&](io_operation operation, size_t bytes) noexcept {
        io_cmd request{.operation = static_cast<uint64_t>(operation),
                       .offset = operation == io_operation::Flush ? 0 : Offset,
                       .length = bytes};
        if (session.queue().submit(request) != libk::ring_status::ready)
            sys::exit(STATUS_INTERNAL);
        if (const auto rc = session.flush(); rc != STATUS_OK)
            sys::exit(rc);
        for (;;) {
            io_done completion;
            const auto result = session.queue().take(completion);
            if (result == libk::ring_status::ready) {
                if (completion.id != request.id || completion.status != STATUS_OK ||
                    completion.bytes != bytes)
                    sys::exit(STATUS_BACKING_FAILED);
                if (const auto rc = session.flush(); rc != STATUS_OK)
                    sys::exit(rc);
                return;
            }
            if (result != libk::ring_status::empty)
                sys::exit(STATUS_PEER_FAULT);
            if (const auto rc = sys::notification_wait(event.selector()).status; rc != STATUS_OK)
                sys::exit(rc);
        }
    };

#ifndef VERIFY_ONLY
    for (size_t i = 0; i < Size; ++i)
        session.writable_payload()[i] = pattern(i);
    run(io_operation::Write, Size);
    run(io_operation::Flush, 0);
    for (size_t i = 0; i < Size; ++i)
        session.writable_payload()[i] = 0;
#endif
    run(io_operation::Read, Size);
    for (size_t i = 0; i < Size; ++i)
        if (session.payload()[i] != pattern(i))
            sys::exit(STATUS_BACKING_FAILED);
    if (const auto rc = session.close(); rc != STATUS_OK)
        sys::exit(rc);
    sys::exit();
}
