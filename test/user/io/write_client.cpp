#include <sys/protocol.hpp>
#include <sys/channel.hpp>
#include <sys/start.hpp>
#include <sys/queue.hpp>

namespace {
constexpr uint64_t Offset = 64 * 512;
constexpr size_t Size = 4096;

auto pattern(size_t index) noexcept -> uint8_t {
    return static_cast<uint8_t>((index * 37 + 19) & 255);
}
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    const auto pool = sys::capability(info, BOOT_POOL);
    const auto events = notification_create(pool, 1);
    sys::require(events.status);
    cap_owner event{events.value};
    io::ClientSession session;
    uint64_t capacity{};
    sys::require(session.connect(sys::capability(info, boot::Block),
        pool, sys::capability(info, BOOT_CSPACE), event.selector(),
        sys::capability(info, BOOT_VSPACE), 0x70000000, capacity, true));
    if (capacity < Offset + Size || session.writable_payload() == nullptr)
        exit(STATUS_BAD_ARGS);

    auto run = [&](io::Operation operation, size_t bytes) noexcept {
        io::Request request{.operation = static_cast<uint64_t>(operation),
            .offset = operation == io::Operation::Flush ? 0 : Offset,
            .length = bytes};
        if (session.queue().submit(request) != libk::ring_status::ready)
            exit(STATUS_INTERNAL);
        sys::require(session.flush());
        for (;;) {
            io::Completion completion;
            const auto result = session.queue().take(completion);
            if (result == libk::ring_status::ready) {
                if (completion.id != request.id || completion.status != STATUS_OK
                    || completion.bytes != bytes) exit(STATUS_BACKING_FAILED);
                sys::require(session.flush());
                return;
            }
            if (result != libk::ring_status::empty) exit(STATUS_PEER_FAULT);
            sys::require(notification_wait(event.selector()).status);
        }
    };

#ifndef VERIFY_ONLY
    for (size_t i = 0; i < Size; ++i) session.writable_payload()[i] = pattern(i);
    run(io::Operation::Write, Size);
    run(io::Operation::Flush, 0);
    for (size_t i = 0; i < Size; ++i) session.writable_payload()[i] = 0;
#endif
    run(io::Operation::Read, Size);
    for (size_t i = 0; i < Size; ++i)
        if (session.payload()[i] != pattern(i)) exit(STATUS_BACKING_FAILED);
    sys::require(session.close());
    exit();
}
