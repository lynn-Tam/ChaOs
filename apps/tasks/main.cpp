#include "terminal.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <charconv>

static constexpr std::array<std::string_view, ObjCount> ts_kinds{
    "?", "Thread", "SC", "Domain", "CSpace", "Mem", "VSpace", "Account", "Notify",
    "Channel", "Pager", "IRQ", "IOMMU", "IOSpace", "Phys", "IRQ ctl", "Scope", "Port", "VM Op", "IO Op"};
static auto ts_text(ubus_value value) -> std::string {
    if (const auto text = value.as<std::string_view>()) return std::string{*text};
    if (const auto number = value.as<uint64_t>()) return std::to_string(*number);
    if (const auto number = value.as<int64_t>()) return std::to_string(*number);
    if (const auto flag = value.as<bool>()) return *flag ? "yes" : "no";
    if (const auto words = value.vector<uint64_t>()) {
        std::string result{"["};
        for (const auto word : *words) { if (result.size() > 1) result += ","; result += std::to_string(word); }
        return result + "]";
    }
    if (value.type() == ubus_type::Record) {
        std::string result;
        for (const auto field : value.values()) {
            if (!result.empty()) result += "  ";
            result += std::string{field.name()} + "=";
            if (field.name() == "kind") result += ts_kinds[std::min<uint64_t>(field.as<uint64_t>().value_or(0), ObjCount - 1)];
            else result += ts_text(field);
        }
        return result;
    }
    return "?";
}
struct ts_owner { std::string name; ubus_conn connection; };
struct ts_row { size_t owner; ubus_data data; };
struct ts_nav { std::string facet{"caps"}; uint64_t object{}, selector{}, selected{}; };
class ts_view {
    const rt_env& env_;
    ft_terminal terminal_;
    ubus_conn bus_;
    ubus_msg setup_;
    std::array<ts_owner, 4> owners_{{{"system", {}}, {"session", {}}, {"jobs", {}}, {"devices", {}}}};
    std::vector<ts_row> rows_, incoming_;
    std::vector<std::string> labels_, tabs_{"1 Tasks", "2 Devices", "3 Topology"};
    std::vector<size_t> visible_;
    std::optional<ts_row> target_;
    ts_nav nav_;
    std::vector<ts_nav> history_;
    ubus_data detail_;
    std::string filter_, notice_{"Native facts sampled through the owning manager"};
    int page_{}, selected_{};
    size_t scan_{};
    uint64_t refresh_{}, generation_{}, pending_generation_{};
    bool pending_{}, scanning_{}, dirty_{true}, filtering_{}, detail_next_{};
    std::optional<ubus_msg> action_;
    std::string action_method_;
    size_t action_owner_{};
    bool acting_{};
    ftxui::Component list_, input_, ui_;

    auto selected() -> ts_row* {
        return selected_ >= 0 && size_t(selected_) < visible_.size() ? &rows_[visible_[selected_]] : nullptr;
    }
    void labels() {
        labels_.clear(); visible_.clear();
        for (size_t i = 0; i < rows_.size(); ++i) {
            const auto row = rows_[i].data.view();
            auto text = ts_text(row);
            if (page_ != 2) {
                auto state = ts_text(row["state"]);
                if (!state.empty() && state[0] >= 'a' && state[0] <= 'z') state[0] -= 'a' - 'A';
                text = ts_text(row["name"]) + "  " + state + "  id=" + ts_text(row["id"])
                    + "  RAM=" + std::to_string(row["resident"].as<uint64_t>().value_or(0) / 1024) + " KiB";
            } else if (row["selector"]) {
                const auto kind = row["kind"].as<uint64_t>().value_or(0);
                text = "Slot " + ts_text(row["selector"]) + "  " + std::string{ts_kinds[std::min<uint64_t>(kind, ObjCount - 1)]}
                    + "  Grant " + ts_text(row["grant_id"]) + "  Object " + ts_text(row["object_id"])
                    + (row["state"].as<uint64_t>().value_or(0) & CAP_INFO_VALID ? "  Valid" : "  Unavailable");
            }
            if (filter_.empty() || text.find(filter_) != text.npos) { labels_.push_back(std::move(text)); visible_.push_back(i); }
        }
        selected_ = std::clamp(selected_, 0, std::max(0, int(labels_.size()) - 1)); dirty_ = true;
    }
    void refresh() { ++generation_; refresh_ = 0; scanning_ = false; detail_next_ = page_ == 2 && nav_.object; dirty_ = true; }
    auto connect(size_t owner) -> status_t {
        auto& endpoint = owners_[owner];
        if (endpoint.connection.endpoint()) return STATUS_OK;
        if (endpoint.name == "jobs") return endpoint.connection.bind(setup_.capability(setup_.data["message"]["jobs"]));
        ubus_msg request, response; request.data.put("name", endpoint.name);
        const auto status = bus_.invoke("Get", std::move(request), response);
        return status == STATUS_OK ? endpoint.connection.bind(response.capability(response.data["connector"])) : status;
    }
    void query() {
        if (pending_) return;
        const auto now = sys::clock_now().value;
        if (!action_ && !scanning_ && now < refresh_) return;
        if (!scanning_) { incoming_.clear(); scan_ = page_ == 0 ? 0 : page_ == 1 ? 3 : target_ ? target_->owner : 4; scanning_ = true; }
        if (scan_ >= owners_.size()) { rows_ = std::move(incoming_); scanning_ = false; refresh_ = rt_after_ms(500).value_or(0); labels(); return; }
        const auto owner = action_ ? action_owner_ : scan_;
        auto status = connect(owner);
        ubus_msg request; std::string_view method = "List";
        if (action_) { request = std::move(*action_); action_.reset(); method = action_method_; acting_ = true; }
        else if (page_ == 2 && target_) {
            method = "Inspect";
            request.data.put("id", target_->data["id"]).put("gen", target_->data["gen"])
                .put("facet", detail_next_ ? "info" : nav_.facet).put("object", nav_.object);
            if (nav_.selector) request.data.put("selector", nav_.selector);
            if (nav_.selected) request.data.put("selected", nav_.selected);
        }
        request.deadline = rt_after_ms(200).value_or(0);
        if (status == STATUS_OK) status = owners_[owner].connection.begin(method, std::move(request));
        if (status != STATUS_OK) { notice_ = "request failed: " + std::to_string(status); if (!acting_) advance(); else acting_ = false; dirty_ = true; return; }
        pending_ = true; pending_generation_ = generation_;
    }
    void advance() {
        if (page_ == 0 && scan_ < 2) ++scan_;
        else { rows_ = std::move(incoming_); scanning_ = false; refresh_ = rt_after_ms(500).value_or(0); labels(); }

    }
    void reply() {
        if (!pending_) return;
        const auto owner = acting_ ? action_owner_ : scan_;
        ubus_msg response;
        auto status = owners_[owner].connection.poll(response);
        if (status == STATUS_WOULD_BLOCK) return;
        pending_ = false;
        if (status == STATUS_OK) status = response.status;
        if (acting_) { acting_ = false; notice_ = "action: " + std::to_string(status); refresh(); return; }
        if (pending_generation_ != generation_) return;
        if (status != STATUS_OK) {
            notice_ = status == STATUS_STALE ? "snapshot changed; refreshing" : "request: " + std::to_string(status);
            if (status == STATUS_PEER_CLOSED || status == STATUS_CLOSED || status == STATUS_INVALID_CAP) (void)owners_[owner].connection.close();
            if (status == STATUS_STALE) refresh(); else advance();
            dirty_ = true; return;
        }
        if (detail_next_) {
            for (const auto row : response.data["items"].values()) { (void)detail_.assign(row.bytes()); break; }
            detail_next_ = false; return;
        }
        for (const auto row : response.data["items"].values()) {
            ubus_data copy;
            if (copy.assign(row.bytes())) incoming_.push_back({owner, std::move(copy)});
        }

        advance();
    }
    void navigate(ts_nav next) { history_.push_back(nav_); nav_ = std::move(next); filter_.clear(); selected_ = 0; refresh(); }
    void action(std::string method, const ts_row& task, ubus_data params = {}) {
        if (!task.data["control"].as<bool>().value_or(false)) { notice_ = "Read-only authority"; dirty_ = true; return; }
        ubus_msg request; request.data = std::move(params);
        request.data.put("id", task.data["id"]).put("gen", task.data["gen"]);
        action_.emplace(std::move(request)); action_method_ = std::move(method); action_owner_ = task.owner;
    }
    void key(const ftxui::Event& event) {
        if (filtering_) {
            if (event == ftxui::Event::Return || event == ftxui::Event::Escape) { filtering_ = false; list_->TakeFocus(); }
            else { input_->OnEvent(event); labels(); }
            dirty_ = true; return;
        }
        const auto ch = event.is_character() && event.character().size() == 1 ? event.character()[0] : 0;
        auto* row = selected();
        if (ch >= '1' && ch <= '3') {
            if (ch == '3' && page_ != 2 && row) { target_ = *row; nav_ = {page_ == 1 ? "members" : "caps"}; history_.clear(); }
            page_ = ch - '1'; filter_.clear(); selected_ = 0; refresh(); return;
        }
        if (ch == 'f' || ch == '/') { filtering_ = true; input_->TakeFocus(); dirty_ = true; return; }
        if (event == ftxui::Event::CtrlL) { refresh(); return; }
        if (page_ == 2) {
            if (ch == 'h') { if (history_.empty()) page_ = 0; else { nav_ = history_.back(); history_.pop_back(); } refresh(); return; }
            if (ch == 'v') {
                if (nav_.facet == "caps") {
                    if (!row || !row->data["selector"]) return;
                    navigate({"grants", nav_.object, *row->data["selector"].as<uint64_t>(), 0});
                } else navigate({nav_.facet == "grants" ? "members" : "caps"});
                return;
            }
            if (ch == 'l') { navigate({"dependencies"}); return; }
            if (ch == 't') { navigate({"usage", nav_.object}); return; }
            if (ch == 'a' || ch == 'w') {
                const auto object = detail_[ch == 'a' ? "account_id" : "owner_id"].as<uint64_t>();
                if (object && *object) navigate({"info", *object});
                return;
            }
            if (event == ftxui::Event::Return && row) {
                const auto object = row->data["object_id"].as<uint64_t>();
                const auto kind = row->data["kind"].as<uint64_t>().value_or(0);
                if (object && *object) navigate({kind == ObjCSpace ? "caps" : kind == ObjScope ? "members"
                    : kind == ObjAccount ? "usage" : "links", *object});
                return;
            }
            if ((ch == 'x' || ch == 'b') && row && target_) {
                ubus_data request;
                const auto selector = ch == 'x' ? row->data["selector"].as<uint64_t>() : std::optional{nav_.selector};
                if (!selector || !*selector) return;
                request.put("selector", *selector);
                if (ch == 'b') request.put("selected", row->data["grant_id"].as<uint64_t>().value_or(0));
                action(ch == 'x' ? "CloseCap" : "Revoke", *target_, std::move(request)); return;
            }
            if (ch == 'r') { refresh(); return; }
        } else if (row && (ch == 'p' || ch == 'c' || ch == 'x' || ch == 's' || ch == 'r')) {
            ubus_data request; request.put("action", ch == 'p' ? "pause" : ch == 'c' ? "resume" : ch == 'x' ? "stop" : ch == 's' ? "start" : "restart");
            action("Action", *row, std::move(request)); return;
        }
        list_->OnEvent(event); dirty_ = true;
    }
    auto render() -> ftxui::Element {
        using namespace ftxui;
        std::string title = "myos tasks | " + tabs_[page_];
        if (page_ == 2) title += " | " + (nav_.facet == "members" ? std::string{"objects"} : nav_.facet == "dependencies" ? std::string{"deployment"} : nav_.facet) + (target_ ? " | " + ts_text(target_->data["name"]) : "");
        auto footer = page_ == 2 ? "v view  Enter object  h back  a account  w owner  t usage  l deployment  x Close  b Revoke"
            : "p pause  c continue  x stop  s start  r restart";
        return vbox({text(title) | bold, separator(), filtering_ ? input_->Render() : text("/ filter  1/2/3 page  q quit") | dim,
            list_->Render() | vscroll_indicator | frame | flex, separator(),
            paragraph(selected() ? ts_text(selected()->data.view()) : ts_text(detail_.view())) | dim, text(notice_), text(footer)}) | border;
    }
public:
    explicit ts_view(const rt_env& env) : env_(env) {
        list_ = ftxui::Menu(&labels_, &selected_);
        input_ = ftxui::Input(&filter_, "filter");
        ui_ = ftxui::Renderer(ftxui::Container::Vertical({input_, list_}), [this] { return render(); });
        list_->TakeFocus();
    }
    auto run() -> status_t {
        auto status = bus_.bind(env_.environment);
        if (status == STATUS_OK) status = bus_.invoke("Receive", {}, setup_);
        if (status == STATUS_OK) status = terminal_.open(env_, setup_);
        if (status != STATUS_OK) return status;
        for (;;) {
            reply(); query(); reply();
            for (size_t count = 0; !pending_ && !scanning_ && count < 64; ++count) {
                const auto input = terminal_.read();
                if (!input) return input.error();
                dirty_ |= terminal_.redraw();
                if (!*input) break;
                if (!filtering_ && **input == ftxui::Event::Character('q')) return STATUS_OK;
                key(**input);
                if (!refresh_ || action_) break;
            }
            if (terminal_.focused() && refresh_ && !pending_ && !scanning_ && std::exchange(dirty_, false))
                if ((status = terminal_.present(ui_)) != STATUS_OK) return status;
            if (!pending_ && (action_ || scanning_ || sys::clock_now().value >= refresh_)) continue;
            const auto next = pending_ ? rt_after_ms(20).value_or(0) : refresh_;
            status = terminal_.wait(next);
            if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
        }
    }
};
int main() { return ts_view{rt_startup()}.run(); }
