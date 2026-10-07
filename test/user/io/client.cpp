#include <servers/runtime/service.hpp>
#include <sys/start.hpp>
#include <sys/queue.hpp>

namespace {
sys::io::ControlPort control;
sys::io::ClientMemory memory;
cap_t events;

auto receive() noexcept -> sys::io::ControlPacket {
    using namespace sys;
    for (;;) {
        io::ControlPacket packet;
        const auto status = control.receive(packet);
        if (status == STATUS_OK) return packet;
        if (status != STATUS_WOULD_BLOCK && status != STATUS_BUSY) exit(status);
        service::require(control.arm());
        service::require(notification_wait(events).status);
    }
}
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    events = service::capability(info, BOOT_EVENTS);
    const auto pair = channel_create(service::capability(info, BOOT_POOL),
        1, CHANNEL_MAX_WORDS, 4, 2);
    service::require(pair.status);
    cap::OwnedCap client{{pair.value, 0}}, server{{pair.value2, 0}};
    const auto own_endpoint = channel_mint(client.selector(), service::capability(info, BOOT_CSPACE),
        1, RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE);
    service::require(own_endpoint.status);
    client = cap::OwnedCap{{own_endpoint.value, 0}};
    service::require(control.open(client.selector(), events));
    const auto endpoint = channel_mint(server.selector(), service::capability(info, BOOT_CSPACE),
        1, RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE | RIGHT_DUPLICATE);
    service::require(endpoint.status);
    cap::OwnedCap fixed{{endpoint.value, 0}};
    const CapXfer transfers[]{
        {fixed.selector(), RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE, CAP_COPY, 0},
        {events, RIGHT_SIGNAL, CAP_COPY, 0}};
    service::require(io::ControlPort::send_to(service::capability(info, boot::Block),
        {.operation = static_cast<uint64_t>(io::Control::Open), .id = 1, .value = io::QueueDepth}, transfers, 2));
    auto opened = receive();
    service::require(opened.message.status);
    if (opened.message.id != 1 || opened.message.value != 1024 * 1024) exit(STATUS_BAD_ARGS);
    for (size_t index = 1; index < 2; ++index) {
        const uintptr_t probe = 0x71000000 + index * 4096;
        const auto region = vm_slice(service::capability(info, BOOT_VSPACE),
            probe, 4096, VM_READ | VM_WRITE, RIGHT_MAP);
        service::require(region.status);
        cap::OwnedCap owner{{region.value, 0}};
        if (vm_map(region.value, opened.capabilities[index].selector(), probe, 4096, 0,
            VM_READ | VM_WRITE).status != STATUS_BAD_RIGHTS) exit(STATUS_INTERNAL);
    }
    service::require(memory.map(service::capability(info, BOOT_VSPACE),
        0x70000000, opened, true));
    if (memory.writable_payload() == nullptr) exit(STATUS_INTERNAL);
    service::require(memory.signal());
    io::ClientQueue queue{memory.client(), memory.server()};
    for (size_t batch = 0; batch < 16; ++batch) {
        uint64_t expected[io::QueueDepth]{};
        status_t expected_status[io::QueueDepth]{};
        for (size_t slot = 0; slot < io::QueueDepth; ++slot) {
            io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
                .offset = slot * io::BufferSize, .buffer_offset = slot * io::BufferSize,
                .length = io::BufferSize};
            if (slot == 0 && batch >= 1 && batch <= 5) {
                if (batch == 1) request.offset = UINT64_MAX - 511;
                if (batch == 2) request.buffer_offset = io::PayloadSize + 1;
                if (batch == 3) request.length = 0;
                if (batch == 4) request.operation = UINT64_MAX;
                if (batch == 5) {
                    request.operation = static_cast<uint64_t>(io::Operation::Write);
                    memory.writable_payload()[0] = 0x5a;
                }
                expected_status[slot] = batch == 5 ? STATUS_DENIED : STATUS_BAD_ARGS;
            }
            if (queue.submit(request) != libk::RingResult::Ready) exit(STATUS_INTERNAL);
            expected[slot] = request.id;
        }
        if (!queue.publish()) exit(STATUS_INTERNAL);
        service::require(memory.signal());
        const uint64_t cancelled = expected[io::QueueDepth - 1];
        service::require(control.send({.operation = static_cast<uint64_t>(io::Control::Cancel),
            .id = batch + 2, .value = cancelled}));
        auto reply = receive();
        if (reply.count != 0 || reply.message.id != batch + 2
            || (reply.message.status != STATUS_OK && reply.message.status != STATUS_NOT_FOUND))
            exit(STATUS_BAD_ARGS);
        service::require(memory.signal()); // release the control reply's queue credit
        size_t remaining = io::QueueDepth;
        while (remaining != 0) {
            io::Completion completion;
            for (;;) {
                const auto result = queue.take(completion);
                if (result == libk::RingResult::Empty) break;
                if (result != libk::RingResult::Ready) exit(STATUS_PEER_FAULT);
                size_t slot{};
                while (slot < io::QueueDepth && expected[slot] != completion.id) ++slot;
                if (slot == io::QueueDepth) exit(STATUS_INTERNAL);
                const bool cancellation = completion.id == cancelled && reply.message.status == STATUS_OK;
                const auto status = cancellation ? STATUS_CANCELED : expected_status[slot];
                if (completion.status != status
                    || completion.bytes != (status == STATUS_OK ? io::BufferSize : 0)) exit(STATUS_INTERNAL);
                if (status == STATUS_OK) {
                    for (size_t index = 0; index < io::BufferSize; ++index)
                        if (memory.payload()[slot * io::BufferSize + index] != 0) exit(STATUS_BACKING_FAILED);
                }
                expected[slot] = 0;
                --remaining;
            }
            if (queue.release()) service::require(memory.signal());
            if (remaining != 0) service::require(notification_wait(events).status);
        }
    }
    for (size_t slot = 0; slot < io::QueueDepth; ++slot) {
        io::Request request{.operation = static_cast<uint64_t>(io::Operation::Read),
            .offset = slot * io::BufferSize, .buffer_offset = slot * io::BufferSize, .length = io::BufferSize};
        if (queue.submit(request) != libk::RingResult::Ready) exit(STATUS_INTERNAL);
    }
    if (!queue.publish()) exit(STATUS_INTERNAL);
    service::require(memory.signal());
    service::require(control.send({.operation = static_cast<uint64_t>(io::Control::Close), .id = 18}));
    const auto closed = receive();
    if (closed.count != 0 || closed.message.id != 18) exit(STATUS_BAD_ARGS);
    service::require(closed.message.status);
    exit();
}
