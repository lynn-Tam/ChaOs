#include <servers/runtime/service.hpp>
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
    const auto info = service::bootstrap(address, size);
    const auto pool = service::capability(info, BOOT_POOL);
    const auto events = notification_create(pool, 1);
    service::require(events.status);
    cap::OwnedCap event{{events.value, 0}};
    io::ClientSession session;
    uint64_t capacity{};
    service::require(session.connect(service::capability(info, boot::Block),
        pool, service::capability(info, BOOT_CSPACE), event.selector(),
        service::capability(info, BOOT_VSPACE), 0x70000000, capacity, true));
    if (capacity < Offset + Size || session.writable_payload() == nullptr)
        exit(STATUS_BAD_ARGS);

    auto run = [&](io::Operation operation, size_t bytes) noexcept {
        io::Request request{.operation = static_cast<uint64_t>(operation),
            .offset = operation == io::Operation::Flush ? 0 : Offset,
            .length = bytes};
        if (session.queue().submit(request) != libk::RingResult::Ready)
            exit(STATUS_INTERNAL);
        service::require(session.flush());
        for (;;) {
            io::Completion completion;
            const auto result = session.queue().take(completion);
            if (result == libk::RingResult::Ready) {
                if (completion.id != request.id || completion.status != STATUS_OK
                    || completion.bytes != bytes) exit(STATUS_BACKING_FAILED);
                service::require(session.flush());
                return;
            }
            if (result != libk::RingResult::Empty) exit(STATUS_PEER_FAULT);
            service::require(notification_wait(event.selector()).status);
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
    service::require(session.close());
    exit();
}
