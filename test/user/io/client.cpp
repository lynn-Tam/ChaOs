#include <user/server_rt/service.hpp>
#include <user/abi/startup.hpp>
#include <user/ipc/io.hpp>

namespace {
myos::io::ControlPort control;
myos::io::ClientMemory memory;
myos_cap_t events;

auto receive() noexcept -> myos::io::ControlPacket {
    using namespace myos;
    for (;;) {
        io::ControlPacket packet;
        const auto status = control.receive(packet);
        if (status == MYOS_STATUS_OK) return packet;
        if (status != MYOS_STATUS_WOULD_BLOCK && status != MYOS_STATUS_BUSY) exit(status);
        service::require(control.arm());
        service::require(notification_wait(events).status);
    }
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    const auto pair = channel_create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
        1, MYOS_CHANNEL_MAX_WORDS, 4, 2);
    service::require(pair.status);
    cap::OwnedCap client{{pair.value, 0}}, server{{pair.value2, 0}};
    const auto own_endpoint = channel_mint(client.selector(), service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE),
        1, MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_CLOSE);
    service::require(own_endpoint.status);
    client = cap::OwnedCap{{own_endpoint.value, 0}};
    service::require(control.open(client.selector(), events));
    const auto endpoint = channel_mint(server.selector(), service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE),
        1, MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_CLOSE | MYOS_RIGHT_DUPLICATE);
    service::require(endpoint.status);
    cap::OwnedCap fixed{{endpoint.value, 0}};
    const myos_cap_transfer transfers[]{
        {fixed.selector(), MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_CLOSE, MYOS_CAP_COPY, 0},
        {events, MYOS_RIGHT_SIGNAL, MYOS_CAP_COPY, 0}};
    service::require(io::ControlPort::send_to(service::capability(info, bootstrap::imports::Block),
        {.operation = static_cast<uint64_t>(io::Control::Open), .id = 1, .value = io::QueueDepth}, transfers, 2));
    auto opened = receive();
    service::require(opened.message.status);
    if (opened.message.id != 1 || opened.message.value != 1024 * 1024) exit(MYOS_STATUS_BAD_ARGS);
    for (size_t index = 1; index < 2; ++index) {
        const uintptr_t probe = 0x71000000 + index * 4096;
        const auto region = vm_create_region(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
            probe, 4096, MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_NORMAL, MYOS_RIGHT_MAP);
        service::require(region.status);
        cap::OwnedCap owner{{region.value, 0}};
        if (vm_map(region.value, opened.capabilities[index].selector(), probe, 4096, 0,
            MYOS_VM_READ | MYOS_VM_WRITE).status != MYOS_STATUS_BAD_RIGHTS) exit(MYOS_STATUS_INTERNAL);
    }
    service::require(memory.map(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        0x70000000, opened, true));
    if (memory.writable_payload() == nullptr) exit(MYOS_STATUS_INTERNAL);
    service::require(memory.signal());
    io::ClientQueue queue{memory.client(), memory.server()};
    for (size_t batch = 0; batch < 16; ++batch) {
        uint64_t expected[io::QueueDepth]{};
        myos_status_t expected_status[io::QueueDepth]{};
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
                expected_status[slot] = batch == 5 ? MYOS_STATUS_DENIED : MYOS_STATUS_BAD_ARGS;
            }
            if (queue.submit(request) != libk::RingResult::Ready) exit(MYOS_STATUS_INTERNAL);
            expected[slot] = request.id;
        }
        if (!queue.publish()) exit(MYOS_STATUS_INTERNAL);
        service::require(memory.signal());
        const uint64_t cancelled = expected[io::QueueDepth - 1];
        service::require(control.send({.operation = static_cast<uint64_t>(io::Control::Cancel),
            .id = batch + 2, .value = cancelled}));
        auto reply = receive();
        if (reply.count != 0 || reply.message.id != batch + 2
            || (reply.message.status != MYOS_STATUS_OK && reply.message.status != MYOS_STATUS_NOT_FOUND))
            exit(MYOS_STATUS_BAD_ARGS);
        service::require(memory.signal()); // release the control reply's queue credit
        size_t remaining = io::QueueDepth;
        while (remaining != 0) {
            io::Completion completion;
            for (;;) {
                const auto result = queue.take(completion);
                if (result == libk::RingResult::Empty) break;
                if (result != libk::RingResult::Ready) exit(MYOS_STATUS_PEER_FAULT);
                size_t slot{};
                while (slot < io::QueueDepth && expected[slot] != completion.id) ++slot;
                if (slot == io::QueueDepth) exit(MYOS_STATUS_INTERNAL);
                const bool cancellation = completion.id == cancelled && reply.message.status == MYOS_STATUS_OK;
                const auto status = cancellation ? MYOS_STATUS_CANCELED : expected_status[slot];
                if (completion.status != status
                    || completion.bytes != (status == MYOS_STATUS_OK ? io::BufferSize : 0)) exit(MYOS_STATUS_INTERNAL);
                if (status == MYOS_STATUS_OK) {
                    for (size_t index = 0; index < io::BufferSize; ++index)
                        if (memory.payload()[slot * io::BufferSize + index] != 0) exit(MYOS_STATUS_BACKING_FAILED);
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
        if (queue.submit(request) != libk::RingResult::Ready) exit(MYOS_STATUS_INTERNAL);
    }
    if (!queue.publish()) exit(MYOS_STATUS_INTERNAL);
    service::require(memory.signal());
    service::require(control.send({.operation = static_cast<uint64_t>(io::Control::Close), .id = 18}));
    const auto closed = receive();
    if (closed.count != 0 || closed.message.id != 18) exit(MYOS_STATUS_BAD_ARGS);
    service::require(closed.message.status);
    exit();
}
