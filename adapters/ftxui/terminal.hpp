#pragma once
#include <protocols/terminal.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/terminal_input_parser.hpp>
#include <ftxui/component/event.hpp>
#include <optional>

// Explicit, single-threaded native FTXUI session. The application owns its loop.
class ft_terminal final {
    tm_client client_;
    cap_t port_{};
    std::deque<ftxui::Event> events_;
    uint64_t input_at_{}, animated_{}, animate_at_{};
    ftxui::TerminalInputParser parser_{[this](ftxui::Event e) { input_at_ = 0; events_.push_back(std::move(e)); }};
    bool redraw_{true};
    std::string frame_;
public:
    ft_terminal() = default;
    ~ft_terminal();
    auto open(const rt_env&) -> status_t;
    auto read() -> std::expected<std::optional<ftxui::Event>, status_t>;
    auto present(const ftxui::Component&) -> status_t;
    auto wait(uint64_t deadline = 0) -> status_t;
    auto focused() const -> bool { return client_.focused(); }
    auto rows() const -> size_t { return client_.rows(); }
    auto columns() const -> size_t { return client_.columns(); }
    auto redraw() -> bool { return std::exchange(redraw_, false); }
    auto deadline() const -> uint64_t {
        return input_at_ && animate_at_ ? std::min(input_at_, animate_at_) : std::max(input_at_, animate_at_);
    }
    void animate();
    void print(std::string_view text) { frame_.clear(); (void)client_.write(text); }
};
