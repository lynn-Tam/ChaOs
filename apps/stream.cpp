#include <virtio.hpp>
#include <ubus/stream.hpp>
#include <deque>

static vi_dev device;
static vi_queue receiveq, transmitq, control_rx, control_tx;
static vi_dma storage;
static constexpr size_t slots = 16, block = ubus_stream_limit;
static std::array<bool, slots> transmitting;
struct st_control { uint32_t port; uint16_t event, value; };
static_assert(sizeof(st_control) == 8);
static constexpr size_t control_at = slots * block * 2, control_out = control_at + slots * 256;
static std::array<bool, slots> controlling;
static std::deque<st_control> controls;
static std::deque<vi_queue::completion> received;
static _ch_pair incoming, outgoing;
static _cap_owner sink;
static ubus_rx input;
static ubus_tx output;
static _ev_watch writable;
static bool connected, online;
static uint64_t generation;
static uint32_t port{UINT32_MAX}, ports;

static auto replenish(uint64_t n) -> status_t {
    const vi_queue::part part{storage.address + n * block, block, true};
    return receiveq.submit(n, {&part, 1});
}
static void close() {
    (void)output.cancel(); (void)writable.close(); input = {}; sink = {}; incoming = {}; outgoing = {}; connected = false;
}
int main() {
    ubus_conn parent;
    auto status = parent.bind(_startup().bootstrap); if (status != STATUS_OK) return status;
    std::vector<ubus_msg> setup;
    do {
        ubus_msg reply; status = parent.invoke("Receive", {}, reply); if (status != STATUS_OK) return status;
        ubus_data data; if (!data.assign(reply.data["message"].bytes())) return STATUS_BAD_ARGS;
        reply.data = std::move(data);
        setup.push_back(std::move(reply));
    } while (setup.back().data["more"].as<bool>().value_or(false));
    status = device.open(setup, 0x1043, 1 << 1); if (status != STATUS_OK) return status;
    if (!(device.features & (1 << 1)) || device.config_size < 8) return STATUS_BACKING_FAILED;
    ports = vi_read<uint32_t>(device.config + 4);
    if (!ports || ports > 32767) return STATUS_BACKING_FAILED;
    if ((status = control_rx.open(device, 2, slots)) != STATUS_OK || (status = control_tx.open(device, 3, slots)) != STATUS_OK) return status;
    auto dma = device.allocate(control_out + slots * sizeof(st_control)); if (!dma) return dma.error(); storage = std::move(*dma);
    for (size_t n = 0; n < control_rx.size(); ++n) {
        const vi_queue::part part{storage.address + control_at + n * 256, 256, true};
        if ((status = control_rx.submit(n, {&part, 1})) != STATUS_OK) return status;
    }
    if ((status = device.ready()) != STATUS_OK) return status;
    control_rx.publish(); controls.push_back({0, 0, 1});
    ubus_object object{"io.stream"};
    status = object.add("Open", {}, {{"read", ubus_type::Cap}, {"write", ubus_type::Cap}, {"generation", ubus_type::U64}}, [](auto&, auto& reply) -> status_t {
        if (connected) return STATUS_BUSY;
        if (!online) return STATUS_PEER_CLOSED;
        // Payload memory, optional bulk metadata, and the native Reply.
        auto status = incoming.open(_startup().alloc(), 32, 3);
        if (status == STATUS_OK) status = outgoing.open(_startup().alloc(), 32, 3);
        if (status == STATUS_OK) status = input.open(incoming.ends[1].selector(), &_events());
        if (status == STATUS_OK) status = writable.open(&_events(), outgoing.ends[0].selector(), SOURCE_READY | SOURCE_TERMINAL, 0, WATCH_CHANGED, SOURCE_WRITE);
        if (status != STATUS_OK) { close(); return status; }
        const auto producer = _channel_mint(outgoing.ends[0].selector(), _startup().cspace, 1, RIGHT_SEND);
        if (producer.status != STATUS_OK) { close(); return producer.status; }
        sink = _cap_owner{producer.value};
        const auto writer = _channel_mint(incoming.ends[0].selector(), _startup().cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
        if (writer.status != STATUS_OK) { close(); return writer.status; }
        const auto read = reply.offer(std::move(outgoing.ends[1]), RIGHT_RECEIVE | RIGHT_DUPLICATE);
        const auto write = reply.give(_cap_owner{writer.value});
        if (!read || !write) { close(); return !read ? read.error() : write.error(); }
        (void)incoming.ends[0].close();
        reply.data.cap("read", *read).cap("write", *write).put("generation", ++generation);
        connected = true; return STATUS_OK;
    });
    if (status == STATUS_OK) status = object.add("Close", {}, {}, [](auto&, auto&) { close(); return STATUS_OK; });
    if (status == STATUS_OK) status = object.open();
    if (status != STATUS_OK) return status;
    auto connector = object.connector(); if (!connector) return connector.error();
    ubus_msg announce, reply;
    const auto index = announce.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE); if (!index) return index.error();
    announce.data.put("name", "display").put("interface", "io.stream").put("version", uint64_t{1}).cap("connector", *index);
    status = parent.invoke("Publish", std::move(announce), reply); if (status != STATUS_OK) return status;

    ubus_frame frame;
    for (;;) {
        bool again{};
        if ((status = device.acknowledge()) != STATUS_OK) return status;
        for (size_t n = 0; n < slots; ++n) {
            vi_queue::completion done; status = control_tx.take(done); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK || done.cookie >= slots || !controlling[done.cookie] || done.size) return STATUS_BACKING_FAILED;
            controlling[done.cookie] = false; again = true;
        }
        for (size_t n = 0; n < slots; ++n) {
            vi_queue::completion done; status = control_rx.take(done); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK || done.cookie >= slots || done.size < sizeof(st_control) || done.size > 256) return STATUS_BACKING_FAILED;
            st_control value; ::memcpy(&value, reinterpret_cast<const void*>(storage.memory.address + control_at + done.cookie * 256), sizeof(value));
            if (value.event == 1 && port == UINT32_MAX) {
                if (value.port >= ports) return STATUS_BACKING_FAILED;
                const auto index = uint16_t(value.port ? 2 * value.port + 2 : 0);
                if ((status = receiveq.open(device, index, slots)) != STATUS_OK || (status = transmitq.open(device, index + 1, slots)) != STATUS_OK) return status;
                if (receiveq.size() != slots || transmitq.size() != slots) return STATUS_BACKING_FAILED;
                for (size_t i = 0; i < slots; ++i) if ((status = replenish(i)) != STATUS_OK) return status;
                receiveq.publish(); port = value.port;
                controls.push_back({port, 3, 1}); controls.push_back({port, 6, 1});
            } else if (value.port == port) {
                if (value.event == 6) { online = value.value != 0; if (!online) close(); }
                else if (value.event == 2) return STATUS_PEER_CLOSED;
            }
            if (controls.size() > slots) return STATUS_EXHAUSTED;
            const vi_queue::part part{storage.address + control_at + done.cookie * 256, 256, true};
            if ((status = control_rx.submit(done.cookie, {&part, 1})) != STATUS_OK) return status;
            again = true;
        }
        control_rx.publish();
        while (!controls.empty()) {
            const auto free = std::ranges::find(controlling, false); if (free == controlling.end()) break;
            const auto n = size_t(free - controlling.begin());
            ::memcpy(reinterpret_cast<void*>(storage.memory.address + control_out + n * sizeof(st_control)), &controls.front(), sizeof(st_control));
            const vi_queue::part part{storage.address + control_out + n * sizeof(st_control), sizeof(st_control), false};
            if ((status = control_tx.submit(n, {&part, 1})) != STATUS_OK) return status;
            *free = true; controls.pop_front(); again = true;
        }
        control_tx.publish();
        for (size_t n = 0; port != UINT32_MAX && n < slots; ++n) {
            vi_queue::completion done; status = transmitq.take(done); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK || done.cookie >= slots || !transmitting[done.cookie] || done.size) return STATUS_BACKING_FAILED;
            transmitting[done.cookie] = false; again = true;
        }
        for (size_t n = 0; port != UINT32_MAX && n < slots; ++n) {
            vi_queue::completion done; status = receiveq.take(done); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK || done.cookie >= slots || done.size > block) return STATUS_BACKING_FAILED;
            received.push_back(done); again = true;
        }
        while (!received.empty()) {
            const auto done = received.front();
            if (connected && done.size) {
                frame = {}; frame.size = done.size;
                ::memcpy(frame.data, reinterpret_cast<const void*>(storage.memory.address + done.cookie * block), done.size);
                status = output.send(sink.selector(), frame, false);
                if (status == STATUS_WOULD_BLOCK) break;
                if (status != STATUS_OK) close();
            }
            if ((status = replenish(done.cookie)) != STATUS_OK) return status;
            received.pop_front(); again = true;
        }
        if (port != UINT32_MAX) receiveq.publish();
        if (connected) {
            for (size_t n = 0; n < slots; ++n) {
                const auto free = std::ranges::find(transmitting, false); if (free == transmitting.end()) break;
                status = ubus_recv(input, frame); if (status == STATUS_WOULD_BLOCK) break;
                if (status != STATUS_OK || frame.status != STATUS_OK) { close(); break; }
                if (!frame.size) continue;
                const auto slot = size_t(free - transmitting.begin());
                ::memcpy(reinterpret_cast<void*>(storage.memory.address + (slots + slot) * block), frame.data, frame.size);
                const vi_queue::part part{storage.address + (slots + slot) * block, uint32_t(frame.size), false};
                if ((status = transmitq.submit(slot, {&part, 1})) != STATUS_OK) return status;
                *free = true; again = true;
            }
            const auto fact = _source_info(outgoing.ends[0].selector(), SOURCE_WRITE);
            if (fact.status != STATUS_OK || (fact.value & SOURCE_TERMINAL)) close();
            else if ((status = writable.arm(fact.value2).status) != STATUS_OK) return status;
        }
        if (port != UINT32_MAX) transmitq.publish();
        for (size_t n = 0; n < 8; ++n) { status = object.poll(); if (status == STATUS_WOULD_BLOCK) break; if (status != STATUS_OK) return status; again = true; }
        if (!again && (status = _ev_wait(&_events()).status) != STATUS_OK) return status;
    }
}
