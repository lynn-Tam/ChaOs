#include "terminal.hpp"
#include <ranges>
#include <ftxui/component/animation.hpp>
#include <ftxui/component/app.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

static ft_terminal* ft_active;
ft_terminal::~ft_terminal() {
    if (ft_active == this) ft_active = nullptr;
}
auto ft_terminal::open(const _env& env, const ubus_msg& setup) -> status_t {
    if (ft_active) return STATUS_BUSY;
    const auto status = open_session(env, setup);
    if (status != STATUS_OK) return status;
    port_ = &_events();
    ft_active = this;
    auto quirks = ftxui::Terminal::GetQuirks();
    quirks.SetColorSupport(ftxui::Terminal::Palette16);
    quirks.SetComponentAscii(true);
    ftxui::Terminal::SetQuirks(quirks);
    return STATUS_OK;
}
auto ft_terminal::wait(uint64_t deadline) -> status_t {
    return events_.empty() ? _ev_wait(port_, deadline).status : STATUS_OK;
}
auto ft_terminal::read() -> std::expected<std::optional<ftxui::Event>, status_t> {
    const auto now = _clock_now().value;
    bool received{};
    // Bound one input batch; leave further bytes with the terminal protocol.
    for (size_t i = 0; i < 64 && events_.empty(); ++i) {
        const auto byte = read_byte(0, false);
        if (byte == STATUS_WOULD_BLOCK) break;
        if (byte < 0) return std::unexpected(byte);
        if (byte == 12) { if (const auto status = reset(); status != STATUS_OK) return std::unexpected(status); continue; }
        received = true;
        input_at_ = _after(std::chrono::milliseconds{50}).value_or(UINT64_MAX);
        parser_.Add(static_cast<char>(byte));
    }
    // Drain queued sequence bytes before expiring Escape. Rendering/RPC may
    // have delayed this consumer while the complete sequence already arrived.
    if (!received && events_.empty() && input_at_ && now >= input_at_) {
        input_at_ = 0; parser_.Timeout(50);
    }
    if (events_.empty()) return std::optional<ftxui::Event>{};
    auto event = std::move(events_.front()); events_.pop_front();
    return std::optional{std::move(event)};
}
auto ft_terminal::present(const ftxui::Component& component) -> status_t {
    if (!focused()) return STATUS_OK;
    if (!pending_.empty()) return STATUS_WOULD_BLOCK;
    ftxui::Terminal::SetFallbackSize({static_cast<int>(columns()), static_cast<int>(rows())});
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(columns()), ftxui::Dimension::Fixed(rows()));
    ftxui::Render(screen, component->Render());
    std::string frame;
    frame.reserve(rows() * columns());
    screen.ToString(frame);
    std::string output;
    if (frame_.empty()) output = "\x1b[0m\x1b[H\x1b[?25l" + frame;
    else {
        // Upstream resets style on every serialized line. A native focus/size
        // event clears frame_, so cached rows always share this viewport.
        const auto separator = std::string_view{"\r\n"};
        auto previous = std::string_view{frame_} | std::views::split(separator);
        auto old = previous.begin();
        size_t row{};
        for (auto line : std::string_view{frame} | std::views::split(separator)) {
            ++row;
            const std::string_view text{line}, before{*old++};
            if (text != before) output += "\x1b[" + std::to_string(row) + ";1H" + std::string{text};
        }
    }
    if (output.empty()) return STATUS_OK;
    pending_ = std::move(output); next_frame_ = std::move(frame);
    return STATUS_OK;
}
auto ft_terminal::flush() -> status_t {
    for (size_t budget = 0; sent_ < pending_.size() && budget < 8; ++budget) {
        io_frame frame; frame.size = std::min(pending_.size() - sent_, io_stream_limit);
        ::memcpy(frame.data, pending_.data() + sent_, frame.size);
        const auto status = output_.send(control_.endpoint(), frame, false, *control_.token("Frame"), focus_);
        if (status == STATUS_STALE) { focused_ = false; return reset(); }
        if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
            if (output_.active()) {
                const auto status = writable_.close();
                return status == STATUS_OK ? STATUS_WOULD_BLOCK : status;
            }
            const auto armed = writable_ ? writable_.arm().status : writable_.open(port_, control_.endpoint(),
                SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
            return armed == STATUS_OK ? STATUS_WOULD_BLOCK : armed;
        }
        if (status != STATUS_OK) return status;
        sent_ += frame.size;
    }
    if (sent_ < pending_.size()) return STATUS_PENDING;
    if (!pending_.empty()) { frame_ = std::move(next_frame_); pending_.clear(); sent_ = 0; }
    return writable_.close();
}
// Upstream Screen::Print is synchronous; the native event loop uses present/flush.
void ft_terminal::print(std::string_view text) {
    for (;;) {
        const auto status = flush();
        if (status == STATUS_OK) break;
        if (status == STATUS_PENDING) continue;
        if (status != STATUS_WOULD_BLOCK) _thread_exit(status);
        if (const auto ready = wait(); ready != STATUS_OK) _thread_exit(ready);
    }
    frame_.clear(); (void)write(text);
}
void ft_print(std::string_view text) {
    if (!ft_active) _thread_exit(STATUS_INVALID_OP);
    ft_active->print(text);
}
// Standalone components execute closures directly; this session has no animations.
void ftxui::App::PostEventOrExecute(ftxui::Closure closure) { if (closure) closure(); }
void ftxui::animation::RequestAnimationFrame() {}
