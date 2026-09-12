#include <user/lib/file_protocol.hpp>

namespace {
using namespace myos;
io::ClientSession sessions[2];
constexpr uint64_t FileSize = 16397;
void check(bool condition) { if (!condition) exit(MYOS_STATUS_INTERNAL); }
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = service::bootstrap(address, size);
    const auto events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    const auto vspace = service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    constexpr uint32_t roles[] = {MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL, MYOS_BOOTSTRAP_CAP_FILE_CHANNEL};
    uint64_t handles[2]{};
    for (size_t i = 0; i < 2; ++i) {
        uint64_t value{};
        service::require(sessions[i].open(service::capability(info, roles[i]), events, vspace,
            0x70000000 + i * 0x100000, value));
        io::ControlMessage list{.operation = static_cast<uint64_t>(files::Control::List)};
        service::require(sessions[i].exchange(list));
        check(list.size != 0 && list.value == 0);
        io::ControlMessage opened{.operation = static_cast<uint64_t>(files::Control::Open), .size = 9};
        service::copy(opened.data, "/data.bin", 9);
        service::require(sessions[i].exchange(opened));
        check(opened.size == 8 && files::file_size(opened) == FileSize);
        handles[i] = opened.value;
    }
    for (size_t batch = 0; batch < 16; ++batch) {
        uint64_t ids[2][io::QueueDepth]{}, offsets[2][io::QueueDepth]{};
        for (size_t i = 0; i < 2; ++i) {
            for (size_t slot = 0; slot < io::QueueDepth; ++slot) {
                const uint64_t offset = (batch * 1031 + slot * 509 + i * 13) % (FileSize + 512);
                io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
                    .object = handles[i], .offset = offset, .buffer_offset = slot * io::BufferSize,
                    .length = io::BufferSize};
                check(sessions[i].queue().submit(request) == libk::RingResult::Ready);
                ids[i][slot] = request.id; offsets[i][slot] = offset;
            }
            service::require(sessions[i].flush());
        }
        size_t remaining = 2 * io::QueueDepth;
        while (remaining != 0) {
            for (size_t i = 0; i < 2; ++i) {
                for (;;) {
                    io::Completion completion;
                    const auto result = sessions[i].queue().take(completion);
                    if (result == libk::RingResult::Empty) break;
                    check(result == libk::RingResult::Ready);
                    size_t slot{};
                    while (slot < io::QueueDepth && ids[i][slot] != completion.id) ++slot;
                    check(slot < io::QueueDepth && completion.status == MYOS_STATUS_OK && completion.flags == 0);
                    const auto offset = offsets[i][slot];
                    const auto available = offset >= FileSize ? 0 : FileSize - offset;
                    const auto expected = available < io::BufferSize ? available : io::BufferSize;
                    check(completion.bytes == expected);
                    for (size_t n = 0; n < expected; ++n)
                        check(sessions[i].payload()[slot * io::BufferSize + n] == (offset + n) % 251);
                    ids[i][slot] = 0; --remaining;
                }
                service::require(sessions[i].flush());
                service::require(sessions[i].arm());
            }
            if (remaining != 0) service::require(notification_wait(events).status);
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        io::ControlMessage closed{.operation = static_cast<uint64_t>(files::Control::Close), .value = handles[i]};
        service::require(sessions[i].exchange(closed));
        io::ControlMessage opened{.operation = static_cast<uint64_t>(files::Control::Open), .size = 8};
        service::copy(opened.data, "DATA.BIN", 8);
        service::require(sessions[i].exchange(opened));
        check(opened.value != handles[i]);
        closed.value = handles[i];
        check(sessions[i].exchange(closed) == MYOS_STATUS_NOT_FOUND);
        // Close while reads remain published; acknowledgement ends all admitted
        // downstream access without requiring the caller to consume every CQ.
        for (size_t slot = 0; slot < io::QueueDepth; ++slot) {
            io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
                .object = opened.value, .offset = 13, .buffer_offset = slot * io::BufferSize, .length = io::BufferSize};
            check(sessions[i].queue().submit(request) == libk::RingResult::Ready);
        }
        service::require(sessions[i].flush());
        closed = {.operation = static_cast<uint64_t>(io::Control::Close)};
        service::require(sessions[i].exchange(closed));
    }
    exit();
}
