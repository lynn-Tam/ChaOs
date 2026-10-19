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
auto ft_terminal::open(const rt_env& env) -> status_t {
    if (ft_active) return STATUS_BUSY;
    const auto status = client_.open(env);
    if (status != STATUS_OK) return status;
    port_ = env.events;
    ft_active = this;
    auto quirks = ftxui::Terminal::GetQuirks();
    quirks.SetColorSupport(ftxui::Terminal::Palette16);
    quirks.SetComponentAscii(true);
    ftxui::Terminal::SetQuirks(quirks);
    animated_ = sys::clock_now().value;
    return STATUS_OK;
}
auto ft_terminal::wait(uint64_t deadline) -> status_t {
    return events_.empty() ? ev_wait(port_, deadline).status : STATUS_OK;
}
auto ft_terminal::read() -> std::expected<std::optional<ftxui::Event>, status_t> {
    const auto now = sys::clock_now().value;
    bool received{};
    // Bound one input batch; leave further bytes with the terminal protocol.
    for (size_t i = 0; i < 64 && events_.empty(); ++i) {
        const auto byte = client_.read(0, false);
        if (byte == STATUS_WOULD_BLOCK) break;
        if (byte < 0) return std::unexpected(byte);
        if (byte == 12) { frame_.clear(); redraw_ = true; continue; }
        received = true;
        input_at_ = rt_after_ms(50).value_or(UINT64_MAX);
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
void ft_terminal::animate() {
    if (!animate_at_) animate_at_ = rt_after_ms(16).value_or(0);
}
auto ft_terminal::present(const ftxui::Component& component) -> status_t {
    if (!focused()) return STATUS_OK;
    if (animate_at_ && sys::clock_now().value >= animate_at_) {
        animate_at_ = 0;
        ftxui::animation::Params params{ftxui::animation::Duration{float(sys::clock_now().value - animated_) / float(sys::clock_frequency().value)}};
        animated_ = sys::clock_now().value;
        component->OnAnimation(params);
    }
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
    const auto status = client_.write(output);
    if (status == STATUS_OK) frame_ = std::move(frame);
    return status;
}
void ft_print(std::string_view text) {
    if (!ft_active) sys::exit(STATUS_INVALID_OP);
    ft_active->print(text);
}
// These are the upstream standalone-component semantics. Native dispatch and
// animation scheduling belong to the explicit terminal session above.
void ftxui::App::PostEventOrExecute(ftxui::Closure closure) { if (closure) closure(); }
void ftxui::animation::RequestAnimationFrame() { if (ft_active) ft_active->animate(); }
