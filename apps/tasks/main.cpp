#include "terminal.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

static auto ts_text(ubus_value v) -> std::string { return std::string{v.as<std::string_view>().value_or("")}; }
static auto ts_num(ubus_value v) -> uint64_t { return v.as<uint64_t>().value_or(0); }
static auto ts_flag(ubus_value v) -> bool { return v.as<bool>().value_or(false); }
static auto ts_bytes(uint64_t n) -> std::string {
    if (n < 1024) return std::to_string(n) + " B";
    if (n < 1024 * 1024) return std::to_string(n / 1024) + " KiB";
    return std::to_string(n / (1024 * 1024)) + "." + std::to_string(n % (1024 * 1024) * 10 / (1024 * 1024)) + " MiB";
}
static auto ts_match(std::string text, std::string query) -> bool {
    for (auto* value : {&text, &query}) for (auto& c : *value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return query.empty() || text.find(query) != text.npos;
}
static auto ts_duration(uint64_t seconds) -> std::string {
    const auto days = seconds / 86400; seconds %= 86400;
    const auto minutes = seconds / 60 % 60, remainder = seconds % 60;
    return (days ? std::to_string(days) + "d " : "") + std::to_string(seconds / 3600)
        + (minutes < 10 ? ":0" : ":") + std::to_string(minutes)
        + (remainder < 10 ? ":0" : ":") + std::to_string(remainder);
}
static auto ts_percent(uint64_t n) -> std::string { return std::to_string(n / 10) + "." + std::to_string(n % 10) + "%"; }
static auto ts_error(status_t s) -> std::string {
    switch (s) {
    case STATUS_DENIED: case STATUS_BAD_RIGHTS: return "Permission denied";
    case STATUS_CLOSED: case STATUS_PEER_CLOSED: case STATUS_INVALID_CAP: return "Connection lost";
    case STATUS_STALE: return "Program changed; try again";
    case STATUS_TIMED_OUT: return "Not available right now";
    case STATUS_NOT_FOUND: return "Not available";
    case STATUS_BUSY: return "Busy; try again";
    case STATUS_NO_MEMORY: case STATUS_EXHAUSTED: return "Resource limit reached";
    case STATUS_BACKING_FAILED: return "Storage error";
    default: return "Failed (" + std::to_string(s) + ")";
    }
}
static auto ts_name(ubus_value row) -> std::string {
    auto name = ts_text(row["program"]);
    if (name.empty()) return ts_text(row["name"]);
    name.erase(0, name.find_last_of('/') == name.npos ? 0 : name.find_last_of('/') + 1);
    if (name.ends_with(".elf")) name.resize(name.size() - 4);
    return name;
}
static auto ts_state(ubus_value row) -> std::string {
    const auto state = ts_text(row["state"]);
    if (state == "idle") return ts_num(row["run_id"]) ? "Stopped" : "On demand";
    if (state == "running") return "Running";
    if (state == "starting" || state == "loading") return "Starting";
    if (state == "draining") return "Stopping";
    if (state == "failed") return "Failed";
    if (state == "done") return "Exited";
    return "Unknown";
}
struct ts_scope;
struct ts_item {
    std::weak_ptr<ts_scope> owner;
    std::shared_ptr<ts_scope> children;
    ubus_data data;
    std::optional<std::string> command;
    std::string key, path;
    uint64_t serial{}, tick{};
    std::vector<uint64_t> threads;
    std::optional<uint64_t> cpu;

    bool expanded{true};
};
static void ts_sample(ts_item& item, ubus_value row) {
    const auto samples = row["cpu"].vector<uint64_t>(); const auto tick = ts_num(row["sampled"]);
    if (!samples) { item.cpu.reset(); item.threads.clear(); item.tick = 0; return; }
    if (tick <= item.tick) return;
    bool same = item.tick && item.threads.size() == samples->size(); uint64_t elapsed{};
    for (size_t i = 0; same && i + 1 < samples->size(); i += 2) {
        same = item.threads[i] == (*samples)[i] && item.threads[i + 1] <= (*samples)[i + 1];
        if (same) elapsed += (*samples)[i + 1] - item.threads[i + 1];
    }
    item.cpu = same ? std::optional{elapsed * 1000 / (tick - item.tick) / _startup().cpus} : std::nullopt;
    item.threads.assign(samples->begin(), samples->end()); item.tick = tick;
}
struct ts_scope {
    _cap_owner cap;
    ubus_conn connection;
    std::weak_ptr<ts_item> parent;
    std::vector<std::shared_ptr<ts_item>> items;
    std::string path, error;
    uint64_t identity{}, round{};
    bool attempted{};
};
class ts_view {
    ft_terminal terminal_;
    ubus_msg setup_;
    std::shared_ptr<ts_scope> root_{std::make_shared<ts_scope>()};
    ubus_conn environment_, devices_;
    _cap_owner device_cap_;
    std::vector<ubus_data> devices_data_;
    std::vector<std::pair<std::shared_ptr<ts_item>, size_t>> rows_;
    std::string selection_, device_selection_, filter_, notice_, device_error_;
    uint64_t serial_{}, refresh_{}, round_{}, device_round_{}, frequency_{}, now_{};
    int page_{}, sort_{}, menu_{}, choice_{};
    bool tree_{}, all_{}, detail_{}, search_{}, help_{}, frozen_{}, devices_loaded_{}, dirty_{true}, quit_{};
    ftxui::Component input_, ui_;
    enum op { Bind, List, Children, Detail, DeviceGet, DeviceBind, Devices, Action };
    struct query {
        op kind{};
        std::shared_ptr<ts_scope> scope;
        std::shared_ptr<ts_item> item;
        ubus_msg message;
        uint64_t run_id{}, version{};
    };
    std::vector<query> active_;
    std::optional<query> intent_;
    std::shared_ptr<ts_item> target_;
    uint64_t target_run_{}, target_version_{};
    std::vector<std::string> actions_;

    auto scopes() const -> std::vector<std::shared_ptr<ts_scope>> {
        std::vector<std::shared_ptr<ts_scope>> result{root_};
        for (size_t i = 0; i < result.size() && result.size() < 32; ++i)
            for (const auto& item : result[i]->items) if (item->children && result.size() < 32) result.push_back(item->children);
        return result;
    }
    auto selected() const -> std::shared_ptr<ts_item> {
        for (const auto& scope : scopes()) for (const auto& item : scope->items) if (item->key == selection_) return item;
        return {};
    }
    auto device_rows() const -> std::vector<ubus_value> {
        std::vector<ubus_value> rows;
        for (const auto& row : devices_data_) if (ts_match(ts_text(row["name"]) + " " + ts_text(row["type"]), filter_)) rows.push_back(row.view());
        return rows;
    }
    auto device() const -> ubus_value {
        for (const auto row : device_rows()) if (ts_text(row["id"]) == device_selection_) return row;
        return {};
    }
    void rebuild() {
        dirty_ = true;
        if (page_) {
            const auto rows = device_rows();
            if (rows.empty()) device_selection_.clear();
            else if (!device()) device_selection_ = ts_text(rows.front()["id"]);
            return;
        }
        rows_.clear(); const auto& needle = filter_;
        const auto match = [&](const ts_item& item) {
            const auto state = ts_text(item.data["state"]);
            return (all_ || state == "running" || state == "draining" || state == "starting" || state == "loading" || state == "failed")
                && ts_match(ts_name(item.data.view()) + " " + item.path, needle);
        };
        const auto compare = [&](const auto& a, const auto& b) {
            if (sort_ == 1 && a->cpu != b->cpu) return a->cpu > b->cpu;
            if (sort_ == 2 && ts_num(a->data["resident"]) != ts_num(b->data["resident"])) return ts_num(a->data["resident"]) > ts_num(b->data["resident"]);
            const auto x = ts_name(a->data.view()), y = ts_name(b->data.view());
            return x != y ? x < y : a->serial < b->serial;
        };
        const auto visit = [&](auto&& self, const std::shared_ptr<ts_scope>& scope, size_t depth) -> void {
            auto items = scope->items; std::ranges::stable_sort(items, compare);
            for (const auto& item : items) {
                if (match(*item)) rows_.push_back({item, tree_ ? depth : 0});
                if (item->children && depth < 16 && (!tree_ || item->expanded || !needle.empty())) self(self, item->children, depth + 1);
            }
        };
        visit(visit, root_, 0);
        if (!tree_) std::ranges::stable_sort(rows_, [&](const auto& a, const auto& b) { return compare(a.first, b.first); });
        if (rows_.empty()) selection_.clear();
        if (!rows_.empty() && std::ranges::none_of(rows_, [&](const auto& row) { return row.first->key == selection_; })) selection_ = rows_.front().first->key;
        dirty_ = true;
    }
    auto inspect(op kind, const std::shared_ptr<ts_item>& item) -> query {
        query q; q.kind = kind; q.scope = item->owner.lock(); q.item = item;
        q.run_id = ts_num(item->data["run_id"]); q.version = ts_num(item->data["root_version"]);
        q.message.data.put("child_id", item->data["child_id"]).put("run_id", q.run_id).put("facet", kind == Children ? "children" : "overview");
        return q;
    }
    auto connection(query& q) -> ubus_conn& {
        return q.kind == DeviceGet ? environment_ : q.kind == DeviceBind || q.kind == Devices ? devices_ : q.scope->connection;
    }
    void begin(query q) {
        q.message.deadline = _after(std::chrono::milliseconds{800}).value_or(0);
        if (connection(q).active()) return;
        const uint64_t version = q.kind == Bind ? 3 : 1;
        const auto status = q.kind == Bind || q.kind == DeviceBind
            ? connection(q).begin_bind(q.kind == Bind ? q.scope->cap.selector() : device_cap_.selector(), q.kind == Bind ? "ubus.environment" : "io.devices", version, version, 0, q.message.deadline)
            : connection(q).begin(q.kind == List || q.kind == Devices ? "List" : q.kind == DeviceGet ? "Get" : q.kind == Action ? "Action" : "Inspect", std::move(q.message));
        if (status == STATUS_OK) active_.push_back(std::move(q));
        else {
            if (q.scope && (q.kind == Bind || q.kind == List || q.kind == Children)) q.scope->error = ts_error(status);
            else notice_ = ts_error(status);
            dirty_ = true;
        }
    }
    auto poll() -> bool {
        bool changed{};
        for (auto it = active_.begin(); it != active_.end();) {
            ubus_msg response;
            auto& q = *it;
            const auto code = q.kind == Bind || q.kind == DeviceBind ? connection(q).poll_bind() : connection(q).poll(response);
            if (code == STATUS_WOULD_BLOCK) { ++it; continue; }
            const auto status = code == STATUS_OK ? response.status : code;
            auto done = std::move(q); it = active_.erase(it);
            changed = true;
            reply(std::move(done), status, std::move(response));
        }
        return changed;
    }
    void reply(query done, status_t status, ubus_msg response) {
        if (done.item && ts_num(done.item->data["run_id"]) != done.run_id) return;
        if (status != STATUS_OK) {
            if (done.kind == List || done.kind == Bind) done.scope->error = ts_error(status);
            else if (done.kind == Children && done.item) {
                auto child = std::make_shared<ts_scope>(); child->parent = done.item; child->path = done.item->path;
                child->error = ts_error(status); child->attempted = true; done.item->children = std::move(child);
            } else if (done.kind == Devices || done.kind == DeviceGet || done.kind == DeviceBind) {
                device_error_ = ts_error(status); devices_data_.clear(); devices_.close(); device_cap_ = {};
            } else notice_ = ts_error(status);
            dirty_ = true; return;
        }
        if (done.kind == List) {
            auto& scope = *done.scope; scope.error.clear();
            const auto identity = ts_num(response.data["owner"]);
            if (identity != scope.identity) scope.items.clear();
            scope.identity = identity;
            size_t retained{};
            for (const auto& other : scopes()) if (other != done.scope) retained += other->items.size();
            std::vector<std::shared_ptr<ts_item>> incoming;
            for (const auto row : response.data["items"].values()) {
                if (incoming.size() >= std::min(size_t{128}, size_t{256} - std::min(retained, size_t{256}))) { scope.error = "List limit reached"; notice_ = scope.error; break; }
                const auto found = std::ranges::find_if(scope.items, [&](const auto& item) { return ts_num(item->data["child_id"]) == ts_num(row["child_id"]); });
                auto item = found == scope.items.end() ? std::make_shared<ts_item>() : *found;
                if (!item->serial) item->serial = ++serial_;
                if (!ts_flag(row["children"]) || item->data["root_version"].as<uint64_t>() != row["root_version"].as<uint64_t>()) item->children.reset();
                if (item->data["run_id"].as<uint64_t>() != row["run_id"].as<uint64_t>()) { item->tick = 0; item->threads.clear(); item->children.reset(); }
                if (item->data["run_id"].as<uint64_t>() != row["run_id"].as<uint64_t>()
                    || item->data["config"].as<uint64_t>() != row["config"].as<uint64_t>()) item->command.reset();
                item->owner = done.scope; item->key = std::to_string(identity) + ":" + std::to_string(ts_num(row["child_id"]));
                item->path = scope.path + "/" + ts_text(row["name"]);
                ts_sample(*item, row);
                item->data.assign(row.bytes()); incoming.push_back(std::move(item));
            }
            scope.items = std::move(incoming); rebuild();
            if (detail_ && !page_ && !intent_) if (auto item = selected(); item && !item->command
                && std::ranges::none_of(active_, [&](const auto& q) { return q.kind == Detail && q.item == item; })) intent_ = inspect(Detail, item);
        } else if (done.kind == Children && done.item && done.item->data["root_version"].as<uint64_t>() == done.version) {
            auto child = std::make_shared<ts_scope>(); child->parent = done.item; child->path = done.item->path;
            child->cap = response.take(response.data["connector"]);
            if (child->cap) done.item->children = std::move(child);
        } else if (done.kind == Detail) {
            const auto row = response.data["items"][size_t{0}];
            if (row["config"].as<uint64_t>() != done.item->data["config"].as<uint64_t>()) return;
            ts_sample(*done.item, row);
            for (const auto field : {"resident", "started"}) if (row[field]) done.item->data.put(field, row[field]);
            std::string command;
            for (const auto arg : row["argv"].values()) { if (!command.empty()) command += ' '; command += ts_text(arg); }
            done.item->command = std::move(command); dirty_ = true;
        } else if (done.kind == DeviceGet) { device_cap_ = response.take(response.data["connector"]); device_error_.clear(); device_round_ = 0; }
        else if (done.kind == DeviceBind) device_round_ = 0;
        else if (done.kind == Devices) {
            devices_data_.clear(); device_error_.clear(); devices_loaded_ = true;
            for (const auto row : response.data["items"].values()) { if (devices_data_.size() == 128) { notice_ = "Device list limit reached"; break; } ubus_data data; data.assign(row.bytes()); devices_data_.push_back(std::move(data)); }
            rebuild();
        } else if (done.kind == Action) { notice_ = "Requested"; refresh_ = 0; dirty_ = true; }
    }
    void next() {
        if (!terminal_.focused()) return;
        if (intent_ && !connection(*intent_).active()) { auto q = std::move(*intent_); intent_.reset(); begin(std::move(q)); }
        if (frozen_ || menu_) return;
        const auto now = _clock_now().value;
        if (now >= refresh_) {
            refresh_ = refresh_ ? refresh_ + ((now - refresh_) / frequency_ + 1) * frequency_ : now + frequency_;
            ++round_; if (detail_) dirty_ = true;
        }
        if (page_ == 1) {
            if (environment_.active() || devices_.active()) return;
            query q;
            if (!device_cap_) {
                if (device_round_ == round_) return;
                device_round_ = round_;
                const auto name = setup_.data["message"]["devices"].as<std::string_view>();
                if (!name) { device_error_ = "No device access"; dirty_ = true; return; }
                q.kind = DeviceGet; q.message.data.put("name", *name).put("activate", false);
            } else if (!devices_.endpoint()) q.kind = DeviceBind;
            else {
                if (device_round_ == round_) return;
                device_round_ = round_; q.kind = Devices;
            }
            begin(std::move(q)); return;
        }
        for (const auto& scope : scopes()) {
            for (auto p = scope->parent.lock(); p; p = p->owner.lock() ? p->owner.lock()->parent.lock() : nullptr)
                if (scope->identity && p->owner.lock() && p->owner.lock()->identity == scope->identity) { scope->error = "Repeated reference"; break; }
            if (!scope->error.empty() || scope->connection.active()) continue;
            query q; q.scope = scope;
            if (!scope->connection.endpoint()) {
                if (!scope->attempted) { scope->attempted = true; q.kind = Bind; begin(std::move(q)); }
                continue;
            }
            if (scope->round != round_) {
                scope->round = round_; q.kind = List; begin(std::move(q)); continue;
            }
            for (const auto& item : scope->items) if (ts_flag(item->data["children"]) && !item->children) {
                if (scopes().size() == 32) { notice_ = "View limit reached"; continue; }
                begin(inspect(Children, item)); break;
            }
        }
    }
    void move(int delta) {
        if (menu_ == 2) return;
        if (menu_) { choice_ = std::clamp(choice_ + delta, 0, std::max(0, int(actions_.size()) - 1)); }
        else if (page_ == 0) {
            const auto found = std::ranges::find_if(rows_, [&](const auto& row) { return row.first->key == selection_; });
            if (!rows_.empty()) selection_ = rows_[std::clamp(int(found - rows_.begin()) + delta, 0, int(rows_.size()) - 1)].first->key;
        } else {
            std::vector<std::string> ids; for (const auto row : device_rows()) ids.push_back(ts_text(row["id"]));
            const auto found = std::ranges::find(ids, device_selection_);
            if (!ids.empty()) device_selection_ = ids[std::clamp(int(found - ids.begin()) + delta, 0, int(ids.size()) - 1)];
        }
        if (detail_ && !page_) if (auto item = selected(); item && !item->command) intent_ = inspect(Detail, item);
        notice_.clear(); dirty_ = true;
    }
    void actions() {
        actions_.clear(); choice_ = 0; target_ = selected();
        if (page_ || !target_) { notice_ = "No actions"; return; }
        if (const auto scope = target_->owner.lock(); !scope || !scope->error.empty()) { notice_ = "Not available"; return; }
        target_run_ = ts_num(target_->data["run_id"]); target_version_ = ts_num(target_->data["config"]);
        for (const auto value : target_->data["actions"].values()) {
            const auto verb = ts_text(value); if (verb == "stop" || verb == "start" || verb == "restart") actions_.push_back(verb);
        }
        if (actions_.empty()) { notice_ = ts_flag(target_->data["control"]) ? "No actions" : "Read only"; return; }
        menu_ = 1;
    }
    void key(const ftxui::Event& e) {
        dirty_ = true; notice_.clear();
        if (search_) {
            if (e == ftxui::Event::Escape) { search_ = false; filter_.clear(); }
            else if (e == ftxui::Event::Return) search_ = false;
            else input_->OnEvent(e);
            rebuild(); return;
        }
        if (e == ftxui::Event::Escape) {
            if (menu_) { menu_ = 0; target_.reset(); }
            else if (help_) help_ = false;
            else if (detail_) detail_ = false;
            else if (!filter_.empty()) { filter_.clear(); rebuild(); }
            else quit_ = true;
            return;
        }
        if (e == ftxui::Event::ArrowUp) { move(-1); return; }
        if (e == ftxui::Event::ArrowDown) { move(1); return; }
        if (e == ftxui::Event::Return) {
            if (menu_) {
                const auto verb = actions_[choice_];
                if (menu_ == 1 && verb != "start") { menu_ = 2; return; }
                if (auto scope = target_->owner.lock()) {
                    query q; q.kind = Action; q.scope = scope; q.item = target_; q.run_id = target_run_;
                    q.message.data.put("child_id", target_->data["child_id"]).put("run_id", target_run_).put("version", target_version_).put("action", verb); intent_ = std::move(q);
                }
                menu_ = 0; target_.reset();
            } else if (!help_) { detail_ = true; if (!page_) if (auto item = selected(); item && !item->command) intent_ = inspect(Detail, item); }
            return;
        }
        if (menu_ || help_) { if (e == ftxui::Event::Character('q')) { menu_ = 0; help_ = false; } return; }
        if (e == ftxui::Event::Character('q')) { quit_ = true; return; }
        if (e == ftxui::Event::Character('?')) { help_ = true; return; }
        if (e == ftxui::Event::Character('a')) { actions(); return; }
        if (e == ftxui::Event::Character('/')) { search_ = true; return; }
        if (e == ftxui::Event::Character(' ')) { frozen_ = !frozen_; return; }
        if (e == ftxui::Event::Character('r')) {
            refresh_ = 0;
            for (const auto& scope : scopes()) {
                scope->error.clear(); scope->attempted = false;
                for (const auto& item : scope->items) if (item->children && !item->children->error.empty()) item->children.reset();
            }
            return;
        }
        if (e == ftxui::Event::Character('1') || e == ftxui::Event::Character('2') || e == ftxui::Event::Tab) {
            page_ = e == ftxui::Event::Tab ? 1 - page_ : e == ftxui::Event::Character('1') ? 0 : 1;
            detail_ = false; filter_.clear(); notice_.clear(); refresh_ = 0; rebuild(); return;
        }
        if (!page_ && !detail_) {
            if (e == ftxui::Event::Character('s')) sort_ = (sort_ + 1) % 3;
            if (e == ftxui::Event::Character('t')) tree_ = !tree_;
            if (e == ftxui::Event::Character('h')) all_ = !all_;
            if (auto item = selected(); item && (e == ftxui::Event::ArrowLeft || e == ftxui::Event::ArrowRight)) item->expanded = e == ftxui::Event::ArrowRight;
            rebuild();
        }
    }
    auto line(std::string name, std::string state, std::string a, std::string b) -> ftxui::Element {
        ftxui::Elements parts{ftxui::text(name) | ftxui::flex, ftxui::text(state) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 12)};
        if (terminal_.columns() >= 64) parts.push_back(ftxui::text(a) | ftxui::align_right | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, page_ ? 16 : 9));
        parts.push_back(ftxui::text(b) | ftxui::align_right | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 12)); return ftxui::hbox(parts);
    }
    auto field(std::string name, std::string value) -> ftxui::Element {
        return ftxui::hbox({ftxui::text(name) | ftxui::dim | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 12), ftxui::text(value) | ftxui::flex});
    }
    auto render() -> ftxui::Element {
        const auto tab = [&](std::string label, bool active) { return ftxui::text(" " + label + " ") | (active ? ftxui::inverted : ftxui::dim); };
        ftxui::Elements parts{ftxui::hbox({ftxui::text(" TASKS ") | ftxui::bold, tab("1 Programs", !page_), tab("2 Devices", page_), ftxui::filler(), ftxui::text(frozen_ ? "Paused " : root_->path.substr(root_->path.find_last_of('/') + 1) + " ") | ftxui::dim}), ftxui::separator()};
        if (help_) {
            parts.push_back(ftxui::vbox({ftxui::text("Keys") | ftxui::bold, ftxui::text(""), field("Up/Down", "Select"), field("Left/Right", "Collapse / expand"), field("Enter", "Details"), field("a", "Actions"), field("/", "Search"), field("s", "Sort by name, CPU, memory"), field("t", "Tree / list"), field("h", "Include stopped programs"), field("Space", "Pause updates"), field("r", "Refresh"), field("Esc", "Back"), field("q", "Quit"), ftxui::text(""), field("CPU", "100% = all cores"), field("Memory", "Resident memory")}) | ftxui::flex);
        } else if (detail_) {
            ftxui::Elements body;
            if (page_) {
                const auto row = device();
                body = {ftxui::text(ts_text(row["name"])) | ftxui::bold, ftxui::text(""), field("Status", ts_text(row["state"])), field("Type", ts_text(row["type"])), field("Driver", ts_text(row["driver"])), field("Location", ts_text(row["location"]))};
                if (!row) body = {ftxui::text(device_error_.empty() ? "Device not available" : device_error_) | ftxui::dim};
                if (row["error"]) body.push_back(field("Problem", ts_error(static_cast<status_t>(row["error"].as<int64_t>().value_or(0)))));
            } else if (const auto item = selected()) {
                const auto row = item->data.view();
                const auto scope = item->owner.lock(); const bool current = scope && scope->error.empty();
                body = {ftxui::hbox({ftxui::text(ts_name(row)) | ftxui::bold | ftxui::flex,
                    ftxui::text(current ? ts_state(item->data.view()) : "Unavailable") | ftxui::dim}),
                    ftxui::text(""), field("Program", ts_text(row["program"]))};
                if (item->command && !item->command->empty()) body.push_back(field("Command", *item->command));
                const auto age = item->data["started"].as<uint64_t>();
                ftxui::Elements labels, values;
                for (const auto text : {"CPU", "Memory", "Elapsed"}) labels.push_back(ftxui::text(text) | ftxui::dim | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, terminal_.columns() / 3));
                for (const auto& text : std::array<std::string, 3>{
                    current && item->cpu ? ts_percent(*item->cpu) : "--",
                    current && item->data["resident"] ? ts_bytes(ts_num(item->data["resident"])) : "--",
                    current && age && now_ >= *age ? ts_duration((now_ - *age) / frequency_) : "--"})
                    values.push_back(ftxui::text(text) | ftxui::bold | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, terminal_.columns() / 3));
                body.insert(body.begin() + 2, {ftxui::hbox(labels), ftxui::hbox(values), ftxui::separator()});
                if (const auto result = row["result"].as<int64_t>(); ts_text(item->data["state"]) == "failed" && result && *result) body.push_back(field("Last error", ts_error(static_cast<status_t>(*result))));
            } else body.push_back(ftxui::text("Program ended") | ftxui::dim);
            parts.push_back(ftxui::vbox(body) | ftxui::flex);
        } else {
            if (search_) parts.push_back(input_->Render());
            else if (!filter_.empty()) parts.push_back(ftxui::text("Search: " + filter_) | ftxui::dim);
            else if (!page_ && (all_ || tree_)) parts.push_back(ftxui::text(std::string{tree_ ? "Tree" : "List"} + (all_ ? "  /  All programs" : "")) | ftxui::dim);
            parts.push_back(page_ ? line("Device", "Status", "Type", "Driver") | ftxui::dim
                : line("Name" + std::string{sort_ == 0 ? " ↑" : ""}, "Status", sort_ == 1 ? "CPU ↓" : "CPU", sort_ == 2 ? "Memory ↓" : "Memory") | ftxui::dim);
            parts.push_back(ftxui::separator()); ftxui::Elements rows;
            if (page_) {
                for (const auto row : device_rows()) {
                    auto entry = line(ts_text(row["name"]), ts_text(row["state"]), ts_text(row["type"]), ts_text(row["driver"]));
                    if (ts_text(row["id"]) == device_selection_) entry = entry | ftxui::inverted | ftxui::focus;
                    rows.push_back(entry);
                }
                if (rows.empty()) rows.push_back(ftxui::text(device_error_.empty() ? devices_loaded_ ? "No devices" : "Loading..." : device_error_) | ftxui::dim);
            } else {
                for (const auto& [item, depth] : rows_) {
                    auto name = ts_name(item->data.view());
                    name = std::string(depth * 2, ' ') + (tree_ && ts_flag(item->data["children"]) ? item->expanded ? "- " : "+ " : "") + name;
                    const auto scope = item->owner.lock(); const bool current = scope && scope->error.empty();
                    auto entry = line(name, current ? ts_state(item->data.view()) : "Unavailable", current && item->cpu ? ts_percent(*item->cpu) : "--", current && item->data["resident"] ? ts_bytes(ts_num(item->data["resident"])) : "--");
                    if (ts_text(item->data["state"]) == "failed") entry |= ftxui::color(ftxui::Color::Red);
                    if (item->key == selection_) entry = entry | ftxui::inverted | ftxui::focus;
                    rows.push_back(entry);
                }
                if (rows.empty()) rows.push_back(ftxui::text(root_->error.empty() ? root_->identity ? "No programs" : "Loading..." : root_->error) | ftxui::dim);
            }
            parts.push_back(ftxui::vbox(rows) | ftxui::vscroll_indicator | ftxui::yframe | ftxui::flex);
        }
        if (!notice_.empty()) parts.push_back(ftxui::text(notice_) | ftxui::color(ftxui::Color::Yellow));
        parts.push_back(ftxui::separator());
        parts.push_back(ftxui::text(help_ ? " Esc Back" : page_ ? detail_ ? " Esc Back    q Quit" : " Enter Details    / Search    ? Help    q Quit" : detail_ ? " Esc Back    a Actions    q Quit" : " Enter Details    a Actions    / Search    ? Help    q Quit") | ftxui::dim);
        auto result = ftxui::vbox(parts);
        if (menu_) {
            const auto label = [](const std::string& verb) { return verb == "stop" ? "End program" : verb == "restart" ? "Restart" : "Start"; };
            ftxui::Elements box{ftxui::text(ts_name(target_->data.view())) | ftxui::bold, ftxui::text("")};
            if (menu_ == 2) {
                box.push_back(ftxui::text(std::string{label(actions_[choice_])} + "?"));
                if (ts_flag(target_->data["children"])) box.push_back(ftxui::text("Includes its child programs.") | ftxui::dim);
                box.push_back(ftxui::text("")); box.push_back(ftxui::text("Enter Confirm   Esc Cancel") | ftxui::dim);
            } else for (size_t i = 0; i < actions_.size(); ++i) box.push_back(ftxui::text(label(actions_[i])) | (int(i) == choice_ ? ftxui::inverted : ftxui::nothing));
            result = ftxui::dbox({result, ftxui::vbox(box) | ftxui::border | ftxui::clear_under | ftxui::bgcolor(ftxui::Color::Black) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, std::min(40, int(terminal_.columns()) - 4)) | ftxui::center});
        }
        return result;
    }
public:
    ts_view() { input_ = ftxui::Input(&filter_, "Search"); ui_ = ftxui::Renderer([this] { return render(); }); }
    auto run() -> status_t {
        const auto& env = _startup();
        frequency_ = _clock_frequency().value;
        auto status = environment_.bind(env.bootstrap);
        if (status == STATUS_OK) status = environment_.invoke("Receive", {}, setup_);
        if (status == STATUS_OK) status = terminal_.open(env, setup_);
        if (status != STATUS_OK) return status;
        root_->cap = setup_.take(setup_.data["message"]["scope"]);
        root_->path = "/" + ts_text(setup_.data["message"]["scope.name"]);
        if (!root_->cap) { root_->error = "No program access"; root_->attempted = true; }
        while (!quit_) {
            for (size_t count = 0; count < 64; ++count) {
                const auto e = terminal_.read(); if (!e) return e.error();
                dirty_ |= terminal_.redraw(); if (!*e) break; key(**e);
            }
            if (quit_) break;
            if (!frozen_) now_ = _clock_now().value;
            bool progress = poll(); next(); progress |= poll();
            if (terminal_.focused() && dirty_) {
                status = terminal_.present(ui_);
                if (status == STATUS_OK) dirty_ = false;
                else if (status != STATUS_WOULD_BLOCK) return status;
            }
            const auto sent = terminal_.flush();
            if (sent != STATUS_OK && sent != STATUS_PENDING && sent != STATUS_WOULD_BLOCK) return sent;
            if (sent == STATUS_PENDING || progress || (sent == STATUS_OK && terminal_.focused() && dirty_)) continue;
            auto deadline = frozen_ || menu_ || !terminal_.focused() ? uint64_t{0} : refresh_;
            const auto limit = [&](uint64_t when) { if (when && (!deadline || when < deadline)) deadline = when; };
            for (const auto& q : active_) limit(q.message.deadline);
            limit(terminal_.deadline());
            status = terminal_.wait(deadline);
            if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
        }
        return STATUS_OK;
    }
};
int main() { return ts_view{}.run(); }
