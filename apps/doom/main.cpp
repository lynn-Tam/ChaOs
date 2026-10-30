#include <gui/gui.hpp>
#include <posix.hpp>
#include <thread.hpp>
#include <unistd.h>
#include <cstdio>
#include <bitset>
#include <deque>
#include <doomgeneric.h>
extern "C" {
#include <doomkeys.h>
#include <i_system.h>
}

static gui_client gui;
static gui_event geometry;
static std::vector<gui_buffer> buffers;
static std::bitset<768> physical;
static std::array<uint16_t, 256> counts;
static std::bitset<256> delivered;
static std::deque<std::pair<unsigned char, bool>> keys;
static bool frame_pending, closing;
static auto translate(unsigned code) -> unsigned char {
    static constexpr char plain[] = "\0\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./";
    if (code == 29 || code == 97) return KEY_RCTRL;
    if (code == 42 || code == 54) return KEY_RSHIFT;
    if (code == 56 || code == 100) return KEY_RALT;
    if (code == 57) return ' ';
    if (code == 103) return KEY_UPARROW;
    if (code == 108) return KEY_DOWNARROW;
    if (code == 105) return KEY_LEFTARROW;
    if (code == 106) return KEY_RIGHTARROW;
    if (code == 14) return KEY_BACKSPACE;
    if (code >= 59 && code <= 68) return KEY_F1 + code - 59;
    return code < sizeof(plain) - 1 ? plain[code] : 0;
}
static void reset() {
    keys.clear(); physical.reset(); counts.fill(0);
    for (unsigned c = 0; c < delivered.size(); ++c) if (delivered[c]) keys.emplace_back(c, false);
}
static void pump() {
    for (unsigned n = 0; n < 64; ++n) {
        gui_event e; const auto status = gui.poll(e); if (status == STATUS_WOULD_BLOCK) break;
        if (status != STATUS_OK) { fputs("doom: graphical session lost\n", stderr); exit(1); }
        if (e.type == gui_configure) geometry = e;
        else if (e.type == gui_release) { for (auto& b : buffers) if (b.id == uint32_t(e.code) && b.commit == e.serial) b.busy = false; }
        else if (e.type == gui_frame) frame_pending = false;
        else if (e.type == gui_close) closing = true;
        else if (e.type == gui_reset || (e.type == gui_focus && !e.value)) reset();
        else if (e.type == gui_key && e.code >= 0 && size_t(e.code) < physical.size() && e.value != 2) {
            const bool down = e.value != 0; if (physical[e.code] == down) continue;
            physical[e.code] = down; const auto code = translate(e.code); if (!code) continue;
            if (keys.size() >= 768) { reset(); continue; }
            if (down) { if (!counts[code]++) keys.emplace_back(code, true); }
            else if (counts[code] && !--counts[code]) keys.emplace_back(code, false);
        }
    }
    if (closing) I_Quit();
}
extern "C" void DG_Init() {
    auto g = gui.create("Doom", DOOMGENERIC_RESX, DOOMGENERIC_RESY, gui_resizable | gui_keep_aspect);
    if (!g) { fprintf(stderr, "doom: create window: %d\n", g.error()); exit(1); }
    geometry = *g;
}
extern "C" void DG_DrawFrame() {
    pump();
    if (frame_pending) return;
    while (buffers.size() < 2) {
        auto b = gui.allocate(geometry.window, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
        if (!b) I_Error("Pixel allocation failed: %d", b.error());
        buffers.push_back(std::move(*b));
    }
    for (auto& b : buffers) if (!b.busy) {
        ::memcpy(reinterpret_cast<void*>(b.pixels.address), DG_ScreenBuffer, size_t(b.width) * b.height * 4);
        const auto status = gui.commit(geometry.window, geometry.serial, b);
        if (status == STATUS_STALE || status == STATUS_BUSY) return;
        if (status != STATUS_OK) I_Error("Pixel commit failed: %d", status);
        frame_pending = true; return;
    }
}
extern "C" void DG_SleepMs(uint32_t ms) {
    const auto due = _after(std::chrono::milliseconds{ms}).value_or(UINT64_MAX);
    do { pump(); if (_clock_now().value >= due) break; (void)_ev_wait(&_events(), due); } while (true);
}
extern "C" uint32_t DG_GetTicksMs() { return _clock_ns() / 1000000; }
extern "C" int DG_GetKey(int* pressed, unsigned char* key) {
    pump(); if (keys.empty()) return 0;
    *key = keys.front().first; *pressed = keys.front().second; delivered[*key] = *pressed; keys.pop_front(); return 1;
}
extern "C" void DG_SetWindowTitle(const char* title) { if (geometry.window) (void)gui.configure(geometry.window, title); }
int main(int argc, char** argv) {
    ubus_conn parent; ubus_msg setup;
    auto status = parent.bind(_startup().bootstrap); if (status != STATUS_OK) return status;
    status = parent.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    status = gui.open(setup.capability(setup.data["message"]["gui"])); if (status != STATUS_OK) return status;
    status = px_init(&setup); if (status != STATUS_OK) return status;
    (void)px_mkdir("doom");
    if (chdir("doom") != 0) { perror("doom: writable data directory"); return 1; }
    std::atexit([] { (void)gui.close(); });
    std::vector<char*> args{argv[0], const_cast<char*>("-nogui")};
    if (std::none_of(argv + 1, argv + argc, [](const char* arg) { return std::string_view{arg} == "-iwad"; }))
        args.insert(args.end(), {const_cast<char*>("-iwad"), const_cast<char*>("/boot/doom.wad")});
    for (int n = 1; n < argc; ++n) args.push_back(argv[n]);
    args.push_back(nullptr); doomgeneric_Create(args.size() - 1, args.data());
    for (;;) doomgeneric_Tick();
}
