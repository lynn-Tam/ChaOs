#include <gui/gui.hpp>
#include <gui/display.hpp>
#include <gui/server.hpp>
#include <gui/host.hpp>
#include <gui/input.hpp>
#include <thread.hpp>
#include <map>
#include <bitset>
#include <atomic>
#include <font8x8_basic.h>

class gui_server {
    struct gui_session;
    struct gui_pixels {
        _vm_area area;
        uint64_t window{}, commit{};
        uint32_t width{}, height{}, format{};
        bool busy{}, frame{};
    };
    struct gui_window {
        gui_session* owner{};
        uint64_t id{}, serial{1};
        std::string title;
        gui_rect rect, restore;
        uint32_t flags{};
        gui_limits limits;
        uint32_t current{}, pending{};
        gui_rect damage;
        uint64_t host_offset{}, host_bytes{}, host_flight{}, host_submitted{};
        bool host_dirty{true}, host_activate{}, host_published{}, host_repaint{true};
    };
    struct gui_session {
        uint64_t id{}, bytes{}, limit{}, last_commit{}, blocked{};
        uint32_t next_buffer{};
        _cap_owner scope, budget, events;
        _ev_watch ready;
        ubus_object client{"gui.session", 1, gui_scaled_buffers}, control{"gui.control"};
        std::map<uint64_t, std::unique_ptr<gui_window>> windows;
        std::map<uint32_t, gui_pixels> buffers;
        std::vector<gui_event> queue;
        size_t sent{}, reserved{};
        bool active{true}, activate{true}, closing{}, stopped{}, connected{};
        gui_session() { queue.reserve(256); }
    };
    std::vector<std::unique_ptr<gui_session>> sessions{};
    std::vector<gui_window*> windows{};
    gui_window *focus{}, *grab{}, *drag{};
    uint64_t next_session{}, next_window{}, epoch{1}, event_sequence{};
    std::bitset<768> held{}, delivered{};
    int32_t pointer_x{}, pointer_y{}, drag_x{}, drag_y{};
    uint32_t buttons{};
    bool resizing{};
    uint64_t repeat_due{};
    uint16_t repeat_key{};
    uint64_t input_time{};
    bool caps_lock{};
    uint32_t output_width{800}, output_height{600};
    uint64_t interval{}, next_frame{};
    size_t max_sessions{8}, max_windows{4};
    uint64_t session_bytes{16 << 20};
    _cap_desc descriptor{};
    _cap_owner shared{};
    bool hosted{};
    gui_host host;
    status_t host_error{STATUS_PENDING};
    uint64_t host_epoch{}, host_input_sequence{};
    struct gui_retired { uint64_t id, offset, bytes, sequence; bool sent{}; };
    std::vector<gui_retired> retired;

    auto bounds(const gui_window& w) -> gui_rect {
        return w.flags & gui_fullscreen ? w.rect : gui_rect{w.rect.x - 2, w.rect.y - 22, w.rect.width + 4, w.rect.height + 24};
    }
    auto visible(const gui_window& w) -> bool { return w.owner->active && !w.owner->closing && !w.owner->stopped; }
    auto event(gui_window* w, uint32_t type) -> gui_event {
        return {.window=w ? w->id : 0, .serial=epoch, .sequence=++event_sequence, .time=_clock_now().value, .type=type};
    }
    void enqueue(gui_session& s, gui_event e, bool obligation = false) {
        if (s.closing || s.stopped || !s.events) return;
        if (obligation) { if (!s.reserved) _thread_exit(STATUS_INTERNAL); --s.reserved; }
        if (s.sent && (s.sent == s.queue.size() || s.queue.size() == s.queue.capacity())) {
            s.queue.erase(s.queue.begin(), s.queue.begin() + s.sent); s.sent = 0;
        }
        // Defer teardown to the event loop: the caller may still hold a window
        // reference or be traversing the scene when its event cannot be queued.
        if (s.queue.size() + s.reserved >= s.queue.capacity()) { s.closing = true; return; }
        s.queue.push_back(e);
    }
    void input_event(gui_window* w, gui_event e) {
        if (!w || !visible(*w) || w->owner->stopped) return;
        auto& s = *w->owner;
        if (input_time) e.time = input_time;
        if (s.queue.size() - s.sent >= 64) {
            // Only unsent input can be discarded. Control obligations keep order.
            std::erase_if(s.queue, [&](const gui_event& x) {
                const auto i = size_t(&x - s.queue.data());
                return i >= s.sent && (x.type == gui_key || x.type == gui_pointer || x.type == gui_text);
            });
            reset_seat(); return;
        }
        enqueue(s, e);
    }
    void release(gui_window& w, uint32_t id, uint32_t result = 0) {
        if (!id) return;
        auto& b = w.owner->buffers.find(id)->second;
        if (!b.busy) _thread_exit(STATUS_INTERNAL);
        if (b.frame) {
            auto e = event(&w, gui_frame); e.serial = b.commit; e.value = result ? result : gui_discarded;
            enqueue(*w.owner, e, true); b.frame = false;
        }
        auto e = event(&w, gui_release); e.serial = b.commit; e.code = id;
        enqueue(*w.owner, e, true); b.busy = false;
    }
    void remove(gui_window& w) {
        if (hosted && host.ready() && w.host_published)
            retired.push_back({w.id, w.host_offset, w.host_bytes, ++present_sequence});
        damaged(bounds(w));
        if (grab == &w) { input_event(&w, event(&w, gui_reset)); grab = nullptr; buttons = 0; }
        if (drag == &w) drag = nullptr;
        if (focus == &w) set_focus(nullptr, false);
        release(w, w.pending); w.pending = 0; release(w, w.current); w.current = 0;
        std::erase(windows, &w);
        for (auto i = w.owner->buffers.begin(); i != w.owner->buffers.end();) {
            if (i->second.window == w.id) { w.owner->bytes -= i->second.area.size;
                const auto status = _object_destroy(i->second.area.memory.selector()).status;
                if (status != STATUS_OK) _thread_exit(status);
                (void)i->second.area.close(); i = w.owner->buffers.erase(i); }
            else ++i;
        }
        if (!hosted && !focus) for (auto i = windows.rbegin(); i != windows.rend(); ++i) if (visible(**i)) { set_focus(*i); break; }
    }
    void stop(gui_session& s) {
        if (s.stopped) return;
        s.stopped = true;
        for (auto& [id, w] : s.windows) remove(*w);
        s.windows.clear(); s.buffers.clear();
        (void)s.ready.close(); s.events = {};
        if (s.scope) { (void)_scope_close(s.scope.selector()); (void)_wait_one(s.scope.selector(), STATE_DRAINED); s.scope = {}; }
    }
    void set_focus(gui_window* w, bool request = true) {
        if (hosted && request) {
            if (w) { w->host_activate = true; w->host_dirty = true; }
            return;
        }
        if (focus == w) return;
        ++epoch; delivered.reset(); repeat_due = 0;
        if (focus) { damaged(bounds(*focus)); auto e = event(focus, gui_focus); e.value = 0; enqueue(*focus->owner, e); }
        if (grab) { enqueue(*grab->owner, event(grab, gui_reset)); grab = nullptr; buttons = 0; }
        focus = w;
        if (focus) {
            damaged(bounds(*focus)); auto e = event(focus, gui_focus); e.value = 1; enqueue(*focus->owner, e);
            std::erase(windows, focus); windows.push_back(focus);
        }
    }
    void reset_seat() {
        ++epoch; delivered.reset(); repeat_due = 0;
        if (focus) enqueue(*focus->owner, event(focus, gui_reset));
        if (grab && grab != focus) enqueue(*grab->owner, event(grab, gui_reset));
        grab = drag = nullptr; buttons = 0;
    }
    void configure(gui_window& w, int32_t width, int32_t height, bool full, bool request = true) {
        if (w.serial == UINT64_MAX) { w.owner->closing = true; return; }
        damaged(bounds(w));
        const bool changed = full != bool(w.flags & gui_fullscreen);
        if (!hosted && full && changed) w.restore = w.rect;
        if (!hosted && !full && changed) w.rect = w.restore;
        if (hosted && request && changed) { width = w.rect.width; height = w.rect.height; }
        w.flags = full ? w.flags | gui_fullscreen : w.flags & ~gui_fullscreen;
        if (full && !hosted) w.rect = {0, 0, int32_t(output_width), int32_t(output_height)};
        else {
            const auto max_w = hosted ? w.limits.max_width : std::min(w.limits.max_width, output_width);
            const auto max_h = hosted ? w.limits.max_height : std::min(w.limits.max_height, output_height - 24);
            w.rect.width = std::clamp(width, int32_t(std::min(w.limits.min_width, max_w)), int32_t(max_w));
            w.rect.height = std::clamp(height, int32_t(std::min(w.limits.min_height, max_h)), int32_t(max_h));
        }
        ++w.serial;
        w.host_dirty = true;
        auto e = event(&w, gui_configure); e.serial = w.serial; e.flags = w.flags; e.width = w.rect.width; e.height = w.rect.height;
        enqueue(*w.owner, e); damaged(bounds(w));
    }

    // A connection rebuilds from authority after loss; its integers never carry
    // over. All RPC progress is nonblocking in the compositor loop.
    struct gui_link {
        gui_server& server;
        explicit gui_link(gui_server& owner) : server(owner) {}
        std::string id;
        bool display{}, motion{};
        ubus_conn bus, directory, peer;
        _cap_owner connector;
        _ch_pair channel;
        ubus_rx input;
        unsigned phase{};
        uint64_t retry{}, generation{}, sequence{};
        status_t error{STATUS_PENDING};
        std::bitset<768> pressed;
        std::array<int32_t, 2> minimum{}, maximum{32767, 32767};
        auto poll() -> status_t {
            ubus_msg q, r; status_t s = STATUS_OK;
            if (phase == 6) return STATUS_OK;
            if (!phase) {
                if (_clock_now().value < retry) return STATUS_WOULD_BLOCK;
                s = bus.begin_bind(_startup().bootstrap); if (s != STATUS_OK) return s;
                phase = 1;
            }
            if (phase == 1) {
                s = bus.poll_bind(); if (s != STATUS_OK) return s;
                q.data.put("name", display ? "display" : "hardware");
                s = bus.begin("Get", std::move(q)); if (s != STATUS_OK) return s; phase = 2;
            }
            if (phase == 2) {
                s = bus.poll(r); if (s != STATUS_OK) return s; if (r.status != STATUS_OK) return r.status;
                connector = r.take(r.data["connector"]);
                s = directory.begin_bind(connector.selector(), display ? "io.display" : "io.devices", 1, 1); if (s != STATUS_OK) return s; phase = 3;
            }
            if (phase == 3) {
                s = directory.poll_bind(); if (s != STATUS_OK) return s;
                if (!display) { q.data.put("id", id); s = directory.begin("Open", std::move(q)); phase = 7; }
                else phase = 4;
                if (s != STATUS_OK) return s;
            }
            if (phase == 7) {
                s = directory.poll(r); if (s != STATUS_OK) return s; if (r.status != STATUS_OK) return r.status;
                connector = r.take(r.data["connector"]); (void)directory.close();
                s = directory.begin_bind(connector.selector(), "io.input", 1, 1); if (s != STATUS_OK) return s; phase = 8;
            }
            if (phase == 8) { s = directory.poll_bind(); if (s != STATUS_OK) return s; phase = 4; }
            if (phase == 4) {
                s = channel.open(_startup().alloc(), 64, 0);
                if (s == STATUS_OK) s = input.open(channel.ends[0].selector(), &_events());
                if (s != STATUS_OK) return s;
                const auto sender = _channel_mint(channel.ends[1].selector(), _startup().cspace, 1, RIGHT_SEND);
                if (sender.status != STATUS_OK) return sender.status;
                auto idx = q.give(_cap_owner{sender.value}); if (!idx) return idx.error();
                q.data.cap("events", *idx); s = directory.begin("Open", std::move(q)); if (s != STATUS_OK) return s; phase = 5;
            }
            if (phase == 5) {
                s = directory.poll(r); if (s != STATUS_OK) return s; if (r.status != STATUS_OK) return r.status;
                generation = r.data["generation"].as<uint64_t>().value_or(0);
                auto handle = r.take(r.data["handle"]);
                s = peer.begin_adopt(std::move(handle), display ? "io.display.session" : "io.input.session", 1, 1);
                if (s != STATUS_OK) return s;
                s = server.link_opened(*this, std::move(r)); if (s != STATUS_OK) return s; phase = 9;
            }
            if (phase == 9) { s = peer.poll_bind(); if (s != STATUS_OK) return s; (void)channel.ends[1].close(); phase = 6; }
            return phase == 6 ? STATUS_OK : STATUS_WOULD_BLOCK;
        }
    };
    struct gui_output { _vm_area pixels; gui_rect damage; bool busy{}; uint64_t sequence{}, submitted{}; std::vector<gui_event> frames; };
    std::array<gui_output, 2> outputs{};
    gui_link display{*this};
    std::vector<std::unique_ptr<gui_link>> inputs{};
    void link_lost(gui_link& link) {
            (void)link.input.close(); (void)link.channel.close(); (void)link.peer.close(); (void)link.directory.close(); (void)link.bus.close(); link.connector = {};
            link.pressed.reset();
            if (!link.display) { held.reset(); for (const auto& i : inputs) held |= i->pressed; }
            link.phase = 0; link.retry = _clock_now().value + _startup().frequency / 4;
        }
    uint64_t present_sequence{};
    bool present_call{}, cursor_call{}, cursor_dirty{}, frame_pending{}, scene_dirty{};
    uint64_t composed_pixels{}, copied_bytes{}, frames_done{}, present_ticks{}, compose_ticks{}, compose_count{}, compose_max{};
    std::array<uint64_t, 8> compose_histogram{};
    uint64_t client_calls{}, events_sent{}, input_batches{}, present_calls{}, cursor_calls{};
    void damaged(gui_rect rect) {
        if (hosted) return;
        rect = gui_intersect(rect, {0, 0, int32_t(output_width), int32_t(output_height)});
        if (rect.empty()) return;
        scene_dirty = true;
        for (auto& b : outputs) gui_damage(b.damage, rect);
    }
    auto link_opened(gui_link& link, ubus_msg r) -> status_t {
        link.error = STATUS_OK;
        if (!link.display) {
            for (auto axis : r.data["capabilities"]["axes"].values()) {
                const auto n = axis["axis"].as<uint32_t>();
                const auto min = axis["min"].as<int32_t>(), max = axis["max"].as<int32_t>();
                if (n && *n < 2 && min && max && *max > *min) { link.minimum[*n] = *min; link.maximum[*n] = *max; }
            }
            link.sequence = 0; link.pressed.reset(); return STATUS_OK;
        }
        const auto w = r.data["width"].as<uint32_t>(), h = r.data["height"].as<uint32_t>(), stride = r.data["stride"].as<uint32_t>();
        if (!w || !h || !stride || *w < 64 || *h < 96 || !gui_bytes(*w, *h) || *stride != *w * 4) return STATUS_PEER_FAULT;
        output_width = *w; output_height = *h;
        std::bitset<std::tuple_size_v<decltype(outputs)>> mapped;
        for (auto row : r.data["buffers"].values()) {
            const auto id = row["id"].as<uint32_t>(); const auto slice = row["pixels"].as<ubus_buffer>();
            if (!id || *id >= outputs.size() || mapped[*id] || !slice || slice->offset || slice->size != *gui_bytes(*w, *h)) return STATUS_PEER_FAULT;
            auto p = _vm_area::map(_vm(), r.take(row["pixels"]), 0, slice->size, VM_READ | VM_WRITE);
            if (!p) return p.error();
            outputs[*id].pixels = std::move(*p); outputs[*id].busy = false; outputs[*id].frames.reserve(max_sessions * max_windows); mapped[*id] = true;
        }
        if (!mapped.all()) return STATUS_PEER_FAULT;
        damaged({0, 0, int32_t(*w), int32_t(*h)});
        for (auto* window : windows) if (window->flags & gui_fullscreen) configure(*window, *w, *h, true);
        cursor_dirty = true; present_sequence = 0; return STATUS_OK;
    }
    void display_lost() {
        for (auto& o : outputs) {
            for (auto e : o.frames) for (auto& s : sessions) if (s->id == e.sequence) { e.sequence = ++event_sequence; e.value = gui_failed; enqueue(*s, e, true); }
            o.frames.clear(); (void)o.pixels.close(); o.busy = false;
        }
        link_lost(display); present_call = cursor_call = frame_pending = false;
        for (auto& s : sessions) enqueue(*s, event(nullptr, gui_display));
    }

    auto session_open(ubus_request& q, ubus_msg& r) -> status_t {
        if (sessions.size() >= max_sessions || next_session == UINT64_MAX) return STATUS_NO_MEMORY;
        auto s = std::make_unique<gui_session>(); s->id = ++next_session; s->limit = session_bytes;
        const auto scope = _scope_create({_startup().quota, shared.selector()}); if (scope.status != STATUS_OK) return scope.status;
        s->scope = _cap_owner{scope.value};
        if (const auto cap = q.capability(q.data["budget"])) {
            const auto copy = _cap_duplicate(cap, _startup().cspace, RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
            if (copy.status != STATUS_OK) return copy.status;
            s->budget = _cap_owner{copy.value};
        }
        auto* p = s.get();
        auto status = s->client.add("Connect", {{"events", ubus_type::Cap}}, {}, [this, p](auto& q, auto&) {
            if (p->connected) return STATUS_BUSY;
            auto end = q.take(q.data["events"]); const auto fact = _source_info(end.selector(), SOURCE_WRITE);
            if (fact.status != STATUS_OK || (fact.value & SOURCE_TERMINAL)) return STATUS_BAD_ARGS;
            const auto status = p->ready.open(&_events(), end.selector(), SOURCE_TERMINAL, 0, WATCH_CHANGED, SOURCE_WRITE);
            if (status == STATUS_OK) { p->events = std::move(end); p->connected = true; }
            return status;
        });
        if (status == STATUS_OK) status = s->client.add("Create", {{"title", ubus_type::String, false, 128}, {"width", ubus_type::U64}, {"height", ubus_type::U64}, {"flags", ubus_type::U64}, {"limits", ubus_type::Vector}}, {{"configure", ubus_type::Bytes}}, [this, p](auto& q, auto& r) -> status_t {
            if (!p->connected || p->windows.size() >= max_windows || windows.size() + retired.size() >= max_sessions * max_windows || next_window == UINT64_MAX) return STATUS_NO_MEMORY;
            const auto width = *q.data["width"].template as<uint64_t>(), height = *q.data["height"].template as<uint64_t>();
            if (!gui_bytes(width, height)) return STATUS_BAD_ARGS;
            const auto limits = q.data["limits"].template vector<uint32_t>();
            if (!limits || limits->size() != 4) return STATUS_BAD_ARGS;
            const auto l = *limits;
            if (!l[0] || !l[1] || l[2] < l[0] || l[3] < l[1] || l[2] > INT32_MAX || l[3] > INT32_MAX || l[0] > output_width || l[1] > output_height - (hosted ? 0 : 24)) return STATUS_BAD_ARGS;
            auto w = std::make_unique<gui_window>(); w->owner = p; w->id = ++next_window;
            w->limits = {l[0], l[1], l[2], l[3]};
            w->title = *q.data["title"].template as<std::string_view>(); w->flags = *q.data["flags"].template as<uint64_t>() & (gui_resizable | gui_fullscreen | gui_keep_aspect);
            w->rect = {hosted ? 0 : 24 + int32_t((w->id - 1) % 8) * 24, hosted ? 0 : 46 + int32_t((w->id - 1) % 6) * 24,
                int32_t(std::clamp<uint64_t>(width, l[0], std::max(l[0], std::min(l[2], output_width - (hosted ? 0 : 32))))),
                int32_t(std::clamp<uint64_t>(height, l[1], std::max(l[1], std::min(l[3], output_height - (hosted ? 0 : 72)))))};
            if (!hosted && (w->flags & gui_fullscreen)) { w->restore = w->rect; w->rect = {0, 0, int32_t(output_width), int32_t(output_height)}; }
            auto e = event(w.get(), gui_configure); e.serial = w->serial; e.flags = w->flags; e.width = w->rect.width; e.height = w->rect.height;
            r.data.put("configure", std::as_bytes(std::span{&e, 1}));
            windows.push_back(w.get()); damaged(bounds(*w));
            // Activation is delegated to this session by its launcher, not by Create.
            if (p->active && p->activate) { set_focus(w.get()); p->activate = false; }
            p->windows.emplace(w->id, std::move(w)); return STATUS_OK;
        });
        if (status == STATUS_OK) status = s->client.add("Allocate", {{"window", ubus_type::U64}, {"width", ubus_type::U64}, {"height", ubus_type::U64}, {"format", ubus_type::U64}}, {{"id", ubus_type::U64}, {"stride", ubus_type::U64}, {"pixels", ubus_type::Buffer}}, [this, p](auto& q, auto& r) -> status_t {
            const auto id = *q.data["window"].template as<uint64_t>(), width = *q.data["width"].template as<uint64_t>(), height = *q.data["height"].template as<uint64_t>(), format = *q.data["format"].template as<uint64_t>();
            const auto w = p->windows.find(id); if (w == p->windows.end()) return STATUS_NOT_FOUND;
            const auto bytes = gui_bytes(width, height);
            if (!bytes || (format != gui_xrgb && format != gui_argb)) return STATUS_BAD_ARGS;
            if (p->buffers.size() >= max_windows * 4 || *bytes > p->limit - p->bytes || p->next_buffer == INT32_MAX) return STATUS_NO_MEMORY;
            auto resident = _vm(); resident.pager = 0;
            auto area = _vm_area::create({p->budget ? p->budget.selector() : _startup().quota, p->scope.selector()}, resident, 0, *bytes);
            if (!area) return area.error();
            // Existing client mappings must remain writable during service restart,
            // including first touches after this execution's quota starts closing.
            for (size_t page = 0; page < *bytes / 4096; ++page) {
                const auto status = _memory_populate(area->memory.selector(), page).status;
                if (status != STATUS_OK) return status;
            }
            auto cap = descriptor.derive(area->memory.selector(), _cap_mem(RIGHT_MAP | RIGHT_DUPLICATE, 0, *bytes / 4096, VM_READ | VM_WRITE)); if (!cap) return cap.error();
            const auto index = r.give(std::move(*cap)); if (!index) return index.error();
            const auto number = ++p->next_buffer;
            r.data.put("id", uint64_t{number}).put("stride", width * 4).put("pixels", ubus_buffer{*index, 0, *bytes});
            p->bytes += *bytes; p->buffers.emplace(number, gui_pixels{std::move(*area), id, 0, uint32_t(width), uint32_t(height), uint32_t(format)});
            return STATUS_OK;
        });
        if (status == STATUS_OK) status = s->client.add("Commit", {{"args", ubus_type::Vector}}, {}, [this, p](auto& q, auto&) -> status_t {
            auto args = q.data["args"].template vector<uint64_t>(); if (!args || args->size() != 9) return STATUS_BAD_ARGS;
            const auto a = *args; const auto it = p->windows.find(a[0]); if (it == p->windows.end()) return STATUS_NOT_FOUND;
            auto& w = *it->second; const auto b = p->buffers.find(a[2]);
            if (a[1] != w.serial || a[3] <= p->last_commit) return STATUS_STALE;
            if (b == p->buffers.end() || b->second.window != w.id || b->second.busy) return STATUS_BAD_ARGS;
            if (a[4] > b->second.width || a[5] > b->second.height || !a[6] || !a[7] || a[6] > b->second.width - a[4] || a[7] > b->second.height - a[5] || a[8] > 1) return STATUS_BAD_ARGS;
            if (p->queue.size() - p->sent + p->reserved + 2 > 192) return STATUS_BUSY;
            p->reserved += 1 + bool(a[8]);
            const auto previous = w.pending ? w.pending : w.current;
            if (!previous || p->buffers.find(previous)->second.width != b->second.width || p->buffers.find(previous)->second.height != b->second.height || p->buffers.find(previous)->second.format != b->second.format)
                w.damage = {0, 0, int32_t(b->second.width), int32_t(b->second.height)};
            release(w, w.pending, gui_discarded); w.pending = a[2];
            b->second.busy = true; b->second.commit = a[3]; b->second.frame = a[8]; p->last_commit = a[3];
            gui_damage(w.damage, {int32_t(a[4]), int32_t(a[5]), int32_t(a[6]), int32_t(a[7])});
            return STATUS_OK;
        });
        if (status == STATUS_OK) status = s->client.add("Destroy", {{"window", ubus_type::U64}, {"buffer", ubus_type::U64}}, {}, [this, p](auto& q, auto&) {
            auto i = p->buffers.find(*q.data["buffer"].template as<uint64_t>());
            if (i == p->buffers.end() || i->second.window != *q.data["window"].template as<uint64_t>()) return STATUS_NOT_FOUND;
            if (i->second.busy) return STATUS_BUSY;
            const auto cap = i->second.area.memory.selector(); const auto size = i->second.area.size;
            // Destroy revokes remote writable mappings before storage can be reused.
            auto s = _object_destroy(cap).status; if (s != STATUS_OK) return s;
            s = i->second.area.close(); if (s == STATUS_OK) { p->buffers.erase(i); p->bytes -= size; } return s;
        });
        if (status == STATUS_OK) status = s->client.add("Configure", {{"window", ubus_type::U64}, {"title", ubus_type::String, true, 128}, {"fullscreen", ubus_type::Bool, true}}, {}, [this, p](auto& q, auto&) {
            auto i = p->windows.find(*q.data["window"].template as<uint64_t>()); if (i == p->windows.end()) return STATUS_NOT_FOUND;
            auto& w = *i->second;
            if (const auto title = q.data["title"].template as<std::string_view>()) { w.title = *title; w.host_dirty = true; }
            if (const auto full = q.data["fullscreen"].template as<bool>(); full && *full != bool(w.flags & gui_fullscreen))
                configure(w, *full ? output_width : w.restore.width, *full ? output_height : w.restore.height, *full);
            damaged(bounds(w)); return STATUS_OK;
        });
        if (status == STATUS_OK) status = s->client.add("Remove", {{"window", ubus_type::U64}}, {}, [this, p](auto& q, auto&) {
            auto i = p->windows.find(*q.data["window"].template as<uint64_t>()); if (i == p->windows.end()) return STATUS_NOT_FOUND;
            remove(*i->second); p->windows.erase(i); return STATUS_OK;
        });
        if (status == STATUS_OK) status = s->control.add("Activate", {{"enabled", ubus_type::Bool}, {"focus", ubus_type::Bool}}, {}, [this, p](auto& q, auto&) {
            p->active = *q.data["enabled"].template as<bool>();
            p->activate = p->active && *q.data["focus"].template as<bool>();
            if (!p->active && focus && focus->owner == p) set_focus(nullptr, false);
            for (auto& [id, w] : p->windows) { damaged(bounds(*w)); w->host_dirty = true; }
            if (p->activate && !p->windows.empty()) { set_focus(p->windows.rbegin()->second.get()); p->activate = false; }
            if (!hosted && !focus) for (auto i = windows.rbegin(); i != windows.rend(); ++i) if (visible(**i)) { set_focus(*i); break; }
            return STATUS_OK;
        });
        if (status == STATUS_OK) status = s->control.add("Close", {}, {}, [this, p](auto&, auto&) { stop(*p); return STATUS_OK; });
        if (status == STATUS_OK) status = s->client.open(16);
        if (status == STATUS_OK) status = s->control.open(4);
        if (status != STATUS_OK) return status;
        auto a = s->client.handle(), b = s->control.handle(); if (!a || !b) return !a ? a.error() : b.error();
        auto ai = r.give(std::move(*a)), bi = r.give(std::move(*b)); if (!ai || !bi) return STATUS_NO_MEMORY;
        r.data.cap("client", *ai).cap("control", *bi).put("bytes", p->limit);
        sessions.push_back(std::move(s)); return STATUS_OK;
    }

    void host_lost(status_t error) {
        host_error = error;
        for (auto* w : windows) {
            if (w->host_flight && w->current) {
                auto& b = w->owner->buffers.find(w->current)->second;
                if (b.frame) { auto e = event(w, gui_frame); e.serial = b.commit; e.value = gui_failed; enqueue(*w->owner, e, true); b.frame = false; }
            }
            w->host_offset = w->host_bytes = w->host_flight = 0;
            w->host_dirty = w->host_repaint = true; w->host_published = false;
        }
        host.close(); host_epoch = host_input_sequence = 0; retired.clear();
        held.reset(); reset_seat(); set_focus(nullptr, false);
        for (auto& s : sessions) enqueue(*s, event(nullptr, gui_display));
    }
    auto host_poll() -> bool {
        const auto status = host.poll(max_sessions * max_windows);
        if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) { host_lost(status); return true; }
        if (!host.ready()) return false;
        if (host_epoch != host.epoch()) {
            host_epoch = host.epoch(); host_input_sequence = 0; host_error = STATUS_OK;
            output_width = host.width; output_height = host.height;
            for (auto* w : windows) { w->host_dirty = w->host_repaint = true; w->host_published = false; }
        }
        bool again{};
        for (size_t n = 0; n < 64; ++n) {
            gui_host_packet packet; const auto status = host.receive(packet);
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) { host_lost(status); return true; }
            again = true;
            if (packet.type == gh_ack && packet.flags == gh_remove) {
                const auto it = std::ranges::find_if(retired, [&](const auto& item) { return item.id == packet.window && item.sequence == packet.sequence; });
                if (it != retired.end()) {
                    if (it->bytes) {
                        host.release(it->offset, it->bytes);
                        for (auto* w : windows) if (!w->host_bytes && w->current) w->host_repaint = true;
                    }
                    retired.erase(it);
                }
                continue;
            }
            const auto it = std::ranges::find(windows, packet.window, &gui_window::id);
            if (it == windows.end()) continue;
            auto& w = **it;
            if (packet.type == gh_ack && packet.flags == gh_present) {
                if (w.host_flight != packet.sequence) continue;
                w.host_flight = 0; ++frames_done; present_ticks += _clock_now().value - w.host_submitted;
                if (w.current) {
                    auto& b = w.owner->buffers.find(w.current)->second;
                    if (b.frame) { auto e = event(&w, gui_frame); e.serial = b.commit; e.value = packet.event.value == gui_used ? gui_used : gui_failed; enqueue(*w.owner, e, true); b.frame = false; }
                }
                continue;
            }
            if (packet.type != gh_input || packet.sequence <= host_input_sequence || !visible(w)) continue;
            host_input_sequence = packet.sequence; input_time = _clock_now().value; ++input_batches;
            const auto& value = packet.event;
            if (value.type == gui_focus) {
                if (value.value) set_focus(&w, false);
                else if (focus == &w) { held.reset(); set_focus(nullptr, false); }
            } else if (value.type == gui_reset) {
                if (focus == &w) { held.reset(); reset_seat(); }
            } else if (value.type == gui_close) enqueue(*w.owner, event(&w, gui_close));
            else if (value.type == gui_configure && value.width > 0 && value.height > 0) {
                if (value.width != w.rect.width || value.height != w.rect.height || bool(value.flags & gui_fullscreen) != bool(w.flags & gui_fullscreen))
                    configure(w, value.width, value.height, value.flags & gui_fullscreen, false);
            } else if (value.type == gui_key && focus == &w && value.code > 0 && size_t(value.code) < held.size() && value.value >= 0 && value.value <= 2) {
                const auto code = uint16_t(value.code);
                if (value.value != 2) held[code] = value.value != 0;
                if (value.value == 1 && code == 58) caps_lock = !caps_lock;
                if (value.value == 1 && code == 87) { configure(w, w.restore.width, w.restore.height, !(w.flags & gui_fullscreen)); continue; }
                if (!value.value && !delivered[code]) continue;
                delivered[code] = value.value != 0;
                auto e = event(&w, gui_key); e.code = code; e.value = value.value; input_event(&w, e);
                if (value.value) { const auto ch = character(code); if (ch && (*ch < 32 || *ch == 127)) { e = event(&w, gui_text); e.value = *ch; input_event(&w, e); } }
            } else if (value.type == gui_text && focus == &w && value.value >= 32 && value.value <= 0x10ffff && (value.value < 0xd800 || value.value > 0xdfff)) {
                auto e = event(&w, gui_text); e.value = value.value; input_event(&w, e);
            } else if (value.type == gui_pointer) {
                auto e = event(&w, gui_pointer); e.x = value.x; e.y = value.y; e.code = value.code; e.value = value.value; input_event(&w, e);
            }
            input_time = 0;
        }
        input_time = 0; return again;
    }
    auto host_paint(uint64_t now) -> bool {
        if (!host.ready()) return false;
        bool again{}, presented{};
        for (auto& item : retired) if (!item.sent) {
            gui_host_packet packet; packet.type = gh_remove; packet.window = item.id; packet.sequence = item.sequence;
            if (host.send(packet) != STATUS_OK) break;
            item.sent = true; again = true;
        }
        for (auto* window : windows) {
            auto& w = *window;
            if (w.host_dirty) {
                gui_host_packet packet; packet.type = gh_window; packet.window = w.id;
                packet.width = w.rect.width; packet.height = w.rect.height; packet.flags = w.flags | (visible(w) ? uint32_t{gh_visible} : 0U) | (w.host_activate ? uint32_t{gh_activate} : 0U); packet.limits = w.limits;
                ::memcpy(packet.title, w.title.data(), std::min(w.title.size(), sizeof(packet.title)));
                if (host.send(packet) != STATUS_OK) break;
                w.host_dirty = w.host_activate = false; w.host_published = true; again = true;
            }
            if (!visible(w)) { if (w.pending) { release(w, w.pending, gui_hidden); w.pending = 0; again = true; } continue; }
            if (w.host_flight || now < next_frame || (!w.pending && (!w.host_repaint || !w.current))) continue;
            const auto id = w.pending ? w.pending : w.current;
            auto& b = w.owner->buffers.find(id)->second; const auto bytes = *gui_bytes(b.width, b.height);
            if (!w.current || w.owner->buffers.find(w.current)->second.width != b.width || w.owner->buffers.find(w.current)->second.height != b.height) w.host_repaint = true;
            if (w.host_bytes != bytes) {
                if (w.host_bytes) host.release(w.host_offset, w.host_bytes);
                w.host_bytes = 0;
                const auto offset = host.allocate(bytes);
                if (!offset) {
                    if (w.pending) { release(w, w.pending, gui_failed); w.pending = 0; }
                    w.host_repaint = false; continue;
                }
                w.host_offset = *offset; w.host_bytes = bytes; w.host_repaint = true;
            }
            const auto full = gui_rect{0, 0, int32_t(b.width), int32_t(b.height)};
            const auto damage = w.host_repaint ? full : gui_intersect(w.damage, full);
            if (damage.empty()) continue;
            gui_host_packet packet; packet.type = gh_present; packet.window = w.id; packet.sequence = present_sequence + 1;
            packet.offset = w.host_offset; packet.width = b.width; packet.height = b.height; packet.stride = b.width * 4; packet.damage = damage;
            if (!host.writable()) continue;
            const auto started = _clock_now().value;
            for (int32_t y = damage.y; y < damage.y + damage.height; ++y) {
                const auto offset = (size_t(y) * b.width + damage.x) * 4;
                ::memcpy(reinterpret_cast<void*>(host.data() + w.host_offset + offset), reinterpret_cast<const void*>(b.area.address + offset), size_t(damage.width) * 4);
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (host.send(packet) != STATUS_OK) _thread_exit(STATUS_INTERNAL);
            if (w.pending) { release(w, w.current); w.current = w.pending; w.pending = 0; }
            w.host_flight = ++present_sequence; w.host_submitted = started;
            w.host_repaint = false; w.damage = {}; presented = again = true; ++present_calls;
            const auto elapsed = _clock_now().value - started;
            ++compose_count; compose_ticks += elapsed; compose_max = std::max(compose_max, elapsed);
            composed_pixels += uint64_t(damage.width) * damage.height; copied_bytes += uint64_t(damage.width) * damage.height * 4;
            ++compose_histogram[std::min<size_t>(7, std::bit_width(elapsed * 1000 / _startup().frequency))];
        }
        if (presented) next_frame = now + interval;
        return again;
    }

    auto character(uint16_t code) -> std::optional<uint32_t> {
        static constexpr char plain[] = "\0\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 \0";
        static constexpr char shifted[] = "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 \0";
        if (code >= sizeof(plain) - 1) return {};
        const bool shift = held[42] || held[54];
        uint32_t ch = (shift ? shifted : plain)[code];
        if (!ch) return {};
        if (caps_lock && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) ch ^= 32;
        if ((held[29] || held[97]) && ch == ' ') ch = 0;
        else if ((held[29] || held[97]) && ch >= '@' && ch <= '_') ch &= 31;
        else if ((held[29] || held[97]) && ch >= 'a' && ch <= 'z') ch &= 31;
        return ch;
    }
    void text(uint16_t code) {
        const auto ch = character(code); if (!ch || !focus) return;
        auto e = event(focus, gui_text); e.value = *ch; input_event(focus, e);
    }
    void key(uint16_t code, int32_t value) {
        if (code >= held.size() || value < 0 || value > 2) { reset_seat(); return; }
        if (value == 2) return; // Seat owns repeat deadlines.
        if (bool(value) == held[code]) return;
        held[code] = value;
        if (value && code == 58) caps_lock = !caps_lock;
        if (value && (held[56] || held[100])) {
            if (code == 15) {
                if (!windows.empty()) {
                    auto i = std::ranges::find(windows, focus); const size_t start = i == windows.end() ? 0 : size_t(i - windows.begin());
                    for (size_t n = 1; n <= windows.size(); ++n) {
                        auto* w = windows[(start + windows.size() - n) % windows.size()];
                        if (visible(*w)) { set_focus(w); break; }
                    }
                }
                return;
            }
            if (code == 62 && focus) { enqueue(*focus->owner, event(focus, gui_close)); return; }
            if (code == 1) { reset_seat(); return; }
        }
        if (value && code == 87 && focus) { configure(*focus, focus->restore.width, focus->restore.height, !(focus->flags & gui_fullscreen)); return; }
        if (!focus) return;
        if (!value && !delivered[code]) return;
        delivered[code] = value;
        auto e = event(focus, gui_key); e.code = code; e.value = value; input_event(focus, e);
        if (value && delivered[code]) { text(code); if (character(code)) { repeat_key = code; repeat_due = _clock_now().value + _startup().frequency / 2; } }
        else if (repeat_key == code) repeat_due = 0;
    }
    void pointer(uint16_t code = 0, int32_t value = 0) {
        if (drag) {
            damaged(bounds(*drag));
            if (resizing) configure(*drag, pointer_x - drag->rect.x + drag_x, pointer_y - drag->rect.y + drag_y, false);
            else { drag->rect.x = std::clamp(pointer_x - drag_x, 2, int32_t(output_width) - 32); drag->rect.y = std::clamp(pointer_y - drag_y, 22, int32_t(output_height) - 16); }
            damaged(bounds(*drag));
            if (code == 272 && !value) drag = nullptr;
            return;
        }
        if (code >= 272 && code <= 279 && !value && (!grab || !(buttons & (1U << (code - 272))))) return;
        gui_window* target = grab;
        if (!target) for (auto i = windows.rbegin(); i != windows.rend(); ++i) {
            if (visible(**i) && !gui_intersect(bounds(**i), {pointer_x, pointer_y, 1, 1}).empty()) { target = *i; break; }
        }
        if (!target) return;
        if (code >= 272 && code <= 279 && value) {
            set_focus(target);
            if (code == 272 && !(target->flags & gui_fullscreen) && pointer_y < target->rect.y) {
                if (pointer_x >= target->rect.x + target->rect.width - 20) enqueue(*target->owner, event(target, gui_close));
                else { drag = target; drag_x = pointer_x - target->rect.x; drag_y = pointer_y - target->rect.y; resizing = false; }
                return;
            }
            if (code == 272 && (target->flags & gui_resizable) && !(target->flags & gui_fullscreen) && pointer_x >= target->rect.x + target->rect.width - 12 && pointer_y >= target->rect.y + target->rect.height - 12) {
                drag = target; resizing = true; drag_x = target->rect.width - (pointer_x - target->rect.x); drag_y = target->rect.height - (pointer_y - target->rect.y); return;
            }
            grab = target; buttons |= 1U << (code - 272);
        }
        if (code >= 272 && code <= 279 && !value) buttons &= ~(1U << (code - 272));
        auto e = event(target, gui_pointer); e.x = pointer_x - target->rect.x; e.y = pointer_y - target->rect.y; e.code = code; e.value = value; e.flags = buttons;
        input_event(target, e);
        if (code >= 272 && code <= 279 && !value && !buttons) grab = nullptr;
    }
    void fill(uint32_t* pixels, gui_rect rect, uint32_t color) {
        if (rect.empty()) return;
        for (int32_t y = rect.y; y < rect.y + rect.height; ++y) std::fill_n(pixels + size_t(y) * output_width + rect.x, rect.width, color);
    }
    void label(uint32_t* pixels, int32_t x, int32_t y, std::string_view text, uint32_t color, gui_rect clip) {
        for (unsigned char ch : text) {
            if (x >= clip.x + clip.width) break;
            const auto r = gui_intersect({x, y, 8, 16}, clip);
            for (int32_t yy = r.y; yy < r.y + r.height; ++yy) for (int32_t xx = r.x; xx < r.x + r.width; ++xx)
                if (font8x8_basic[ch < 128 ? ch : '?'][(yy - y) / 2] & (1U << (xx - x))) pixels[size_t(yy) * output_width + xx] = color;
            x += 8;
        }
    }
    void render(gui_output& out, gui_rect damage) {
        auto* pixels = reinterpret_cast<uint32_t*>(out.pixels.address);
        size_t first{}; bool covered{};
        // A complete opaque cover makes every lower read/write unnecessary. This
        // also handles fullscreen through the normal scene and lease protocol.
        for (size_t n = windows.size(); n--;) {
            const auto* w = windows[n]; if (!visible(*w) || !w->current) continue;
            const auto& src = w->owner->buffers.find(w->current)->second;
            const auto r = gui_intersect(damage, w->rect);
            if (src.format == gui_xrgb && r.width == damage.width && r.height == damage.height) { first = n; covered = true; break; }
        }
        if (!covered) {
            fill(pixels, damage, 0x00182030);
            fill(pixels, gui_intersect(damage, {0, 0, int32_t(output_width), 24}), 0x00273549);
            label(pixels, 12, 4, "myos    Alt+Tab: switch    F11: fullscreen    Alt+Esc: reset input", 0x00c5d4e8, damage);
        }
        for (size_t n = first; n < windows.size(); ++n) {
            auto* w = windows[n];
            if (!visible(*w)) continue;
            const auto b = gui_intersect(bounds(*w), damage); if (b.empty()) continue;
            if (!(w->flags & gui_fullscreen)) {
                const uint32_t color = w == focus ? 0x0046739d : 0x00374658;
                fill(pixels, gui_intersect(damage, {w->rect.x - 2, w->rect.y - 22, w->rect.width + 4, 22}), color);
                fill(pixels, gui_intersect(damage, {w->rect.x - 2, w->rect.y, 2, w->rect.height}), color);
                fill(pixels, gui_intersect(damage, {w->rect.x + w->rect.width, w->rect.y, 2, w->rect.height}), color);
                fill(pixels, gui_intersect(damage, {w->rect.x - 2, w->rect.y + w->rect.height, w->rect.width + 4, 2}), color);
                label(pixels, w->rect.x + 6, w->rect.y - 18, w->title, 0x00f2f5fa, gui_intersect(damage, {w->rect.x, w->rect.y - 22, w->rect.width - 24, 22}));
                label(pixels, w->rect.x + w->rect.width - 16, w->rect.y - 18, "x", 0x00f2f5fa, damage);
            }
            const auto content = gui_intersect(w->rect, damage);
            if (!w->current) { fill(pixels, content, 0x000a1018); continue; }
            auto& src = w->owner->buffers.find(w->current)->second;
            const auto view = w->flags & gui_keep_aspect ? gui_fit(w->rect, src.width, src.height) : w->rect;
            const auto r = gui_intersect(content, view);
            if (src.format == gui_xrgb) {
                fill(pixels, gui_intersect(content, {w->rect.x, w->rect.y, w->rect.width, view.y - w->rect.y}), 0);
                fill(pixels, gui_intersect(content, {w->rect.x, view.y + view.height, w->rect.width, w->rect.y + w->rect.height - view.y - view.height}), 0);
                fill(pixels, gui_intersect(content, {w->rect.x, view.y, view.x - w->rect.x, view.height}), 0);
                fill(pixels, gui_intersect(content, {view.x + view.width, view.y, w->rect.x + w->rect.width - view.x - view.width, view.height}), 0);
            }
            const auto* data = reinterpret_cast<const uint32_t*>(src.area.address);
            const bool same = view.width == int32_t(src.width) && view.height == int32_t(src.height);
            for (int32_t y = r.y; y < r.y + r.height; ++y) {
                auto* dst = pixels + size_t(y) * output_width + r.x;
                const auto* row = data + (same ? y - view.y : uint64_t(y - view.y) * src.height / view.height) * src.width;
                if (src.format == gui_xrgb && same) ::memcpy(dst, row + r.x - view.x, size_t(r.width) * 4);
                else for (int32_t x = 0; x < r.width; ++x) {
                    const uint32_t v = row[same ? r.x - view.x + x : uint64_t(r.x - view.x + x) * src.width / view.width];
                    if (src.format == gui_xrgb) { dst[x] = v; continue; }
                    const auto alpha = v >> 24, old = dst[x];
                    uint32_t result{};
                    for (unsigned c = 0; c < 24; c += 8) result |= std::min(255U, ((v >> c) & 255) + (((old >> c) & 255) * (255 - alpha) + 127) / 255) << c;
                    dst[x] = result;
                }
            }
            if (src.format == gui_xrgb) copied_bytes += uint64_t(r.width) * r.height * 4;
        }
    }
    auto paint(uint64_t now) -> bool {
        if (display.phase != 6 || present_call || cursor_call || frame_pending || now < next_frame) return false;
        auto out = std::ranges::find_if(outputs, [this](const auto& o) { return !o.busy; }); if (out == outputs.end()) return false;
        bool changed{};
        for (auto* w : windows) {
            if (w->pending) {
                const auto& next = w->owner->buffers.find(w->pending)->second;
                const auto old = w->current ? &w->owner->buffers.find(w->current)->second : nullptr;
                const bool raster_changed = !old || old->width != next.width || old->height != next.height || old->format != next.format;
                release(*w, w->current); w->current = std::exchange(w->pending, 0);
                if (visible(*w)) {
                    if (raster_changed) damaged(w->rect);
                    else {
                        const auto view = w->flags & gui_keep_aspect ? gui_fit(w->rect, next.width, next.height) : w->rect;
                        const auto x = uint64_t(w->damage.x) * view.width / next.width, y = uint64_t(w->damage.y) * view.height / next.height;
                        const auto right = (uint64_t(w->damage.x + w->damage.width) * view.width + next.width - 1) / next.width;
                        const auto bottom = (uint64_t(w->damage.y + w->damage.height) * view.height + next.height - 1) / next.height;
                        damaged({view.x + int32_t(x), view.y + int32_t(y), int32_t(right - x), int32_t(bottom - y)});
                    }
                }
                w->damage = {}; changed = true;
            }
            if (!visible(*w) && w->current) {
                auto& b = w->owner->buffers.find(w->current)->second;
                if (b.frame) { auto e = event(w, gui_frame); e.serial = b.commit; e.value = gui_hidden; enqueue(*w->owner, e, true); b.frame = false; changed = true; }
            }
        }
        if (!scene_dirty) {
            // A commit wholly outside the output still owns feedback credits.
            for (auto* w : windows) if (w->current) {
                auto& b = w->owner->buffers.find(w->current)->second;
                if (b.frame) { auto e = event(w, gui_frame); e.serial = b.commit; e.value = gui_hidden; enqueue(*w->owner, e, true); b.frame = false; changed = true; }
            }
            return changed;
        }
        const auto start = _clock_now().value;
        const auto damage = out->damage;
        // Other buffers keep their accumulated age damage until the next scene
        // change; repairing an idle back buffer is not a presentation request.
        render(*out, damage); out->damage = {}; scene_dirty = false;
        const auto elapsed = _clock_now().value - start;
        compose_ticks += elapsed; ++compose_count; compose_max = std::max(compose_max, elapsed);
        ++compose_histogram[std::min<size_t>(7, std::bit_width(elapsed * 1000 / _startup().frequency))];
        composed_pixels += uint64_t(damage.width) * damage.height;
        for (auto* w : windows) if (visible(*w) && w->current) {
            auto& b = w->owner->buffers.find(w->current)->second;
            if (b.frame) { auto e = event(w, gui_frame); e.serial = b.commit; e.sequence = w->owner->id; out->frames.push_back(e); b.frame = false; }
        }
        const auto n = uint64_t(out - outputs.begin());
        const std::array<uint64_t, 7> args{display.generation, n, ++present_sequence, uint64_t(damage.x), uint64_t(damage.y), uint64_t(damage.width), uint64_t(damage.height)};
        ubus_msg q; q.data.array("args", std::span{args});
        out->submitted = _clock_now().value;
        const auto s = display.peer.begin("Present", std::move(q));
        if (s != STATUS_OK) { display_lost(); return true; }
        ++present_calls;
        out->busy = true; out->sequence = present_sequence; present_call = frame_pending = true;
        next_frame = now + interval;
        return true;
    }
public:
    auto run(ubus_conn& parent, ubus_msg setup) -> status_t {
        display.display = true;
        auto status = STATUS_OK;
        const auto config = setup.data["message"];
        shared = setup.take(config["shared"]); if (!shared) return STATUS_BAD_ARGS;
        max_sessions = std::clamp<uint64_t>(config["sessions"].as<uint64_t>().value_or(8), 1, 32);
        max_windows = std::clamp<uint64_t>(config["windows"].as<uint64_t>().value_or(4), 1, 16);
        session_bytes = config["bytes"].as<uint64_t>().value_or(16 << 20);
        interval = _startup().frequency / std::clamp<uint64_t>(config["rate"].as<uint64_t>().value_or(60), 1, 240);
        const auto backend = config["backend"].as<std::string_view>().value_or("auto");
        if (backend != "native") {
            ubus_msg query, response; query.data.put("name", "hardware");
            status = parent.invoke("Get", std::move(query), response); if (status != STATUS_OK) return status;
            ubus_conn inventory;
            status = inventory.bind(response.capability(response.data["connector"]), "io.devices", 1, 1);
            if (status == STATUS_OK) status = inventory.invoke("List", {}, response);
            if (status != STATUS_OK) return status;
            for (const auto item : response.data["items"].values()) hosted |= item["interface"].as<std::string_view>() == "io.stream";
            if (backend == "host" && !hosted) return STATUS_NOT_FOUND;
        }
        ubus_object launcher{"gui.launcher"};
        status = launcher.add("Open", {{"budget", ubus_type::Cap, true}}, {{"client", ubus_type::Cap}, {"control", ubus_type::Cap}, {"bytes", ubus_type::U64}}, [this](auto& q, auto& r) { return session_open(q, r); });
        if (status == STATUS_OK) status = launcher.add("Stat", {}, {{"frames", ubus_type::U64}, {"pixels", ubus_type::U64}, {"copied_bytes", ubus_type::U64}, {"compose_ticks", ubus_type::U64}, {"compose_count", ubus_type::U64}, {"compose_max", ubus_type::U64}, {"compose_histogram", ubus_type::Vector}, {"present_ticks", ubus_type::U64}, {"display", ubus_type::Record}, {"sessions", ubus_type::U64}, {"inputs", ubus_type::Array}, {"windows", ubus_type::Array}, {"frequency", ubus_type::U64}, {"focus", ubus_type::U64}, {"traffic", ubus_type::Record}}, [this](auto&, auto& r) {
            ubus_data output; output.put("ready", hosted ? host.ready() : display.phase == 6).put("status", int64_t(hosted ? host_error : display.error)).put("backend", hosted ? "host" : "native").put("generation", hosted ? host_epoch : display.generation).put("width", uint64_t(output_width)).put("height", uint64_t(output_height));
            ubus_data devices{ubus_type::Array};
            for (const auto& i : inputs) { ubus_data row; row.put("id", i->id).put("ready", i->phase == 6).put("status", int64_t(i->error)); devices.push(row); }
            if (hosted) devices.push(ubus_data{}.put("id", "host").put("ready", host.ready()).put("status", int64_t(host_error)));
            ubus_data scene{ubus_type::Array};
            for (const auto* w : windows) {
                ubus_data row; row.put("id", w->id).put("title", w->title)
                    .put("width", uint64_t(w->rect.width)).put("height", uint64_t(w->rect.height)).put("flags", uint64_t(w->flags)).put("visible", visible(*w)).put("buffer", uint64_t(w->current));
                if (!hosted) row.put("x", int64_t(w->rect.x)).put("y", int64_t(w->rect.y));
                if (w->current) {
                    const auto& b = w->owner->buffers.find(w->current)->second;
                    row.put("buffer_width", uint64_t(b.width)).put("buffer_height", uint64_t(b.height));
                }
                scene.push(row);
            }
            r.data.put("windows", scene).put("frequency", _startup().frequency).put("focus", focus ? focus->id : 0);
            r.data.put("traffic", ubus_data{}.put("client_calls", client_calls).put("events_sent", events_sent).put("input_batches", input_batches).put("present_calls", present_calls).put("cursor_calls", cursor_calls));
            r.data.put("frames", frames_done).put("pixels", composed_pixels).put("compose_ticks", compose_ticks).put("present_ticks", present_ticks)
                .put("copied_bytes", copied_bytes).put("compose_count", compose_count).put("compose_max", compose_max).array("compose_histogram", std::span{compose_histogram})
                .put("display", output).put("sessions", uint64_t(sessions.size())).put("inputs", devices); return STATUS_OK;
        });
        if (status == STATUS_OK) status = launcher.open();
        if (status != STATUS_OK) return status;
        auto connector = launcher.connector(); if (!connector) return connector.error();
        ubus_msg pub, reply; const auto idx = pub.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE); if (!idx) return idx.error();
        pub.data.put("name", "gui").put("interface", "gui.launcher").put("version", uint64_t{1}).cap("connector", *idx);
        status = parent.invoke("Publish", std::move(pub), reply); if (status != STATUS_OK) return status;
        // Discovery is infrequent and outside the frame/input path.
        ubus_conn inventory, discovery;
        unsigned discovering{}; uint64_t discover_due{};
        for (;;) {
            bool again{}; const auto now = _clock_now().value;
            if (hosted) again |= host_poll();
            else {
            if (display.phase != 6) {
                status = display.poll();
                if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) { display.error = status; display_lost(); }
            } else {
                if (present_call || cursor_call) {
                    status = display.peer.poll(reply);
                    if (status == STATUS_OK) {
                        if (reply.status != STATUS_OK) { display_lost(); }
                        present_call = cursor_call = false; again = true;
                    } else if (status != STATUS_WOULD_BLOCK) { display_lost(); }
                }
                for (size_t n = 0; display.phase == 6 && n < 16; ++n) {
                    ubus_request q; status = display.input.receive(q).status; if (status == STATUS_WOULD_BLOCK) break;
                    auto bytes = q.data["event"].as<std::span<const std::byte>>();
                    if (status != STATUS_OK || !bytes || bytes->size() != sizeof(dp_event)) { display_lost(); break; }
                    dp_event e; ::memcpy(&e, bytes->data(), sizeof(e));
                    if (e.generation != display.generation || e.buffer >= outputs.size()) { display_lost(); break; }
                    auto& o = outputs[e.buffer];
                    if (e.sequence != o.sequence) { display_lost(); break; }
                    if (e.type == dp_release) o.busy = false;
                    else if (e.type == dp_frame) {
                        frame_pending = false; ++frames_done; present_ticks += e.time - o.submitted;
                        for (auto frame : o.frames) for (auto& s : sessions) if (s->id == frame.sequence) {
                            { frame.time = e.time; frame.sequence = ++event_sequence; frame.value = e.status == STATUS_OK ? gui_used : gui_failed; enqueue(*s, frame, true); }
                        }
                        o.frames.clear();
                    } else { display_lost(); break; }
                    again = true;
                }
            }
            if (!discovering && now >= discover_due) { status = discovery.begin_bind(_startup().bootstrap); if (status == STATUS_OK) discovering = 1; }
            if (discovering) {
                ubus_msg q, r;
                if (discovering == 1) { status = discovery.poll_bind(); if (status == STATUS_OK) { q.data.put("name", "hardware"); status = discovery.begin("Get", std::move(q)); if (status == STATUS_OK) discovering = 2; } }
                else if (discovering == 2) { status = discovery.poll(r); if (status == STATUS_OK) { status = r.status; if (status == STATUS_OK) { status = inventory.begin_bind(r.capability(r.data["connector"]), "io.devices", 1, 1); if (status == STATUS_OK) discovering = 3; } } }
                else if (discovering == 3) { status = inventory.poll_bind(); if (status == STATUS_OK) { status = inventory.begin("List", {}); if (status == STATUS_OK) discovering = 4; } }
                else { status = inventory.poll(r); if (status == STATUS_OK) {
                    if (r.status == STATUS_OK) for (auto row : r.data["items"].values()) {
                        const auto id = row["id"].as<std::string_view>();
                        if (row["type"].as<std::string_view>() != "Input" || !id) continue;
                        if (std::ranges::none_of(inputs, [&](const auto& i) { return i->id == *id; })) { auto i = std::make_unique<gui_link>(*this); i->id = *id; inputs.push_back(std::move(i)); }
                    }
                    discovering = 0; discover_due = now + _startup().frequency * 2;
                } }
                if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) { (void)discovery.close(); (void)inventory.close(); discovering = 0; discover_due = now + _startup().frequency; }
            }
            for (auto& i : inputs) {
                if (i->phase != 6) {
                    status = i->poll(); if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) { i->error = status; link_lost(*i); }
                    continue;
                }
                for (size_t n = 0; n < 64; ++n) {
                    ubus_request q; status = i->input.receive(q).status; if (status == STATUS_WOULD_BLOCK) break;
                    auto bytes = q.data["event"].as<std::span<const std::byte>>();
                    if (status != STATUS_OK || !bytes || bytes->size() != sizeof(in_batch)) { link_lost(*i); reset_seat(); break; }
                    in_batch b; ::memcpy(&b, bytes->data(), sizeof(b));
                    ++input_batches;
                    if (b.count > std::size(b.events)) { link_lost(*i); reset_seat(); break; }
                    if (b.reset || (i->sequence && b.sequence != i->sequence && b.sequence != i->sequence + 1)) {
                        i->pressed.reset();
                        held.reset(); for (const auto& device : inputs) held |= device->pressed;
                        reset_seat();
                    }
                    i->sequence = b.sequence; input_time = b.time;
                    for (auto e : std::span{b.events}.first(b.count)) {
                        if (e.type == 1) { if (e.code < 272 && e.value != 2) {
                            if (e.value != 0 && e.value != 1) { reset_seat(); continue; }
                            i->pressed[e.code] = e.value;
                            const bool down = std::ranges::any_of(inputs, [&](const auto& device) { return device->pressed[e.code]; });
                            key(e.code, down);
                        } else if (e.code >= 272) pointer(e.code, e.value); }
                        else if (e.type == 3) {
                            if (e.code == 0) pointer_x = (int64_t(std::clamp(e.value, i->minimum[0], i->maximum[0])) - i->minimum[0]) * (output_width - 1) / (int64_t(i->maximum[0]) - i->minimum[0]);
                            if (e.code == 1) pointer_y = (int64_t(std::clamp(e.value, i->minimum[1], i->maximum[1])) - i->minimum[1]) * (output_height - 1) / (int64_t(i->maximum[1]) - i->minimum[1]);
                            cursor_dirty = i->motion = true;
                        } else if (e.type == 2) {
                            if (e.code == 0) pointer_x = std::clamp<int64_t>(int64_t{pointer_x} + e.value, 0, output_width - 1);
                            if (e.code == 1) pointer_y = std::clamp<int64_t>(int64_t{pointer_y} + e.value, 0, output_height - 1);
                            if (e.code == 8) pointer(e.code, e.value);
                            cursor_dirty = i->motion = true;
                        } else if (!e.type && !e.code && i->motion) { pointer(); i->motion = false; }
                    }
                    input_time = 0; again = true;
                }
            }
            if (repeat_due && now >= repeat_due) { if (held[repeat_key] && delivered[repeat_key]) { auto e = event(focus, gui_key); e.code = repeat_key; e.value = 2; input_event(focus, e); text(repeat_key); repeat_due = now + _startup().frequency / 30; } else repeat_due = 0; }
            }
            for (size_t n = 0; n < 8; ++n) { status = launcher.poll(); if (status == STATUS_WOULD_BLOCK) break; if (status != STATUS_OK) return status; ++client_calls; again = true; }
            for (auto& s : sessions) {
                if (s->closing) stop(*s);
                if (s->stopped) continue;
                for (size_t n = 0; !s->closing && !s->stopped && n < 8; ++n) {
                    status = s->control.poll(); if (status == STATUS_WOULD_BLOCK) break;
                    if (status != STATUS_OK) { stop(*s); break; } ++client_calls; again = true;
                }
                for (size_t n = 0; !s->closing && !s->stopped && n < 8; ++n) {
                    status = s->client.poll(); if (status == STATUS_WOULD_BLOCK) break;
                    if (status != STATUS_OK) { stop(*s); break; } ++client_calls; again = true;
                }
                if (!s->events || s->stopped) continue;
                const auto fact = _source_info(s->events.selector(), SOURCE_WRITE);
                if (fact.status != STATUS_OK || (fact.value & SOURCE_TERMINAL)) { stop(*s); continue; }
                while (s->sent < s->queue.size()) {
                    ubus_msg q; q.data.put("event", std::as_bytes(std::span{&s->queue[s->sent], 1}));
                    status = ubus_send(s->events.selector(), q, false); if (status == STATUS_WOULD_BLOCK) break;
                    if (status != STATUS_OK) { stop(*s); break; } ++s->sent; ++events_sent; again = true;
                }
                if (s->sent == s->queue.size()) { s->queue.clear(); s->sent = 0; s->blocked = 0; }
                else if (!s->blocked) s->blocked = now;
                else if (now - s->blocked > _startup().frequency * 5) stop(*s);
                if (!s->stopped) { status = s->ready.arm(fact.value2).status; if (status != STATUS_OK) stop(*s); }
            }
            std::erase_if(sessions, [this](const auto& s) { return s->stopped; });
            if (sessions.size() > 1) std::rotate(sessions.begin(), sessions.begin() + 1, sessions.end());
            again |= hosted ? host_paint(now) : paint(now);
            if (!hosted && display.phase == 6 && cursor_dirty && !present_call && !cursor_call) {
                ubus_msg q; q.data.put("x", uint64_t(pointer_x)).put("y", uint64_t(pointer_y));
                status = display.peer.begin("Cursor", std::move(q));
                if (status == STATUS_OK) { ++cursor_calls; cursor_call = true; cursor_dirty = false; }
                else { display_lost(); }
            }
            uint64_t due = hosted ? host.deadline() : now + _startup().frequency;
            if (!hosted && !display.phase) due = std::min(due, std::max(now + 1, display.retry));
            for (const auto& i : inputs) if (!hosted && !i->phase) due = std::min(due, std::max(now + 1, i->retry));
            if (repeat_due) due = std::min(due, repeat_due);
            if (!frame_pending && !present_call && !cursor_call && display.phase == 6 && (scene_dirty || std::ranges::any_of(windows, [this](const auto* w) { return w->pending != 0; }))) due = std::min(due, std::max(now + 1, next_frame));
            if (hosted && host.ready() && std::ranges::any_of(windows, [this](const auto* w) { return visible(*w) && !w->host_flight && (w->pending || (w->host_repaint && w->current)); })) due = std::min(due, std::max(now + 1, next_frame));
            for (const auto& s : sessions) if (s->blocked) due = std::min(due, s->blocked + _startup().frequency * 5);
            if (!again) { status = _ev_wait(&_events(), due).status; if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status; }
        }
    }
};

auto gui_serve(ubus_conn& parent, ubus_msg setup) -> status_t {
    auto server = std::make_unique<gui_server>(); return server->run(parent, std::move(setup));
}
