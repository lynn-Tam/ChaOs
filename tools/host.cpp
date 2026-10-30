#include <gui/host_protocol.hpp>
#include <SDL.h>
#include <atomic>
#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <cuchar>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// SDL owns windows on its main thread. The worker owns the socket and partial
// packets; queues are bounded and an eventfd/SDL event carry wakeups, not pixels.
class gh_link {
    static constexpr size_t limit = 512;
    int listener_, wake_;
    uint32_t event_;
    std::mutex mutex_;
    std::deque<gui_host_packet> input_, output_;
    bool broken_{};
    std::jthread worker_;
    void notify() { SDL_Event e{}; e.type = event_; (void)SDL_PushEvent(&e); }
    void wake() { while (eventfd_write(wake_, 1) < 0) { if (errno == EINTR) continue; if (errno != EAGAIN) std::abort(); break; } }
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            pollfd accept[]{{listener_, POLLIN, 0}, {wake_, POLLIN, 0}};
            if (::poll(accept, 2, -1) < 0) { if (errno == EINTR) continue; return; }
            if (stop.stop_requested()) break;
            if (accept[1].revents) { uint64_t value; if (eventfd_read(wake_, &value) < 0 && errno != EAGAIN && errno != EINTR) break; }
            if (!(accept[0].revents & POLLIN)) continue;
            const int fd = accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) continue;
            gui_host_reader reader;
            gui_host_packet sent{};
            size_t writing{sizeof(sent)};
            for (;;) {
                bool read, write, broken;
                { std::lock_guard lock{mutex_}; read = input_.size() < limit; write = writing != sizeof(sent) || !output_.empty(); broken = broken_; }
                if (stop.stop_requested() || broken) break;
                pollfd p[]{{fd, short((read ? POLLIN : 0) | (write ? POLLOUT : 0)), 0}, {wake_, POLLIN, 0}};
                if (::poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
                if (p[1].revents) { uint64_t value; if (eventfd_read(wake_, &value) < 0 && errno != EAGAIN && errno != EINTR) break; }
                if (p[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
                if (p[0].revents & POLLIN) {
                    const auto n = ::recv(fd, reader.tail().data(), reader.tail().size(), 0);
                    if (!n || (n < 0 && errno != EAGAIN && errno != EINTR)) break;
                    if (n > 0 && reader.advance(n)) {
                        bool empty;
                        { std::lock_guard lock{mutex_}; empty = input_.empty(); input_.push_back(reader.packet); }
                        if (empty) notify();
                    }
                }
                if (p[0].revents & POLLOUT) {
                    if (writing == sizeof(sent)) {
                        std::lock_guard lock{mutex_};
                        if (!output_.empty()) { sent = output_.front(); output_.pop_front(); writing = 0; }
                    }
                    if (writing != sizeof(sent)) {
                        const auto n = ::send(fd, reinterpret_cast<const char*>(&sent) + writing, sizeof(sent) - writing, MSG_NOSIGNAL);
                        if (n < 0 && errno != EAGAIN && errno != EINTR) break;
                        if (n > 0) writing += n;
                    }
                }
            }
            ::close(fd);
            { std::lock_guard lock{mutex_}; input_.clear(); output_.clear(); broken_ = false; gui_host_packet lost{}; lost.type = 0; input_.push_back(lost); }
            notify();
        }
    }
public:
    gh_link(int listener, int wake, uint32_t event) : listener_(listener), wake_(wake), event_(event),
        worker_([this](std::stop_token stop) { run(stop); }) {}
    ~gh_link() { worker_.request_stop(); wake(); worker_.join(); ::close(wake_); }
    auto take(gui_host_packet& packet) -> bool {
        { std::lock_guard lock{mutex_}; if (input_.empty()) return false; packet = input_.front(); input_.pop_front(); }
        wake(); return true;
    }
    auto send(const gui_host_packet& packet) -> bool {
        bool accepted;
        {
            std::lock_guard lock{mutex_};
            if (!output_.empty() && packet.type == gh_input && output_.back().type == gh_input && output_.back().window == packet.window
                && output_.back().event.type == packet.event.type && (packet.event.type == gui_configure || (packet.event.type == gui_pointer && !packet.event.code && !output_.back().event.code)))
                output_.back() = packet;
            else if (output_.size() < limit) output_.push_back(packet);
            else broken_ = true;
            accepted = !broken_;
        }
        wake(); return accepted;
    }
    void disconnect() { { std::lock_guard lock{mutex_}; broken_ = true; } wake(); }
};
struct gh_view {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    uint32_t width{}, height{}, flags{};
    bool content{};
    auto present() -> int {
        SDL_Rect destination;
        if (flags & gui_keep_aspect) {
            int w, h; if (SDL_GetRendererOutputSize(renderer, &w, &h)) return -1;
            const auto rect = gui_fit({0, 0, w, h}, width, height);
            destination = {rect.x, rect.y, rect.width, rect.height};
            if (SDL_RenderClear(renderer)) return -1;
        }
        if (SDL_RenderCopy(renderer, texture, nullptr, flags & gui_keep_aspect ? &destination : nullptr)) return -1;
        SDL_RenderPresent(renderer); return 0;
    }
    ~gh_view() { if (texture) SDL_DestroyTexture(texture); if (renderer) SDL_DestroyRenderer(renderer); if (window) SDL_DestroyWindow(window); }
};
static auto gh_key(SDL_Scancode code) -> int32_t {
    static constexpr uint16_t letters[]{30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) return letters[code - SDL_SCANCODE_A];
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_0) return 2 + code - SDL_SCANCODE_1;
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F10) return 59 + code - SDL_SCANCODE_F1;
    switch (code) {
    case SDL_SCANCODE_RETURN: return 28; case SDL_SCANCODE_ESCAPE: return 1; case SDL_SCANCODE_BACKSPACE: return 14;
    case SDL_SCANCODE_TAB: return 15; case SDL_SCANCODE_SPACE: return 57; case SDL_SCANCODE_MINUS: return 12;
    case SDL_SCANCODE_EQUALS: return 13; case SDL_SCANCODE_LEFTBRACKET: return 26; case SDL_SCANCODE_RIGHTBRACKET: return 27;
    case SDL_SCANCODE_BACKSLASH: return 43; case SDL_SCANCODE_SEMICOLON: return 39; case SDL_SCANCODE_APOSTROPHE: return 40;
    case SDL_SCANCODE_GRAVE: return 41; case SDL_SCANCODE_COMMA: return 51; case SDL_SCANCODE_PERIOD: return 52;
    case SDL_SCANCODE_SLASH: return 53; case SDL_SCANCODE_CAPSLOCK: return 58;
    case SDL_SCANCODE_F11: return 87; case SDL_SCANCODE_F12: return 88; case SDL_SCANCODE_PRINTSCREEN: return 99;
    case SDL_SCANCODE_SCROLLLOCK: return 70; case SDL_SCANCODE_PAUSE: return 119;
    case SDL_SCANCODE_INSERT: return 110; case SDL_SCANCODE_HOME: return 102; case SDL_SCANCODE_PAGEUP: return 104;
    case SDL_SCANCODE_DELETE: return 111; case SDL_SCANCODE_END: return 107; case SDL_SCANCODE_PAGEDOWN: return 109;
    case SDL_SCANCODE_RIGHT: return 106; case SDL_SCANCODE_LEFT: return 105; case SDL_SCANCODE_DOWN: return 108; case SDL_SCANCODE_UP: return 103;
    case SDL_SCANCODE_LCTRL: return 29; case SDL_SCANCODE_LSHIFT: return 42; case SDL_SCANCODE_LALT: return 56;
    case SDL_SCANCODE_LGUI: return 125; case SDL_SCANCODE_RCTRL: return 97; case SDL_SCANCODE_RSHIFT: return 54;
    case SDL_SCANCODE_RALT: return 100; case SDL_SCANCODE_RGUI: return 126;
    default: return 0;
    }
}
int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: host-viewer SOCKET SHARED-FILE\n"); return 1; }
    (void)std::setlocale(LC_CTYPE, "");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) { std::fprintf(stderr, "host-viewer: %s\n", SDL_GetError()); return 1; }
    const auto event = SDL_RegisterEvents(1); if (event == uint32_t(-1)) return 1;
    const int file = ::open(argv[2], O_RDONLY | O_CLOEXEC); struct stat facts{};
    if (file < 0 || fstat(file, &facts) || facts.st_size <= 0) return 1;
    const size_t size = facts.st_size;
    const auto* pixels = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_SHARED, file, 0));
    ::close(file); if (pixels == MAP_FAILED) return 1;
    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (listener < 0 || std::strlen(argv[1]) >= sizeof(address.sun_path)) return 1;
    std::strcpy(address.sun_path, argv[1]);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 1)) { std::perror("host-viewer"); return 1; }
    const int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); if (wake < 0) return 1;
    gh_link link{listener, wake, event};
    std::map<uint64_t, std::unique_ptr<gh_view>> windows;
    uint64_t epoch{}, sequence{}; size_t capacity{};
    const auto input = [&](uint64_t window, gui_event value) {
        gui_host_packet packet; packet.type = gh_input; packet.epoch = epoch; packet.window = window;
        packet.sequence = ++sequence; packet.event = value; (void)link.send(packet);
    };
    for (;;) {
        SDL_Event e; if (!SDL_WaitEvent(&e)) return 1;
        if (e.type == event) {
            gui_host_packet packet;
            while (link.take(packet)) {
                if (!packet.type) { windows.clear(); epoch = sequence = 0; continue; }
                if (packet.type == gh_hello) {
                    windows.clear(); epoch = packet.epoch; sequence = 0; capacity = packet.flags;
                    if (!epoch || !capacity || capacity > 512 || packet.offset != size) { link.disconnect(); continue; }
                    SDL_Rect display{}; if (SDL_GetDisplayBounds(0, &display) < 0) { display.w = 1920; display.h = 1080; }
                    packet.flags = gh_hello; packet.type = gh_ack; packet.width = display.w; packet.height = display.h; (void)link.send(packet); continue;
                }
                if (!epoch || packet.epoch != epoch) continue;
                auto found = windows.find(packet.window);
                if (packet.type == gh_window) {
                    if (!packet.window || !gui_bytes(packet.width, packet.height) || packet.width > INT32_MAX || packet.height > INT32_MAX) { link.disconnect(); continue; }
                    if (found == windows.end()) {
                        if (windows.size() >= capacity) { link.disconnect(); continue; }
                        auto window = std::make_unique<gh_view>();
                        window->window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, packet.width, packet.height,
                            SDL_WINDOW_HIDDEN | ((packet.flags & gui_resizable) ? SDL_WINDOW_RESIZABLE : 0));
                        if (window->window) window->renderer = SDL_CreateRenderer(window->window, -1, 0);
                        if (!window->renderer) { std::fprintf(stderr, "host-viewer: %s\n", SDL_GetError()); link.disconnect(); continue; }
                        SDL_SetWindowMinimumSize(window->window, packet.limits.min_width, packet.limits.min_height);
                        SDL_SetWindowMaximumSize(window->window, std::min<uint32_t>(packet.limits.max_width, INT32_MAX), std::min<uint32_t>(packet.limits.max_height, INT32_MAX));
                        found = windows.emplace(packet.window, std::move(window)).first;
                    }
                    auto& w = *found->second;
                    std::string title{packet.title, strnlen(packet.title, sizeof(packet.title))}; SDL_SetWindowTitle(w.window, title.c_str());
                    const bool changed = bool(SDL_GetWindowFlags(w.window) & SDL_WINDOW_FULLSCREEN) != bool(packet.flags & gui_fullscreen);
                    if (changed) (void)SDL_SetWindowFullscreen(w.window, packet.flags & gui_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    const auto full = SDL_GetWindowFlags(w.window) & SDL_WINDOW_FULLSCREEN ? uint32_t(gui_fullscreen) : 0U;
                    // Existing geometry belongs to the host. Echoing a delayed
                    // Configure must not overwrite a newer interactive resize.
                    if (changed) {
                        int width, height; SDL_GetWindowSize(w.window, &width, &height);
                        input(packet.window, gui_event{.type=gui_configure, .flags=full, .width=width, .height=height});
                    }
                    w.flags = (packet.flags & ~gui_fullscreen) | full;
                    if (!(w.flags & gh_visible)) SDL_HideWindow(w.window);
                    else if (w.content) SDL_ShowWindow(w.window);
                    if ((w.flags & gh_activate) && w.content) { SDL_RaiseWindow(w.window); (void)SDL_SetWindowInputFocus(w.window); w.flags &= ~gh_activate; }
                } else if (packet.type == gh_remove) {
                    if (found != windows.end()) windows.erase(found);
                    packet.flags = gh_remove; packet.type = gh_ack; (void)link.send(packet);
                } else if (packet.type == gh_present) {
                    const auto bytes = gui_bytes(packet.width, packet.height);
                    const auto rect = gui_intersect(packet.damage, {0, 0, int32_t(packet.width), int32_t(packet.height)});
                    if (found == windows.end() || !bytes || packet.stride != packet.width * 4 || packet.offset > size || *bytes > size - packet.offset || rect.empty()
                        || rect.x != packet.damage.x || rect.y != packet.damage.y || rect.width != packet.damage.width || rect.height != packet.damage.height) { link.disconnect(); continue; }
                    auto& w = *found->second;
                    bool fresh = w.width != packet.width || w.height != packet.height || !w.texture;
                    if (fresh) {
                        if (w.texture) SDL_DestroyTexture(w.texture);
                        w.texture = SDL_CreateTexture(w.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, packet.width, packet.height);
                        w.width = packet.width; w.height = packet.height;
                        if (w.texture) (void)SDL_SetTextureBlendMode(w.texture, SDL_BLENDMODE_NONE);
                    }
                    const SDL_Rect damage{fresh ? 0 : rect.x, fresh ? 0 : rect.y, fresh ? int(packet.width) : rect.width, fresh ? int(packet.height) : rect.height};
                    const auto* source = pixels + packet.offset + size_t(damage.y) * packet.stride + damage.x * 4;
                    std::atomic_thread_fence(std::memory_order_acquire);
                    int status = w.texture ? SDL_UpdateTexture(w.texture, &damage, source, packet.stride) : -1;
                    if (!status) status = w.present();
                    if (!status) {
                        w.content = true;
                        if (w.flags & gh_visible) SDL_ShowWindow(w.window);
                        if (w.flags & gh_activate) { SDL_RaiseWindow(w.window); (void)SDL_SetWindowInputFocus(w.window); w.flags &= ~gh_activate; }
                    }
                    packet.flags = gh_present; packet.type = gh_ack; packet.event.value = status ? gui_failed : gui_used; (void)link.send(packet);
                }
            }
            continue;
        }
        uint32_t id{};
        if (e.type == SDL_WINDOWEVENT) id = e.window.windowID;
        else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) id = e.key.windowID;
        else if (e.type == SDL_TEXTINPUT) id = e.text.windowID;
        else if (e.type == SDL_MOUSEMOTION) id = e.motion.windowID;
        else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) id = e.button.windowID;
        else if (e.type == SDL_MOUSEWHEEL) id = e.wheel.windowID;
        const auto found = std::ranges::find_if(windows, [&](const auto& item) { return SDL_GetWindowID(item.second->window) == id; });
        if (found == windows.end()) {
            if (e.type == SDL_QUIT) for (const auto& [window, _] : windows) input(window, gui_event{.type=gui_close});
            continue;
        }
        const auto window = found->first;
        if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) input(window, gui_event{.type=gui_close});
            else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                const bool focus = e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
                if (focus) SDL_StartTextInput(); else input(window, gui_event{.type=gui_reset});
                input(window, gui_event{.type=gui_focus, .value=focus});
            } else if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && e.window.data1 > 0 && e.window.data2 > 0)
                input(window, gui_event{.type=gui_configure, .flags=SDL_GetWindowFlags(found->second->window) & SDL_WINDOW_FULLSCREEN ? uint32_t(gui_fullscreen) : 0U, .width=e.window.data1, .height=e.window.data2});
            else if (e.window.event == SDL_WINDOWEVENT_EXPOSED && found->second->content) {
                (void)found->second->present();
            }
        } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            const auto code = gh_key(e.key.keysym.scancode);
            if (code) input(window, gui_event{.type=gui_key, .code=code, .value=e.type == SDL_KEYUP ? 0 : e.key.repeat ? 2 : 1});
        } else if (e.type == SDL_TEXTINPUT) {
            std::mbstate_t state{}; const char* text = e.text.text; size_t length = strnlen(text, sizeof(e.text.text));
            while (length) {
                char32_t value{}; const auto n = std::mbrtoc32(&value, text, length, &state);
                if (!n || n > length) break;
                input(window, gui_event{.type=gui_text, .value=int32_t(value)}); text += n; length -= n;
            }
        } else if (e.type == SDL_MOUSEMOTION) input(window, gui_event{.type=gui_pointer, .x=e.motion.x, .y=e.motion.y});
        else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
            static constexpr uint16_t buttons[]{0, 272, 274, 273, 275, 276};
            if (e.button.button < std::size(buttons)) input(window, gui_event{.type=gui_pointer, .x=e.button.x, .y=e.button.y, .code=buttons[e.button.button], .value=e.type == SDL_MOUSEBUTTONDOWN});
        } else if (e.type == SDL_MOUSEWHEEL) input(window, gui_event{.type=gui_pointer, .code=8, .value=e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y});
    }
}
