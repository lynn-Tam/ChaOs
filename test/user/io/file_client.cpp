#include <string.h>
#include <utility>
#include <sys/channel.hpp>
#include <sys/start.hpp>
#include <sys/protocol.hpp>

namespace {
using namespace sys;
io::ClientSession sessions[2];
constexpr uint64_t FileSize = 16397;
void check(bool condition) { if (!condition) exit(STATUS_INTERNAL); }
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    const auto info = sys::bootstrap(address, size);
    const auto events = sys::capability(info, BOOT_EVENTS);
    const auto vspace = sys::capability(info, BOOT_VSPACE);
    const auto directory = sys::capability(info, boot::Files);
    const auto pool = sys::capability(info, BOOT_POOL);
    // No descendant exists: completion precedes arming the blocking revoke.
    const auto disposable = memory_create(pool, 4096, VM_READ | VM_WRITE);
    sys::require(disposable.status);
    cap_owner temporary{disposable.value};
    sys::require(cap_revoke(temporary.selector(), false).status);
    sys::require(object_destroy(temporary.selector()).status);
    temporary = {};
    uint64_t handles[2]{};
    for (size_t i = 0; i < 2; ++i) {
        uint64_t value{};
        sys::require(sessions[i].connect(directory, pool, sys::capability(info, BOOT_CSPACE), events, vspace,
            0x70000000 + i * 0x100000, value));
        sys::Message list{.operation = static_cast<uint64_t>(fs::Control::List)};
        sys::require(sessions[i].exchange(list));
        check(list.size != 0 && list.value == 0);
        sys::Message opened{.operation = static_cast<uint64_t>(fs::Control::Open), .size = 9};
        ::memcpy(opened.data, "/data.bin", 9);
        sys::require(sessions[i].exchange(opened));
        check(opened.size == 8 && fs::file_size(opened) == FileSize);
        handles[i] = opened.value;
    }
    mapping mappings[2];
    uint64_t identity{};
    constexpr size_t mapped_size = (FileSize + 4095) & ~size_t{4095};
    for (size_t i = 0; i < 2; ++i) {
        sys::Message request{.operation = static_cast<uint64_t>(fs::Control::Map),
            .value = handles[i], .size = 1};
        request.data[0] = VM_READ | VM_EXECUTE;
        sys::Packet packet;
        check(sessions[i].exchange(request, packet) == STATUS_DENIED && packet.count == 0);
        request = {.operation = static_cast<uint64_t>(fs::Control::Map), .value = handles[i], .size = 1};
        request.data[0] = VM_READ;
        sys::require(sessions[i].exchange(request, packet));
        check(packet.count == 1 && request.size == 8 && fs::file_size(request) == FileSize);
        if (i == 0) identity = request.value;
        else check(identity != 0 && identity == request.value);
        // The response conveys Map-only content authority; it cannot initialize
        // or modify the server's canonical backing object.
        check(memory_write(packet.capabilities[0].selector(), 0, 0, 1).status == STATUS_BAD_RIGHTS);
        check(mem_trim(packet.capabilities[0].selector(), 0, 1).status == STATUS_BAD_RIGHTS);
        check(mem_writeback(packet.capabilities[0].selector(), 0).status == STATUS_BAD_RIGHTS);
        auto mapped = mapping::map(vspace, std::move(packet.capabilities[0]),
            0x75000000 + i * 0x100000, mapped_size, VM_READ);
        check(static_cast<bool>(mapped));
        mappings[i] = std::move(*mapped);
        const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mappings[i].address);
        for (size_t n = 0; n < mapped_size; ++n)
            check(bytes[n] == (n < FileSize ? n % 251 : 0));
    }
    // Files exports one immutable source to every client. A private mapping
    // initially reads that source, then owns only the pages it changes.
    sys::Message private_request{.operation = static_cast<uint64_t>(fs::Control::Map),
        .value = handles[0], .size = 1};
    private_request.data[0] = VM_READ;
    sys::Packet private_packet;
    sys::require(sessions[0].exchange(private_request, private_packet));
    check(private_packet.count == 1 && private_request.value == identity);
    auto private_result = mapping::map_private(vspace,
        std::move(private_packet.capabilities[0]), 0x76000000, mapped_size);
    check(static_cast<bool>(private_result));
    auto private_mapping = std::move(*private_result);
    auto* private_bytes = reinterpret_cast<volatile uint8_t*>(private_mapping.address);
    check(private_bytes[0] == 0 && private_bytes[4096] == 4096 % 251);
    private_bytes[0] = 97;
    private_bytes[4096] = 98;
    private_bytes[FileSize] = 99;
    check(private_bytes[0] == 97 && private_bytes[4096] == 98 && private_bytes[FileSize] == 99);
    for (const auto& shared : mappings) {
        const auto* bytes = reinterpret_cast<const volatile uint8_t*>(shared.address);
        check(bytes[0] == 0 && bytes[4096] == 4096 % 251 && bytes[FileSize] == 0);
    }
    sys::require(private_mapping.close());
    for (size_t pass = 0; pass < 8; ++pass) {
        for (size_t page = 0; page < mapped_size; page += 4096) {
            const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mappings[pass % 2].address);
            check(bytes[page] == page % 251);
        }
    }
    for (size_t batch = 0; batch < 16; ++batch) {
        uint64_t ids[2][io::QueueDepth]{}, offsets[2][io::QueueDepth]{};
        for (size_t i = 0; i < 2; ++i) {
            for (size_t slot = 0; slot < io::QueueDepth; ++slot) {
                const uint64_t offset = (batch * 1031 + slot * 509 + i * 13) % (FileSize + 512);
                io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
                    .object = handles[i], .offset = offset, .buffer_offset = slot * io::BufferSize,
                    .length = io::BufferSize};
                check(sessions[i].queue().submit(request) == libk::ring_status::ready);
                ids[i][slot] = request.id; offsets[i][slot] = offset;
            }
            sys::require(sessions[i].flush());
        }
        size_t remaining = 2 * io::QueueDepth;
        while (remaining != 0) {
            for (size_t i = 0; i < 2; ++i) {
                for (;;) {
                    io::Completion completion;
                    const auto result = sessions[i].queue().take(completion);
                    if (result == libk::ring_status::empty) break;
                    check(result == libk::ring_status::ready);
                    size_t slot{};
                    while (slot < io::QueueDepth && ids[i][slot] != completion.id) ++slot;
                    check(slot < io::QueueDepth && completion.status == STATUS_OK && completion.flags == 0);
                    const auto offset = offsets[i][slot];
                    const auto available = offset >= FileSize ? 0 : FileSize - offset;
                    const auto expected = available < io::BufferSize ? available : io::BufferSize;
                    check(completion.bytes == expected);
                    for (size_t n = 0; n < expected; ++n)
                        check(sessions[i].payload()[slot * io::BufferSize + n] == (offset + n) % 251);
                    ids[i][slot] = 0; --remaining;
                }
                sys::require(sessions[i].flush());
                sys::require(sessions[i].arm());
            }
            if (remaining != 0) sys::require(notification_wait(events).status);
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        sys::Message closed{.operation = static_cast<uint64_t>(fs::Control::Close), .value = handles[i]};
        sys::require(sessions[i].exchange(closed));
        sys::Message opened{.operation = static_cast<uint64_t>(fs::Control::Open), .size = 8};
        ::memcpy(opened.data, "DATA.BIN", 8);
        sys::require(sessions[i].exchange(opened));
        check(opened.value != handles[i]);
        closed.value = handles[i];
        check(sessions[i].exchange(closed) == STATUS_NOT_FOUND);
        // Close while reads remain published; acknowledgement ends all admitted
        // downstream access without requiring the caller to consume every CQ.
        for (size_t slot = 0; slot < io::QueueDepth; ++slot) {
            io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
                .object = opened.value, .offset = 13, .buffer_offset = slot * io::BufferSize, .length = io::BufferSize};
            check(sessions[i].queue().submit(request) == libk::ring_status::ready);
        }
        sys::require(sessions[i].flush());
        sys::require(sessions[i].close());
    }
    // File/session close ends admission, while an already granted immutable
    // content capability and its mapping have their own lifetime.
    for (size_t i = 0; i < 2; ++i) {
        const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mappings[i].address);
        check(bytes[FileSize - 1] == (FileSize - 1) % 251);
        sys::require(mappings[i].close());
    }
    // Reuse the same slots and virtual ranges. Old handles and payload bytes
    // must not cross sessions; teardown must keep returning resources to the pool.
    for (size_t round = 0; round < 64; ++round) {
        for (size_t i = 0; i < 2; ++i) {
            uint64_t value{};
            sys::require(sessions[i].connect(directory, pool,
                sys::capability(info, BOOT_CSPACE), events, vspace,
                0x70000000 + i * 0x100000, value));
            for (size_t byte = 0; byte < io::PayloadSize; ++byte) check(sessions[i].payload()[byte] == 0);
            sys::Message stale{.operation = static_cast<uint64_t>(fs::Control::Map), .value = handles[i], .size = 1};
            stale.data[0] = VM_READ;
            check(sessions[i].exchange(stale) == STATUS_NOT_FOUND);
            sys::Message opened{.operation = static_cast<uint64_t>(fs::Control::Open), .size = 8};
            ::memcpy(opened.data, "DATA.BIN", 8);
            sys::require(sessions[i].exchange(opened));
            handles[i] = opened.value;
            io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
                .object = opened.value, .offset = 13, .length = io::BufferSize};
            check(sessions[i].queue().submit(request) == libk::ring_status::ready);
            sys::require(sessions[i].flush());
            io::Completion completion;
            for (;;) {
                const auto taken = sessions[i].queue().take(completion);
                if (taken == libk::ring_status::ready) break;
                check(taken == libk::ring_status::empty);
                sys::require(sessions[i].arm());
                sys::require(notification_wait(events).status);
            }
            check(completion.status == STATUS_OK && completion.bytes == io::BufferSize);
            check(sessions[i].payload()[0] == 13);
        }
        for (auto& session : sessions) sys::require(session.close());
    }
    exit();
}
