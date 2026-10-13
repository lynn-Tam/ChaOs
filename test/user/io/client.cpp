#include <util.hpp>
#include <protocols/block.hpp>
#include <protocols/stream.hpp>
#include <ipc.hpp>
#include <start.hpp>
#include <protocols/queue.hpp>

static ch_port control;
static io_mem memory;
static cap_owner peer;
static cap_t events;
static std::array<io_client, 5> sessions;

static auto ch_recv() noexcept -> ch_packet {

    for (;;) {
        ch_packet packet;
        const auto status = control.receive(packet);
        if (status == STATUS_OK)
            return packet;
        if (status != STATUS_WOULD_BLOCK && status != STATUS_BUSY)
            sys::exit(status);
        if (const auto rc = control.arm().status; rc != STATUS_OK)
            sys::exit(rc);
        if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
            sys::exit(rc);
    }
}

int main() {

    const auto& info = rt_startup();
    events = info.events;
    // Multiple independent sessions share hardware credits, not client slots.
    std::array<std::array<uint8_t, 512>, 5> data;
    for (size_t i = 0; i < sessions.size(); ++i) {
        uint64_t capacity{};
        auto status = sessions[i].connect(info.selector(blk_service), info.pool, info.cspace,
                                         events, info.vspace, 0, capacity);
        if (status != STATUS_OK) sys::exit(status);
        if (capacity != 1024 * 1024) sys::exit(STATUS_PEER_FAULT);
        data[i].fill(0xff);
        io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                       .offset = i * 512, .length = 512};
        status = sessions[i].submit(request, data[i].data(), true);
        if (status != STATUS_OK) sys::exit(status);
    }
    for (size_t i = 0; i < sessions.size(); ++i) {
        io_done done;
        if (const auto status = sessions[i].wait(done); status != STATUS_OK) sys::exit(status);
        if (done.status != STATUS_OK || done.bytes != data[i].size()) sys::exit(STATUS_INTERNAL);
        for (const auto byte : data[i])
            if (byte) sys::exit(STATUS_BACKING_FAILED);
    }
    const auto pair = sys::channel_create(info.pool, 1, CHANNEL_MAX_WORDS, 4, 2);
    if (pair.status != STATUS_OK) sys::exit(pair.status);
    cap_owner client{pair.value}, server{pair.value2};
    const auto own_endpoint = sys::channel_mint(client.selector(), info.cspace, 1,
                                                RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE);
    if (own_endpoint.status != STATUS_OK) sys::exit(own_endpoint.status);
    client = cap_owner{own_endpoint.value};
    if (const auto rc = control.open(client.selector(), events); rc != STATUS_OK)
        sys::exit(rc);
    const auto endpoint =
        sys::channel_mint(server.selector(), info.cspace, 1,
                          RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE | RIGHT_DUPLICATE);
    if (endpoint.status != STATUS_OK) sys::exit(endpoint.status);
    cap_owner fixed{endpoint.value};
    const CapXfer transfers[]{
        {fixed.selector(), RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE, CAP_COPY, 0},
        {events, RIGHT_SIGNAL, CAP_COPY, 0}};
    if (const auto rc = ch_port::send_to(info.selector(blk_service),
                                         {.operation = static_cast<uint64_t>(io_control::Open),
                                          .id = 1,
                                          .value = io_queue_depth},
                                         transfers, 2);
        rc != STATUS_OK)
        sys::exit(rc);
    auto opened = ch_recv();
    if (opened.message.status != STATUS_OK) sys::exit(opened.message.status);
    if (opened.message.id != 1 || opened.message.value != 1024 * 1024)
        sys::exit(STATUS_BAD_ARGS);
    for (size_t index = 1; index < 2; ++index) {
        const uintptr_t probe = 0x71000000 + index * 4096;
        const auto region =
            sys::vm_slice(info.vspace, probe, 4096, VM_READ | VM_WRITE, RIGHT_MAP);
        if (region.status != STATUS_OK) sys::exit(region.status);
        cap_owner owner{region.value};
        if (sys::vm_map(region.value, opened.capabilities[index].selector(), probe, 4096, 0,
                        VM_READ | VM_WRITE)
                .status != STATUS_BAD_RIGHTS)
            sys::exit(STATUS_INTERNAL);
    }
    if (const auto rc = memory.map(info.vspace, 0x70000000,
                                   std::span{opened.capabilities}.first<3>(), true);
        rc != STATUS_OK)
        sys::exit(rc);
    peer = std::move(opened.capabilities[3]);
    if (memory.payload() == nullptr)
        sys::exit(STATUS_INTERNAL);
    if (const auto rc = sys::notification_signal(peer.selector()).status; rc != STATUS_OK)
        sys::exit(rc);
    io_tx queue{memory.client(), std::as_const(memory).server()};
    for (size_t batch = 0; batch < 16; ++batch) {
        uint64_t expected[io_queue_depth]{};
        status_t expected_status[io_queue_depth]{};
        for (size_t slot = 0; slot < io_queue_depth; ++slot) {
            io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                           .offset = slot * io_buffer_size,
                           .buffer_offset = slot * io_buffer_size,
                           .length = io_buffer_size};
            if (slot == 0 && batch >= 1 && batch <= 5) {
                if (batch == 1)
                    request.offset = UINT64_MAX - 511;
                if (batch == 2)
                    request.buffer_offset = io_payload_size + 1;
                if (batch == 3)
                    request.length = 0;
                if (batch == 4)
                    request.operation = UINT64_MAX;
                if (batch == 5) {
                    request.operation = static_cast<uint64_t>(io_operation::Write);
                    memory.payload()[0] = 0x5a;
                }
                expected_status[slot] = batch == 5 ? STATUS_DENIED : STATUS_BAD_ARGS;
            }
            if (queue.submit(request) != libk::ring_status::ready)
                sys::exit(STATUS_INTERNAL);
            expected[slot] = request.id;
        }
        if (!queue.publish())
            sys::exit(STATUS_INTERNAL);
        if (const auto rc = sys::notification_signal(peer.selector()).status; rc != STATUS_OK)
            sys::exit(rc);
        const uint64_t cancelled = expected[io_queue_depth - 1];
        if (const auto rc = control
                                .send({.operation = static_cast<uint64_t>(io_control::Cancel),
                                       .id = batch + 2,
                                       .value = cancelled})
                                .status;
            rc != STATUS_OK)
            sys::exit(rc);
        auto reply = ch_recv();
        if (reply.count != 0 || reply.message.id != batch + 2 ||
            (reply.message.status != STATUS_OK && reply.message.status != STATUS_NOT_FOUND))
            sys::exit(STATUS_BAD_ARGS);
        if (const auto rc = sys::notification_signal(peer.selector()).status; rc != STATUS_OK)
            sys::exit(rc); // release the control reply's queue credit
        size_t remaining = io_queue_depth;
        while (remaining != 0) {
            io_done completion;
            for (;;) {
                const auto result = queue.take(completion);
                if (result == libk::ring_status::empty)
                    break;
                if (result != libk::ring_status::ready)
                    sys::exit(STATUS_PEER_FAULT);
                size_t slot{};
                while (slot < io_queue_depth && expected[slot] != completion.id)
                    ++slot;
                if (slot == io_queue_depth)
                    sys::exit(STATUS_INTERNAL);
                const bool cancellation =
                    completion.id == cancelled && reply.message.status == STATUS_OK;
                const auto status = cancellation ? STATUS_CANCELED : expected_status[slot];
                if (completion.status != status ||
                    completion.bytes != (status == STATUS_OK ? io_buffer_size : 0))
                    sys::exit(STATUS_INTERNAL);
                if (status == STATUS_OK) {
                    for (size_t index = 0; index < io_buffer_size; ++index)
                        if (memory.payload()[slot * io_buffer_size + index] != 0)
                            sys::exit(STATUS_BACKING_FAILED);
                }
                expected[slot] = 0;
                --remaining;
            }
            if (queue.release())
                if (const auto rc = sys::notification_signal(peer.selector()).status;
                    rc != STATUS_OK)
                    sys::exit(rc);
            if (remaining != 0)
                if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
                    sys::exit(rc);
        }
    }
    for (size_t slot = 0; slot < io_queue_depth; ++slot) {
        io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                       .offset = slot * io_buffer_size,
                       .buffer_offset = slot * io_buffer_size,
                       .length = io_buffer_size};
        if (queue.submit(request) != libk::ring_status::ready)
            sys::exit(STATUS_INTERNAL);
    }
    if (!queue.publish())
        sys::exit(STATUS_INTERNAL);
    if (const auto rc = sys::notification_signal(peer.selector()).status; rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc =
            control.send({.operation = static_cast<uint64_t>(io_control::Close), .id = 18}).status;
        rc != STATUS_OK)
        sys::exit(rc);
    const auto closed = ch_recv();
    if (closed.count != 0 || closed.message.id != 18)
        sys::exit(STATUS_BAD_ARGS);
    if (closed.message.status != STATUS_OK) sys::exit(closed.message.status);
    const auto coordinator = info.selector(rt_stdout);
    if (const auto rc = ch_send(coordinator, {.id=100}).status; rc != STATUS_OK) sys::exit(rc);
    ch_msg stopped;
    if (const auto rc = ch_recv(coordinator, stopped).status; rc != STATUS_OK) sys::exit(rc);
    if (stopped.id != 101) sys::exit(STATUS_PEER_FAULT);
    for (auto& session : sessions) {
        // Volatile loads cover the whole payload; queue.take also reads the CQ.
        const volatile uint8_t* bytes = session.payload();
        for (size_t i = 0; i < io_payload_size; ++i)
            if (bytes[i]) sys::exit(STATUS_BACKING_FAILED);
        io_done done;
        if (session.queue().take(done) != libk::ring_status::empty) sys::exit(STATUS_INTERNAL);
        ch_msg request{.operation=static_cast<uint64_t>(io_control::Stat)};
        if (session.exchange(request) != STATUS_PEER_CLOSED) sys::exit(STATUS_INTERNAL);
        if (session.close() != STATUS_PEER_CLOSED) sys::exit(STATUS_INTERNAL);
    }
    sys::exit();
}
