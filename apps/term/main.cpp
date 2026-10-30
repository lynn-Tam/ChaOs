#include <gui/gui.hpp>
#include <io.hpp>
#include <mgr/mgr.hpp>
#include <thread.hpp>
#include <vterm.h>
#include <font8x8_basic.h>
#include <bitset>
#include <deque>

static VTerm* vt;
static VTermScreen* screen;
static VTermPos cursor_pos;
static bool cursor_visible{true};
static bool frame_pending, content_dirty;
static gui_client gui;
static gui_event geometry;
struct term_buffer { gui_buffer buffer; gui_rect dirty; };
static std::vector<term_buffer> pixels;
static std::deque<std::vector<VTermScreenCell>> history;
static size_t scroll;
static std::deque<ubus_frame> typed;
static std::bitset<768> keys;
static int rows{24}, columns{80};
static int mouse_mode;
static std::optional<ubus_msg> resize_event;
static auto dirty(VTermRect r, void*) -> int {
    const auto damage = gui_intersect({r.start_col * 8, r.start_row * 16, (r.end_col - r.start_col) * 8, (r.end_row - r.start_row) * 16}, {0, 0, columns * 8, rows * 16});
    if (damage.empty()) return 1;
    content_dirty = true;
    for (auto& p : pixels) gui_damage(p.dirty, gui_intersect(damage, {0, 0, int32_t(p.buffer.width), int32_t(p.buffer.height)}));
    return 1;
}
static void redraw() { (void)dirty({0, rows, 0, columns}, nullptr); }
static void output(const char* text, size_t size, void*) {
    while (size) {
        if (typed.empty() || typed.back().size == ubus_stream_limit) { if (typed.size() == 32) return; typed.emplace_back(); }
        auto& f = typed.back(); const auto n = std::min(size, ubus_stream_limit - f.size);
        ::memcpy(f.data + f.size, text, n); f.size += n; text += n; size -= n;
    }
}
static void resize() {
    rows = std::max(1, geometry.height / 16); columns = std::max(1, geometry.width / 8);
    vterm_set_size(vt, rows, columns); scroll = 0;
    ubus_msg event; event.data.put("rows", uint64_t(rows)).put("columns", uint64_t(columns)); resize_event.emplace(std::move(event)); redraw();
}
static void draw(term_buffer& target) {
    auto& b = target.buffer; auto* data = reinterpret_cast<uint32_t*>(b.pixels.address);
    auto area = gui_intersect(target.dirty, {0, 0, int32_t(b.width), int32_t(b.height)});
    for (int y = area.y; y < area.y + area.height; ++y) std::fill_n(data + size_t(y) * b.width + area.x, area.width, 0x00101720);
    const auto state = vterm_obtain_state(vt);
    for (int r = area.y / 16; r < std::min(rows, (area.y + area.height + 15) / 16); ++r) for (int c = area.x / 8; c < std::min(columns, (area.x + area.width + 7) / 8); ++c) {
        VTermScreenCell cell{};
        const auto row = int64_t(history.size()) + r - int64_t(scroll);
        if (row < 0) continue;
        if (size_t(row) < history.size()) { if (size_t(c) >= history[row].size()) continue; cell = history[row][c]; }
        else if (!vterm_screen_get_cell(screen, {int(row - history.size()), c}, &cell)) continue;
        vterm_state_convert_color_to_rgb(state, &cell.fg); vterm_state_convert_color_to_rgb(state, &cell.bg);
        uint32_t fg = uint32_t(cell.fg.rgb.red) << 16 | uint32_t(cell.fg.rgb.green) << 8 | cell.fg.rgb.blue;
        uint32_t bg = uint32_t(cell.bg.rgb.red) << 16 | uint32_t(cell.bg.rgb.green) << 8 | cell.bg.rgb.blue;
        if (cell.attrs.reverse || (!scroll && cursor_visible && r == cursor_pos.row && c == cursor_pos.col)) std::swap(fg, bg);
        const auto ch = cell.chars[0] < 128 ? cell.chars[0] : uint32_t('?');
        const auto rect = gui_intersect(area, {c * 8, r * 16, 8, 16});
        for (int y = rect.y; y < rect.y + rect.height; ++y) for (int x = rect.x; x < rect.x + rect.width; ++x) {
            unsigned bits = font8x8_basic[ch][(y - r * 16) / 2]; if (cell.attrs.bold) bits |= bits << 1;
            const bool set = (bits & (1U << (x - c * 8))) || (cell.attrs.underline && y % 16 == 14);
            data[size_t(y) * b.width + x] = !cell.attrs.conceal && set ? fg : bg;
        }
    }
}
static void input(gui_event e) {
    if (e.type == gui_focus) {
        auto* state = vterm_obtain_state(vt);
        if (e.value) { vterm_state_focus_in(state); return; }
        vterm_state_focus_out(state);
    }
    if (e.type == gui_reset || (e.type == gui_focus && !e.value)) {
        for (int button = 1; button <= 3; ++button) vterm_mouse_button(vt, button, false, VTERM_MOD_NONE);
        keys.reset(); return;
    }
    const auto mods = VTermModifier((keys[42] || keys[54] ? VTERM_MOD_SHIFT : 0) | (keys[29] || keys[97] ? VTERM_MOD_CTRL : 0) | (keys[56] || keys[100] ? VTERM_MOD_ALT : 0));
    if (e.type == gui_text) {
        if (scroll) { scroll = 0; redraw(); }
        if (e.value == '\t' && keys[15]) vterm_keyboard_key(vt, VTERM_KEY_TAB, mods);
        else vterm_keyboard_unichar(vt, e.value, VTermModifier(mods & VTERM_MOD_ALT));
        return;
    }
    if (e.type == gui_pointer) {
        vterm_mouse_move(vt, std::clamp(e.y / 16, 0, rows - 1), std::clamp(e.x / 8, 0, columns - 1), mods);
        if (e.code >= 272 && e.code <= 274) vterm_mouse_button(vt, e.code == 272 ? 1 : e.code == 273 ? 3 : 2, e.value != 0, mods);
        if (e.code == 8) {
            if (mouse_mode) for (int64_t n = std::min<int64_t>(std::abs(int64_t(e.value)), 32); n; --n) vterm_mouse_button(vt, e.value > 0 ? 4 : 5, true, mods);
            else { scroll = std::clamp<int64_t>(int64_t(scroll) + int64_t(e.value) * 3, 0, history.size()); redraw(); }
        }
        return;
    }
    if (e.type != gui_key || e.code < 0 || size_t(e.code) >= keys.size()) return;
    keys[e.code] = e.value != 0; if (!e.value) return;
    if ((keys[42] || keys[54]) && (e.code == 104 || e.code == 109)) { scroll = std::clamp<int64_t>(int64_t(scroll) + (e.code == 104 ? rows / 2 : -rows / 2), 0, history.size()); redraw(); return; }
    VTermKey key = VTERM_KEY_NONE;
    switch (e.code) {
    case 103: key = VTERM_KEY_UP; break; case 108: key = VTERM_KEY_DOWN; break;
    case 105: key = VTERM_KEY_LEFT; break; case 106: key = VTERM_KEY_RIGHT; break;
    case 102: key = VTERM_KEY_HOME; break; case 107: key = VTERM_KEY_END; break;
    case 104: key = VTERM_KEY_PAGEUP; break; case 109: key = VTERM_KEY_PAGEDOWN; break;
    case 110: key = VTERM_KEY_INS; break; case 111: key = VTERM_KEY_DEL; break;
    case 96: key = VTERM_KEY_ENTER; break; case 88: key = VTermKey(VTERM_KEY_FUNCTION(12)); break;
    default: if (e.code >= 59 && e.code <= 68) key = VTermKey(VTERM_KEY_FUNCTION(e.code - 58)); break;
    }
    if (key != VTERM_KEY_NONE) vterm_keyboard_key(vt, key, mods);
}
int main() {
    ubus_conn parent, files, launcher, controller;
    ubus_msg setup, q, r;
    auto status = parent.bind(_startup().bootstrap); if (status != STATUS_OK) return status;
    status = parent.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    auto config = setup.data["message"];
    ubus_msg capacity;
    capacity.data.put("action", "lend").put("bytes", config["pixels"]).put("caps", uint64_t{64});
    status = parent.invoke("Resource", std::move(capacity), r); if (status != STATUS_OK) return status;
    auto budget = r.take(r.data["quota"]); if (!budget) return STATUS_PEER_FAULT;
    vt = vterm_new(rows, columns); if (!vt) return STATUS_NO_MEMORY;
    vterm_set_utf8(vt, 1); screen = vterm_obtain_screen(vt); vterm_screen_enable_altscreen(screen, 1);
    static const VTermScreenCallbacks callbacks{
        .damage=dirty, .moverect=nullptr,
        .movecursor=[](VTermPos pos, VTermPos old, int visible, void*) { cursor_pos = pos; cursor_visible = visible; dirty({old.row, old.row + 1, old.col, old.col + 1}, nullptr); dirty({pos.row, pos.row + 1, pos.col, pos.col + 1}, nullptr); return 1; },
        .settermprop=[](VTermProp prop, VTermValue* value, void*) { if (prop == VTERM_PROP_CURSORVISIBLE) { cursor_visible = value->boolean; redraw(); } else if (prop == VTERM_PROP_MOUSE) mouse_mode = value->number; return 1; },
        .bell=nullptr, .resize=nullptr,
        .sb_pushline=[](int cols, const VTermScreenCell* cells, void*) { if (history.size() == 512) history.pop_front(); history.emplace_back(cells, cells + cols); return 1; },
        .sb_popline=[](int cols, VTermScreenCell* cells, void*) { if (history.empty()) return 0; std::fill_n(cells, cols, VTermScreenCell{}); std::copy_n(history.back().begin(), std::min<size_t>(cols, history.back().size()), cells); history.pop_back(); return 1; },
        .sb_clear=[](void*) { history.clear(); scroll = 0; return 1; }};
    vterm_screen_set_callbacks(screen, &callbacks, nullptr); vterm_screen_set_damage_merge(screen, VTERM_DAMAGE_ROW);
    vterm_screen_reset(screen, 1); vterm_output_set_callback(vt, output, nullptr);
    _ch_pair in, out;
    status = in.open(_startup().alloc(), 16, 3); if (status == STATUS_OK) status = out.open(_startup().alloc(), 16, 3); if (status != STATUS_OK) return status;
    ubus_rx from_shell; status = from_shell.open(out.ends[0].selector(), &_events()); if (status != STATUS_OK) return status;
    const auto sender = _channel_mint(in.ends[0].selector(), _startup().cspace, 1, RIGHT_SEND); if (sender.status != STATUS_OK) return sender.status;
    _cap_owner to_shell{sender.value};
    const auto sink = _channel_mint(out.ends[1].selector(), _startup().cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
    if (sink.status != STATUS_OK) return sink.status;
    _cap_owner from_child{sink.value};
    q.data.put("name", "exec"); status = parent.invoke("Get", std::move(q), r);
    if (status == STATUS_OK) status = files.bind(r.capability(r.data["connector"]), "io.files", 1, 1);
    if (status != STATUS_OK) return status;
    _vm_area source;
    mgr owner{[&](mgr_child& child) -> std::expected<_proc_image, status_t> {
        ubus_msg q, r; q.data.put("path", child.program()).put("perms", word_t{VM_READ | VM_EXECUTE});
        auto s = files.invoke("Map", std::move(q), r); if (s != STATUS_OK) return std::unexpected(s);
        const auto b = r.data["buffer"].as<ubus_buffer>(); if (!b) return std::unexpected(STATUS_PEER_FAULT);
        return _proc_image::map(source, r.take(r.data["buffer"]), b->offset, b->size, VM_READ | VM_EXECUTE);
    }};
    status = owner.open(); if (status == STATUS_OK) status = owner.attach(parent); if (status != STATUS_OK) return status;
    ubus_msg recipe;
    recipe.data.put("name", "shell").put("program", "/boot/shell.elf").put("resources", config["shell"]["resources"]);
    auto payload = setup.slice(config["shell"]); if (!payload) return payload.error();
    for (const auto& [name, cap, rights] : std::array{
        std::tuple{"stdin", in.ends[1].selector(), word_t{RIGHT_RECEIVE | RIGHT_DUPLICATE}},
        std::tuple{"stdout", from_child.selector(), word_t{RIGHT_SEND | RIGHT_DUPLICATE}}}) {
        const auto idx = payload->attach(cap, rights); if (!idx) return idx.error(); payload->data.cap(name, *idx);
    }
    payload->data.put("rows", uint64_t(rows)).put("columns", uint64_t(columns));
    ubus_data routes; routes.put("*", ubus_data{}.put("parent", true));
    recipe.data.put("routes", routes).put("exports", ubus_data{ubus_type::Array}.push("*"));
    recipe.data.put("setup", payload->data); payload->data = std::move(recipe.data);
    auto child = owner.configure(std::move(*payload)); if (!child) return child.error();
    status = owner.start(**child); if (status != STATUS_OK) return status;
    ubus_tx tx; _ev_watch writable;
    ubus_conn reconnect;
    unsigned phase{}; uint64_t retry{};
    bool closing{};
    for (;;) {
        bool again{}; const auto now = _clock_now().value;
        if ((status = owner.poll()) != STATUS_OK) return status;
        if (!(*child)->task && (*child)->state != mgr_state::Loading) break;
        // Rebuild only the graphical window; VT, pipes and shell remain alive.
        if (!geometry.window && now >= retry) {
            if (!phase) { status = reconnect.begin_bind(_startup().bootstrap); if (status == STATUS_OK) phase = 1; }
            else if (phase == 1) { status = reconnect.poll_bind(); if (status == STATUS_OK) { q = {}; q.data.put("name", "gui"); status = reconnect.begin("Get", std::move(q)); if (status == STATUS_OK) phase = 2; } }
            else if (phase == 2) { status = reconnect.poll(r); if (status == STATUS_OK) { status = r.status; if (status == STATUS_OK) { status = launcher.begin_bind(r.capability(r.data["connector"]), "gui.launcher", 1, 1); if (status == STATUS_OK) phase = 3; } } }
            else if (phase == 3) {
                status = launcher.poll_bind();
                if (status == STATUS_OK) {
                    ubus_msg request;
                    const auto index = request.attach(budget.selector(), RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
                    if (!index) return index.error();
                    request.data.cap("budget", *index);
                    status = launcher.begin("Open", std::move(request)); if (status == STATUS_OK) phase = 4;
                }
            }
            else {
                status = launcher.poll(r);
                if (status == STATUS_OK) {
                    status = r.status;
                    if (status == STATUS_OK) status = controller.bind(r.capability(r.data["control"]), "gui.control", 1, 1);
                    if (status == STATUS_OK) status = gui.open(r.capability(r.data["client"]));
                    if (status == STATUS_OK) { auto g = gui.create("Terminal", columns * 8, rows * 16); if (g) { geometry = *g; resize(); } else status = g.error(); }
                }
            }
            if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) { (void)gui.close(); (void)controller.close(); (void)launcher.close(); (void)reconnect.close(); phase = 0; retry = now + _startup().frequency / 4; }
        }
        for (size_t n = 0; geometry.window && n < 64; ++n) {
            gui_event e; status = gui.poll(e); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) { (void)gui.close(); pixels.clear(); geometry = {}; frame_pending = false; phase = 0; retry = now + _startup().frequency / 4; keys.reset(); break; }
            if (e.type == gui_configure) { geometry = e; resize(); }
            else if (e.type == gui_release) { for (auto& p : pixels) if (p.buffer.id == uint32_t(e.code) && p.buffer.commit == e.serial) p.buffer.busy = false; }
            else if (e.type == gui_frame) frame_pending = false;
            else if (e.type == gui_close && !closing) {
                status = owner.action(**child, "stop"); if (status != STATUS_OK) return status;
                closing = true;
            }
            else input(e);
            again = true;
        }
        if (resize_event) {
            status = ubus_send(to_shell.selector(), *resize_event, false);
            if (status == STATUS_WOULD_BLOCK) {
                status = writable ? writable.arm().status : writable.open(&_events(), to_shell.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
                if (status != STATUS_OK) return status;
            } else if (status == STATUS_OK) resize_event.reset();
            else return status;
        }
        for (size_t n = 0; typed.size() < 24 && n < 8; ++n) {
            ubus_frame f; status = ubus_recv(from_shell, f); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) { if (status == STATUS_PEER_CLOSED || status == STATUS_CLOSED) break; return status; }
            // Native console streams use LF. Apply the usual terminal output
            // CR/LF processing before the VT parser, including split frames.
            size_t start{};
            for (size_t i = 0; i < f.size; ++i) if (f.data[i] == '\n') {
                vterm_input_write(vt, f.data + start, i - start);
                vterm_input_write(vt, "\r\n", 2); start = i + 1;
            }
            vterm_input_write(vt, f.data + start, f.size - start); again = true;
        }
        vterm_screen_flush_damage(screen);
        while (!typed.empty()) {
            status = tx.send(to_shell.selector(), typed.front(), false);
            if (status == STATUS_WOULD_BLOCK) {
                if (!tx.active()) { status = writable ? writable.arm().status : writable.open(&_events(), to_shell.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE); if (status != STATUS_OK) return status; }
                break;
            }
            if (status != STATUS_OK) return status;
            typed.pop_front(); again = true;
        }
        if (typed.empty() && !resize_event) (void)writable.close();
        if (geometry.window) {
            for (auto i = pixels.begin(); i != pixels.end();) {
                if (!i->buffer.busy && (i->buffer.width != uint32_t(geometry.width) || i->buffer.height != uint32_t(geometry.height))) {
                    status = gui.destroy(geometry.window, i->buffer); if (status != STATUS_OK) break; i = pixels.erase(i);
                } else ++i;
            }
            size_t count{}; for (auto& p : pixels) count += p.buffer.width == uint32_t(geometry.width) && p.buffer.height == uint32_t(geometry.height);
            while (count < 2 && pixels.size() < 4) {
                auto b = gui.allocate(geometry.window, geometry.width, geometry.height); if (!b) break;
                pixels.push_back({std::move(*b), {0, 0, geometry.width, geometry.height}}); ++count;
            }
            for (auto& p : pixels) if (content_dirty && !frame_pending && !p.buffer.busy && !p.dirty.empty() && p.buffer.width == uint32_t(geometry.width) && p.buffer.height == uint32_t(geometry.height)) {
                draw(p); status = gui.commit(geometry.window, geometry.serial, p.buffer, p.dirty);
                if (status == STATUS_OK) { p.dirty = {}; content_dirty = false; frame_pending = true; }
                else if (status != STATUS_BUSY && status != STATUS_STALE) { (void)gui.close(); pixels.clear(); geometry = {}; frame_pending = false; phase = 0; retry = now + _startup().frequency / 4; }
                again = true; break;
            }
        }
        if (!again) { auto due = owner.deadline(); if (!geometry.window && !phase && (!due || retry < due)) due = std::max(now + 1, retry); status = _ev_wait(&_events(), due).status; if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status; }
    }
    (void)gui.close(); vterm_free(vt); return closing ? STATUS_OK : (*child)->result;
}
