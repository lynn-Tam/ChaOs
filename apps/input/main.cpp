#include <virtio.hpp>
#include <gui/input.hpp>
#include <deque>

static vi_dev dev;
static vi_queue eventq, statusq;
static vi_dma storage;
static ubus_conn parent;
static ubus_object directory{"io.input"};
static std::unique_ptr<ubus_object> client;
static _cap_owner events;
static _ev_watch event_watch;
static std::deque<in_batch> pending;
static std::array<bool, 32> feedback{};
static in_batch batch;
static uint64_t sequence, losses;
static ubus_data capabilities;

static void reset() {
    pending.clear(); batch = {}; batch.sequence = ++sequence; batch.time = _clock_now().value; batch.reset = 1;
    pending.push_back(batch); batch = {}; ++losses;
}
static void receive(in_event event) {
    if (!events) return;
    if (event.type == 0 && event.code == 3) { reset(); return; }
    if (!batch.sequence) { batch.sequence = ++sequence; batch.time = _clock_now().value; }
    batch.events[batch.count++] = event;
    if (batch.count == std::size(batch.events) || (!event.type && !event.code)) {
        if (pending.size() == 64) { reset(); return; }
        pending.push_back(batch);
        const auto seq = batch.sequence, time = batch.time;
        batch = {};
        if (event.type || event.code) { batch.sequence = seq; batch.time = time; }
    }
}
static auto config(uint8_t select, uint8_t sub) -> std::span<const std::byte> {
    static std::array<std::byte, 128> data;
    vi_write<uint8_t>(dev.config, select); vi_write<uint8_t>(dev.config + 1, sub);
    const auto size = vi_read<uint8_t>(dev.config + 2);
    if (size > data.size()) return {};
    for (size_t i = 0; i < size; ++i) data[i] = std::byte{vi_read<uint8_t>(dev.config + 8 + i)};
    return std::span{data}.first(size);
}
static auto open(ubus_request& q, ubus_msg& r) -> status_t {
    if (client) return STATUS_BUSY;
    auto endpoint = q.take(q.data["events"]);
    auto fact = _source_info(endpoint.selector(), SOURCE_WRITE);
    if (fact.status != STATUS_OK || (fact.value & SOURCE_TERMINAL)) return STATUS_BAD_ARGS;
    auto object = std::make_unique<ubus_object>("io.input.session");
    auto s = object->add("Feedback", {{"type", ubus_type::U64}, {"code", ubus_type::U64}, {"value", ubus_type::I64}}, {}, [](auto& q, auto&) {
        const auto type = *q.data["type"].template as<uint64_t>(), code = *q.data["code"].template as<uint64_t>();
        const auto value = *q.data["value"].template as<int64_t>();
        if (type != 17 || code > 15 || (value != 0 && value != 1)) return STATUS_BAD_ARGS;
        auto bits = config(0x11, type);
        if (code / 8 >= bits.size() || !(uint8_t(bits[code / 8]) & (1U << (code % 8)))) return STATUS_INVALID_OP;
        const auto it = std::ranges::find(feedback, false); if (it == feedback.end()) return STATUS_BUSY;
        const auto n = size_t(it - feedback.begin());
        const in_event event{uint16_t(type), uint16_t(code), int32_t(value)};
        ::memcpy(reinterpret_cast<void*>(storage.memory.address + 4096 + n * 8), &event, sizeof(event));
        const vi_queue::part part{storage.address + 4096 + n * 8, sizeof(event), false};
        const auto status = statusq.submit(n, {&part, 1});
        if (status == STATUS_OK) { feedback[n] = true; statusq.publish(); }
        return status;
    });
    if (s == STATUS_OK) s = object->add("Close", {}, {}, [](auto&, auto&) { (void)event_watch.close(); events = {}; pending.clear(); batch = {}; return STATUS_OK; });
    if (s == STATUS_OK) s = object->open(8);
    if (s != STATUS_OK) return s;
    auto handle = object->handle(); if (!handle) return handle.error();
    auto idx = r.give(std::move(*handle)); if (!idx) return idx.error();
    s = event_watch.open(&_events(), endpoint.selector(), SOURCE_TERMINAL, 0, WATCH_CHANGED, SOURCE_WRITE);
    if (s != STATUS_OK) return s;
    r.data.cap("handle", *idx).put("capabilities", capabilities).put("generation", uint64_t{1});
    events = std::move(endpoint); client = std::move(object); reset(); return STATUS_OK;
}
int main() {
    auto s = parent.bind(_startup().bootstrap); if (s != STATUS_OK) return s;
    std::vector<ubus_msg> setup;
    do {
        ubus_msg r; s = parent.invoke("Receive", {}, r); if (s != STATUS_OK) return s;
        ubus_data data; if (!data.assign(r.data["message"].bytes())) return STATUS_BAD_ARGS;
        r.data = std::move(data); setup.push_back(std::move(r));
    } while (setup.back().data["more"].as<bool>().value_or(false));
    s = dev.open(setup, 0x1052); if (s != STATUS_OK) return s;
    if (dev.config_size < 136) return STATUS_BACKING_FAILED;
    capabilities.put("name", config(1, 0));
    ubus_data types{ubus_type::Array};
    for (unsigned type = 0; type < 32; ++type) {
        const auto bits = config(0x11, type); if (bits.empty()) continue;
        ubus_data row; row.put("type", uint64_t{type}).put("bits", bits); types.push(row);
    }
    capabilities.put("events", types);
    ubus_data axes{ubus_type::Array};
    for (unsigned axis = 0; axis < 64; ++axis) {
        const auto bits = config(0x12, axis); if (bits.size() != 20) continue;
        std::array<int32_t, 5> values; ::memcpy(values.data(), bits.data(), bits.size());
        ubus_data row; row.put("axis", uint64_t{axis}).put("min", int64_t{values[0]}).put("max", int64_t{values[1]}); axes.push(row);
    }
    capabilities.put("axes", axes);
    if ((s = eventq.open(dev, 0, 256)) != STATUS_OK || (s = statusq.open(dev, 1, 32)) != STATUS_OK) return s;
    auto dma = dev.allocate(8192); if (!dma) return dma.error(); storage = std::move(*dma);
    for (unsigned n = 0; n < eventq.size(); ++n) {
        const vi_queue::part part{storage.address + n * 8, 8, true};
        if ((s = eventq.submit(n, {&part, 1})) != STATUS_OK) return s;
    }
    if ((s = dev.ready()) != STATUS_OK) return s;
    eventq.publish();
    s = directory.add("Open", {{"events", ubus_type::Cap}}, {{"handle", ubus_type::Cap}, {"capabilities", ubus_type::Record}, {"generation", ubus_type::U64}}, open);
    if (s == STATUS_OK) s = directory.open();
    if (s != STATUS_OK) return s;
    auto connector = directory.connector(); if (!connector) return connector.error();
    ubus_msg pub, r; auto idx = pub.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE); if (!idx) return idx.error();
    pub.data.put("name", "input").put("interface", "io.input").put("version", uint64_t{1}).cap("connector", *idx);
    s = parent.invoke("Publish", std::move(pub), r); if (s != STATUS_OK) return s;
    for (;;) {
        bool again{};
        if ((s = dev.acknowledge()) != STATUS_OK) return s;
        // Replenish before servicing clients. Reports remain ordered in pending.
        size_t received{};
        for (; received < eventq.size(); ++received) {
            vi_queue::completion c; s = eventq.take(c); if (s == STATUS_WOULD_BLOCK) break;
            if (s != STATUS_OK || c.size != sizeof(in_event) || c.cookie >= eventq.size()) return STATUS_BACKING_FAILED;
            in_event event; ::memcpy(&event, reinterpret_cast<const void*>(storage.memory.address + c.cookie * 8), sizeof(event));
            const vi_queue::part part{storage.address + c.cookie * 8, 8, true};
            if ((s = eventq.submit(c.cookie, {&part, 1})) != STATUS_OK) return s;
            receive(event); again = true;
        }
        eventq.publish(); if (received == eventq.size()) reset();
        for (size_t n = 0; n < feedback.size(); ++n) {
            vi_queue::completion c; s = statusq.take(c); if (s == STATUS_WOULD_BLOCK) break;
            if (s != STATUS_OK || c.cookie >= feedback.size() || !feedback[c.cookie] || c.size) return STATUS_BACKING_FAILED;
            feedback[c.cookie] = false; again = true;
        }
        if (events) {
            auto fact = _source_info(events.selector(), SOURCE_WRITE);
            if (fact.status != STATUS_OK || (fact.value & SOURCE_TERMINAL)) { (void)event_watch.close(); events = {}; }
            else {
                while (!pending.empty()) {
                    ubus_msg m; m.data.put("event", std::as_bytes(std::span{&pending.front(), 1}));
                    s = ubus_send(events.selector(), m, false); if (s == STATUS_WOULD_BLOCK) break;
                    if (s != STATUS_OK) { (void)event_watch.close(); events = {}; break; }
                    pending.pop_front(); again = true;
                }
                if (events && (s = event_watch.arm(fact.value2).status) != STATUS_OK) return s;
            }
        }
        if (client && !events) { client.reset(); pending.clear(); batch = {}; }
        for (size_t n = 0; n < 8; ++n) { s = directory.poll(); if (s == STATUS_WOULD_BLOCK) break; if (s != STATUS_OK) return s; again = true; }
        if (client) for (size_t n = 0; n < 8; ++n) { s = client->poll(); if (s == STATUS_WOULD_BLOCK) break; if (s != STATUS_OK) { (void)event_watch.close(); events = {}; break; } again = true; }
        if (!again && (s = _ev_wait(&_events()).status) != STATUS_OK) return s;
    }
}
