#include <user/lib/imports.hpp>
#include <user/lib/file_protocol.hpp>
#include <uapi/test_scenario.h>

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
    const auto directory = service::capability(info, bootstrap::imports::Files);
    const auto pool = service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    // No descendant exists: completion precedes arming the blocking revoke.
    const auto disposable = memory_create(pool, 4096, MYOS_VM_READ | MYOS_VM_WRITE);
    service::require(disposable.status);
    cap::OwnedCap temporary{{disposable.value, 0}};
    service::require(cap_revoke(temporary.selector(), false).status);
    service::require(object_destroy(temporary.selector()).status);
    temporary = {};
    uint64_t handles[2]{};
    for (size_t i = 0; i < 2; ++i) {
        uint64_t value{};
        service::require(sessions[i].connect(directory, pool, service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), events, vspace,
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
    MappedMemory mappings[2];
    uint64_t identity{};
    constexpr size_t mapped_size = (FileSize + 4095) & ~size_t{4095};
    for (size_t i = 0; i < 2; ++i) {
        io::ControlMessage request{.operation = static_cast<uint64_t>(files::Control::Map),
            .value = handles[i], .size = 1};
        request.data[0] = MYOS_VM_READ | MYOS_VM_EXECUTE;
        io::ControlPacket packet;
        check(sessions[i].exchange(request, packet) == MYOS_STATUS_DENIED && packet.count == 0);
        request = {.operation = static_cast<uint64_t>(files::Control::Map), .value = handles[i], .size = 1};
        request.data[0] = MYOS_VM_READ;
        service::require(sessions[i].exchange(request, packet));
        check(packet.count == 1 && request.size == 8 && files::file_size(request) == FileSize);
        if (i == 0) identity = request.value;
        else check(identity != 0 && identity == request.value);
        // The response conveys Map-only content authority; it cannot initialize
        // or modify the server's canonical backing object.
        check(memory_write(packet.capabilities[0].selector(), 0, 0, 1).status == MYOS_STATUS_BAD_RIGHTS);
        auto mapped = MappedMemory::map(vspace, libk::move(packet.capabilities[0]),
            0x75000000 + i * 0x100000, mapped_size, MYOS_VM_READ);
        check(static_cast<bool>(mapped));
        mappings[i] = libk::move(mapped).value();
        const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mappings[i].address);
        for (size_t n = 0; n < mapped_size; ++n)
            check(bytes[n] == (n < FileSize ? n % 251 : 0));
    }
    // The pressure kernel drains free frames on this first fault. The same
    // consumer and artifacts also run normally; no service has a test mode.
    const auto fresh = [&](uintptr_t address) {
        auto mapping = MappedMemory::create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
            vspace, address, 4096);
        check(static_cast<bool>(mapping));
        return libk::move(mapping).value();
    };
    auto stress = fresh(MYOS_TEST_PRESSURE_STRESS_ADDRESS);
    auto release = fresh(MYOS_TEST_PRESSURE_RELEASE_ADDRESS);
    check(*reinterpret_cast<const volatile uint8_t*>(stress.address) == 0);
    for (size_t pass = 0; pass < 8; ++pass) {
        for (size_t page = 0; page < mapped_size; page += 4096) {
            const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mappings[pass % 2].address);
            check(bytes[page] == page % 251);
        }
    }
    check(*reinterpret_cast<const volatile uint8_t*>(release.address) == 0);
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
        service::require(sessions[i].close());
    }
    // File/session close ends admission, while an already granted immutable
    // content capability and its mapping have their own lifetime.
    for (size_t i = 0; i < 2; ++i) {
        const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mappings[i].address);
        check(bytes[FileSize - 1] == (FileSize - 1) % 251);
        service::require(mappings[i].close());
    }
    // Reuse the same slots and virtual ranges. Old handles and payload bytes
    // must not cross sessions; teardown must keep returning resources to the pool.
    for (size_t round = 0; round < 64; ++round) {
        for (size_t i = 0; i < 2; ++i) {
            uint64_t value{};
            service::require(sessions[i].connect(directory, pool,
                service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), events, vspace,
                0x70000000 + i * 0x100000, value));
            for (size_t byte = 0; byte < io::PayloadSize; ++byte) check(sessions[i].payload()[byte] == 0);
            io::ControlMessage stale{.operation = static_cast<uint64_t>(files::Control::Map), .value = handles[i], .size = 1};
            stale.data[0] = MYOS_VM_READ;
            check(sessions[i].exchange(stale) == MYOS_STATUS_NOT_FOUND);
            io::ControlMessage opened{.operation = static_cast<uint64_t>(files::Control::Open), .size = 8};
            service::copy(opened.data, "DATA.BIN", 8);
            service::require(sessions[i].exchange(opened));
            handles[i] = opened.value;
            io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
                .object = opened.value, .offset = 13, .length = io::BufferSize};
            check(sessions[i].queue().submit(request) == libk::RingResult::Ready);
            service::require(sessions[i].flush());
            io::Completion completion;
            for (;;) {
                const auto taken = sessions[i].queue().take(completion);
                if (taken == libk::RingResult::Ready) break;
                check(taken == libk::RingResult::Empty);
                service::require(sessions[i].arm());
                service::require(notification_wait(events).status);
            }
            check(completion.status == MYOS_STATUS_OK && completion.bytes == io::BufferSize);
            check(sessions[i].payload()[0] == 13);
        }
        for (auto& session : sessions) service::require(session.close());
    }
    exit();
}
