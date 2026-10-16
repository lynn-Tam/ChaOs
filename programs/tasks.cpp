#include <protocols/manage.hpp>
#include <protocols/terminal.hpp>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

struct ts_task { mg_task info; cap_t owner; uint64_t sampled; uint32_t cpu{}; bool measured{}; };
static constexpr std::array<const char*, 8> ts_states{
    "Stopped", "Draining", "Starting", "Running", "Failed", "Suspending", "Suspended", "Done"};
template<class... A> static auto ts_line(std::string_view format, const A&... args) -> std::string {
    libk::fmt::fixed_buffer<160> text;
    (void)libk::fmt::format_to(text, format, args...);
    return std::string{text.view()};
}
static auto ts_memory(uint64_t bytes) -> std::string { return bytes == UINT64_MAX ? "-" : std::to_string(bytes / 1024); }

class ts_view {
    const rt_env& env_;
    tm_client terminal_;
    rpc_pending rpc_;
    std::array<cap_t, 2> owners_;
    std::vector<ts_task> tasks_, incoming_;
    std::vector<mg_device> devices_, device_scan_;
    std::vector<CapInfo> caps_, cap_scan_;
    std::vector<mg_dependency> dependencies_, dependency_scan_;
    std::optional<ts_task> authority_;
    struct action { cap_t endpoint; msg_frame message; };
    std::optional<action> action_;
    bool acting_{}, scanning_{}, scan_dependencies_{}, redraw_{true};
    unsigned page_{}, scan_page_{}, owner_{};
    size_t selected_{}, top_{};
    uint64_t cursor_{}, refresh_{};
    std::string notice_{"Authorized owner snapshots; one CPU = 100%"};
    std::vector<std::string> previous_;
    int escape_{};

    void begin_scan() {
        scan_page_ = page_; cursor_ = 0; owner_ = 0; scanning_ = true;
        incoming_.clear(); device_scan_.clear(); cap_scan_.clear();
        dependency_scan_.clear(); scan_dependencies_ = scan_page_ == 2;
    }
    void complete_scan() {
        if (scan_page_ == 0) tasks_ = std::move(incoming_);
        if (scan_page_ == 1) devices_ = std::move(device_scan_);
        if (scan_page_ == 2) { caps_ = std::move(cap_scan_); dependencies_ = std::move(dependency_scan_); }
        scanning_ = false; redraw_ = true;
        refresh_ = rt_after_ms(250).value_or(0);
        const auto count = page_ == 0 ? tasks_.size() : page_ == 1 ? devices_.size() : caps_.size() + dependencies_.size();
        selected_ = count ? std::min(selected_, count - 1) : 0;
    }
    void query() {
        if (rpc_.active()) return;
        if (action_) {
            acting_ = true;
            const auto status = rpc_.begin(action_->endpoint, env_, action_->message);
            if (status != STATUS_OK) { notice_ = ts_line("request error {}", status); action_.reset(); acting_ = false; }
            return;
        }
        if (!scanning_) {
            if (sys::clock_now().value < refresh_) return;
            begin_scan();
        }
        cap_t endpoint{};
        msg_frame request;
        if (scan_page_ == 0) {
            while (owner_ < owners_.size() && !owners_[owner_]) ++owner_;
            if (owner_ == owners_.size()) { complete_scan(); return; }
            endpoint = owners_[owner_]; request = mg_request(mg_op::Tasks, cursor_);
        } else if (scan_page_ == 1) {
            endpoint = env_.selector(mg_devices); request = mg_request(mg_op::Devices, cursor_);
            if (!endpoint) { notice_ = "Device owner unavailable"; complete_scan(); return; }
        } else {
            if (!authority_) { complete_scan(); return; }
            endpoint = authority_->owner;
            request = mg_request(scan_dependencies_ ? mg_op::Dependencies : mg_op::Caps,
                authority_->info.id, authority_->info.incarnation, cursor_);
        }
        request.deadline = rt_after_ms(200).value_or(0);
        const auto status = rpc_.begin(endpoint, env_, request);
        if (status != STATUS_OK) { notice_ = ts_line("owner error {}", status); complete_scan(); }
    }
    void reply() {
        if (!rpc_.active()) return;
        msg_frame message;
        const auto status = rpc_.poll(message);
        if (status == STATUS_WOULD_BLOCK) return;
        if (acting_) {
            notice_ = status == STATUS_OK ? "Accepted; observing actual state"
                : status == STATUS_STALE ? "Stale target; refresh without replay" : ts_line("action error {}", status);
            action_.reset(); acting_ = false; scanning_ = false; refresh_ = 0; redraw_ = true;
            if (status == STATUS_STALE) { authority_.reset(); page_ = 0; previous_.clear(); }
            return;
        }
        if (status != STATUS_OK) {
            if (status != STATUS_NOT_FOUND) notice_ = ts_line("owner snapshot error {}", status);
            if (scan_page_ == 2 && scan_dependencies_ && status == STATUS_NOT_FOUND) {
                scan_dependencies_ = false; cursor_ = 0; return;
            }
            if (status == STATUS_STALE) { authority_.reset(); page_ = 0; previous_.clear(); }
            if (scan_page_ == 0 && ++owner_ < owners_.size()) cursor_ = 0;
            else complete_scan();
            return;
        }
        if (scan_page_ == 0 && message.size == sizeof(mg_task)) {
            mg_task info; ::memcpy(&info, message.data, sizeof(info));
            ts_task row{info, owners_[owner_], message.deadline};
            for (const auto& old : tasks_)
                if (old.owner == row.owner && old.info.id == info.id && old.info.incarnation == info.incarnation
                    && old.info.runtime != UINT64_MAX && info.runtime != UINT64_MAX && row.sampled > old.sampled
                    && info.runtime >= old.info.runtime) {
                    row.cpu = (info.runtime - old.info.runtime) * 1000 / (row.sampled - old.sampled); row.measured = true;
                }
            incoming_.push_back(row); cursor_ = info.id;
        } else if (scan_page_ == 1 && message.size == sizeof(mg_device)) {
            mg_device device; ::memcpy(&device, message.data, sizeof(device));
            device_scan_.push_back(device); cursor_ = device.id;
        } else if (scan_page_ == 2 && scan_dependencies_ && message.size == sizeof(mg_dependency)) {
            mg_dependency edge; ::memcpy(&edge, message.data, sizeof(edge)); dependency_scan_.push_back(edge);
            cursor_ = message.value;
        } else if (scan_page_ == 2 && !scan_dependencies_ && message.size == sizeof(CapInfo)) {
            CapInfo cap; ::memcpy(&cap, message.data, sizeof(cap)); cap_scan_.push_back(cap);
            cursor_ = message.value;
            if (!cursor_) complete_scan();
        } else { notice_ = "Invalid owner reply"; complete_scan(); }
    }
    void submit(cap_t owner, msg_frame request) {
        if (action_) { notice_ = "Action pending"; return; }
        request.deadline = rt_after_ms(300).value_or(0);
        action_ = action{owner, request}; notice_ = "Submitting"; redraw_ = true;
    }
    void key(int key) {
        if (key == 27) { escape_ = 1; return; }
        if (escape_ == 1) { escape_ = key == '[' ? 2 : 0; return; }
        if (escape_ == 2) {
            escape_ = 0;
            if (key == 'A') key = 'k'; else if (key == 'B') key = 'j';
            else if (key == '5') { key = 'u'; escape_ = 3; }
            else if (key == '6') { key = 'd'; escape_ = 3; }
            else return;
        } else if (escape_ == 3) { escape_ = 0; return; }
        if (key >= '1' && key <= '3') {
            if (key == '3' && page_ == 0 && selected_ < tasks_.size()) authority_ = tasks_[selected_];
            page_ = key - '1'; selected_ = top_ = 0; refresh_ = 0; scanning_ = false; previous_.clear(); redraw_ = true; return;
        }
        const auto count = page_ == 0 ? tasks_.size() : page_ == 1 ? devices_.size() : caps_.size() + dependencies_.size();
        if (key == 'j' && selected_ + 1 < count) ++selected_;
        if (key == 'k' && selected_) --selected_;
        if (key == 'u') selected_ = selected_ > terminal_.rows() - 8 ? selected_ - (terminal_.rows() - 8) : 0;
        if (key == 'd' && count) selected_ = std::min(selected_ + terminal_.rows() - 8, count - 1);
        if (key == 12) { previous_.clear(); redraw_ = true; return; }
        if (key == 'p' || key == 'c' || key == 'x' || key == 's' || key == 'r') {
            const auto action = key == 'p' ? mg_action::Pause : key == 'c' ? mg_action::Resume
                : key == 'x' ? mg_action::Stop : key == 's' ? mg_action::Start : mg_action::Restart;
            if (page_ == 0 && selected_ < tasks_.size()) {
                const auto& target = tasks_[selected_];
                if (target.info.actions & (1U << (3 + static_cast<unsigned>(action))))
                    submit(target.owner, mg_request(mg_op::Action, target.info.id, target.info.incarnation, 0, action));
            } else if (page_ == 1 && selected_ < devices_.size()) {
                const auto& target = devices_[selected_];
                if (target.actions & (1U << (3 + static_cast<unsigned>(action))))
                    submit(env_.selector(mg_devices), mg_request(mg_op::DeviceAction, target.id, target.incarnation, 0, action));
            } else if (page_ == 2 && key == 'x' && selected_ < caps_.size() && authority_
                && (authority_->info.actions & mg_close)) {
                submit(authority_->owner, mg_request(mg_op::CloseCap, authority_->info.id,
                    authority_->info.incarnation, caps_[selected_].selector));
            }
        }
        redraw_ = true;
    }
    void draw() {
        if (!terminal_.focused() || !redraw_) return;
        std::vector<std::string> rows(terminal_.rows());
        const auto visible = rows.size() - 7;
        rows[0] = "myos tasks | 1 Tasks  2 Devices  3 Authority | q quit";
        rows[1] = "Scope: explicitly granted owners; RSS = installed RAM mappings; CPU single core";
        const auto count = page_ == 0 ? tasks_.size() : page_ == 1 ? devices_.size() : caps_.size() + dependencies_.size();
        if (selected_ < top_) top_ = selected_;
        if (selected_ >= top_ + visible) top_ = selected_ - visible + 1;
        rows[2] = page_ == 0 ? "   owner id name             state       CPU% RSS KiB charged/limit KiB"
            : page_ == 1 ? "   device     driver RID  requester IRQ state      IOSpace"
            : "   actual selector       kind    effective rights    state";
        for (size_t n = 0; n < visible && n + top_ < count; ++n) {
            const auto i = n + top_; const auto mark = i == selected_ ? '>' : ' ';
            if (page_ == 0) {
                const auto& row = tasks_[i];
                const auto cpu = row.measured ? ts_line("{}.{}", row.cpu / 10, row.cpu % 10) : std::string{"-"};
                rows[n + 3] = ts_line("{} {} {} {} {} {} {} {}/{}", mark,
                    row.owner == owners_[0] ? "root" : "jobs", row.info.id, row.info.name,
                    ts_states[std::min(row.info.state, 7U)], std::string_view{cpu},
                    std::string_view{ts_memory(row.info.resident)}, std::string_view{ts_memory(row.info.charged)}, std::string_view{ts_memory(row.info.limit)});
            } else if (page_ == 1) {
                const auto& row = devices_[i];
                rows[n + 3] = ts_line("{} {} {} {:x} {} {} {} {}", mark, row.name, row.driver, row.rid,
                    row.requester, row.irq, ts_states[std::min(row.state, 7U)], row.iospace);
            } else if (i < caps_.size()) {
                const auto& row = caps_[i];
                rows[n + 3] = ts_line("{} {:x} {} {:x} {}", mark, row.selector, row.view.kind, row.view.rights,
                    row.state == CAP_INFO_VALID ? "valid" : "unavailable");
            } else {
                const auto& edge = dependencies_[i - caps_.size()];
                rows[n + 3] = ts_line("{} dependency {} provider {} incarnation {}", mark, edge.name, edge.provider, edge.incarnation);
            }
        }
        rows[rows.size() - 4] = notice_;
        rows[rows.size() - 3] = "Arrows/j/k select | PgUp/PgDn/u/d scroll | actions below";
        uint32_t rights{};
        if (page_ == 0 && selected_ < tasks_.size()) rights = tasks_[selected_].info.actions;
        if (page_ == 1 && selected_ < devices_.size()) rights = devices_[selected_].actions;
        if (page_ == 2 && authority_ && selected_ < caps_.size()) rights = authority_->info.actions & mg_close;
        if (rights & mg_pause) rows[rows.size() - 2] += "p pause  ";
        if (rights & mg_resume) rows[rows.size() - 2] += "c continue  ";
        if (rights & mg_stop) rows[rows.size() - 2] += "x stop  ";
        if (rights & mg_start) rows[rows.size() - 2] += "s start  ";
        if (rights & mg_restart) rows[rows.size() - 2] += "r restart  ";
        if (page_ == 2 && (rights & mg_close)) rows[rows.size() - 2] += "x close slot";
        if (page_ == 0 && selected_ < tasks_.size()) {
            const auto& row = tasks_[selected_];
            rows[rows.size() - 1] = ts_line("owner-local id {} incarnation {} parent {} sampled ticks {}", row.info.id, row.info.incarnation, row.info.owner, row.sampled);
        } else if (page_ == 1 && selected_ < devices_.size()) {
            const auto& row = devices_[selected_]; rows[rows.size() - 1] = ts_line("MMIO {:x}+{} incarnation {}", row.mmio, row.bytes, row.incarnation);
        } else if (page_ == 2 && selected_ < caps_.size()) {
            const auto& view = caps_[selected_].view;
            rows[rows.size() - 1] = ts_line("limits {:x} {:x} {:x}; import/lineage origin unknown", view.words[0], view.words[1], view.words[2]);
        }
        if (page_ == 2 && selected_ >= caps_.size() && selected_ - caps_.size() < dependencies_.size()) {
            const auto& edge = dependencies_[selected_ - caps_.size()];
            rows.back() = ts_line("contract {:x} v{}.{} link {}; deployment edge, not lineage", edge.protocol, edge.major, edge.minor,
                edge.flags == 1 ? "restart" : edge.flags == 2 ? "reconnect" : edge.flags == 4 ? "pipe" : "unknown");
        }
        tm_painter paint{terminal_};
        if (previous_.empty()) paint.text("\x1b[2J\x1b[H\x1b[?25l");
        for (size_t n = 0; n < rows.size(); ++n)
            if (previous_.empty() || rows[n] != previous_[n]) {
                paint.position(n + 1, 1); paint.text("\x1b[2K");
                paint.text(std::string_view{rows[n]}.substr(0, terminal_.columns()));
            }
        paint.flush(); previous_ = std::move(rows); redraw_ = false;
    }
public:
    explicit ts_view(const rt_env& env) : env_(env), owners_{env.selector(mg_tasks), env.selector(mg_jobs)} {}
    auto run() -> status_t {
        auto status = terminal_.open(env_);
        if (status != STATUS_OK) return status;
        for (;;) {
            reply(); query(); reply();
            const auto input = terminal_.read(0, false);
            if (input == 'q') return STATUS_OK;
            if (input >= 0) key(input);
            else if (input != STATUS_WOULD_BLOCK) return input;
            draw();
            if (terminal_.status() != STATUS_OK) return terminal_.status();
            const auto next = rt_after_ms(50);
            if (next) (void)sys::notification_wait(env_.events, *next);
        }
    }
};
int main() { return ts_view{rt_startup()}.run(); }
