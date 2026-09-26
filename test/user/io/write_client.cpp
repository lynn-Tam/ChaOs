#include <user/lib/imports.hpp>
#include <user/lib/io_session.hpp>

namespace {
constexpr uint64_t Offset = 64 * 512;
constexpr size_t Size = 4096;

auto pattern(size_t index) noexcept -> uint8_t {
    return static_cast<uint8_t>((index * 37 + 19) & 255);
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto pool = service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const auto events = notification_create(pool, 1);
    service::require(events.status);
    cap::OwnedCap event{{events.value, 0}};
    io::ClientSession session;
    uint64_t capacity{};
    service::require(session.connect(service::capability(info, bootstrap::imports::Block),
        pool, service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), event.selector(),
        service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE), 0x70000000, capacity, true));
    if (capacity < Offset + Size || session.writable_payload() == nullptr)
        exit(MYOS_STATUS_BAD_ARGS);

    auto run = [&](io::Operation operation, size_t bytes) noexcept {
        io::Request request{.operation = static_cast<uint64_t>(operation),
            .offset = operation == io::Operation::Flush ? 0 : Offset,
            .length = bytes};
        if (session.queue().submit(request) != libk::RingResult::Ready)
            exit(MYOS_STATUS_INTERNAL);
        service::require(session.flush());
        for (;;) {
            io::Completion completion;
            const auto result = session.queue().take(completion);
            if (result == libk::RingResult::Ready) {
                if (completion.id != request.id || completion.status != MYOS_STATUS_OK
                    || completion.bytes != bytes) exit(MYOS_STATUS_BACKING_FAILED);
                service::require(session.flush());
                return;
            }
            if (result != libk::RingResult::Empty) exit(MYOS_STATUS_PEER_FAULT);
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
        if (session.payload()[i] != pattern(i)) exit(MYOS_STATUS_BACKING_FAILED);
    service::require(session.close());
    exit();
}
