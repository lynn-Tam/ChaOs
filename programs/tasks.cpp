#include <protocols/manage.hpp>
#include <adapters/ftxui/terminal.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <array>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

struct ts_task { mg_task info; cap_t owner; uint64_t sampled; uint32_t cpu{}; bool measured{}; };
struct ts_scope { cap_t owner; uint64_t id, incarnation; uint32_t actions; std::string name; };
using ts_fact = std::variant<CapInfo, ScopeMember, LinkRecord, mg_grant, mg_dependency, AccountInfo, mg_domain, mg_operation>;
struct ts_nav { unsigned view{}; uint64_t object{}, selector{}, grant{}, rights{}; bool usage{}; };
static constexpr std::array<const char*, 8> ts_states{
    "Stopped", "Draining", "Starting", "Running", "Failed", "Suspending", "Suspended", "Done"};
static constexpr std::array<const char*, ObjCount> ts_kinds{
    "?", "Thread", "SC", "Domain", "CSpace", "Mem", "VSpace", "Account", "Notify", "Channel",
    "Pager", "IRQ", "IOMMU", "IOSpace", "Phys", "IRQ ctl", "Scope", "Port", "VM Op", "IO Op"};
struct ts_col { const char* title; int width; bool numeric{}; };
static constexpr std::array<const char*, 3> ts_views{"holders", "grants", "objects"};
template<class... A> static auto ts_line(std::string_view format, const A&... args) -> std::string {
    libk::fmt::fixed_buffer<160> text;
    (void)libk::fmt::format_to(text, format, args...);
    return std::string{text.view()};
}
static auto ts_kind(unsigned kind) -> const char* { return ts_kinds[std::min<unsigned>(kind, ObjCount - 1)]; }
static auto ts_memory(uint64_t bytes) -> std::string { return bytes == UINT64_MAX ? "-" : std::to_string(bytes / 1024); }
static auto ts_rights(uint64_t rights) -> std::string {
    std::string text;
    for (const auto& [bit, name] : std::array<std::pair<uint64_t, const char*>, 28>{{
        {RIGHT_SEND, "send"}, {RIGHT_RECEIVE, "recv"}, {RIGHT_MAP, "map"},
        {RIGHT_INSPECT, "inspect"}, {RIGHT_CONTROL, "control"}, {RIGHT_MANAGE, "manage"},
        {RIGHT_REVOKE, "revoke"}, {RIGHT_DESTROY, "destroy"}, {RIGHT_CLOSE, "close"},
        {RIGHT_DUPLICATE, "duplicate"}, {RIGHT_DELEGATE, "delegate"}, {RIGHT_RESERVE, "reserve"},
        {RIGHT_UNMAP, "unmap"}, {RIGHT_PROTECT, "protect"}, {RIGHT_CREATE, "create"},
        {RIGHT_SPLIT, "split"}, {RIGHT_SIGNAL, "signal"}, {RIGHT_CONNECT, "connect"},
        {RIGHT_ACK, "ack"}, {RIGHT_SERVE, "serve"}, {RIGHT_SUPPLY, "supply"},
        {RIGHT_FAIL, "fail"}, {RIGHT_WRITEBACK_ACK, "writeback ack"}, {RIGHT_ROUTE, "route"},
        {RIGHT_OBSERVE, "observe"}, {RIGHT_ATTACH, "attach"}, {RIGHT_ADOPT, "adopt"}, {RIGHT_REBILL, "rebill"}}})
        if (rights & bit) { if (!text.empty()) text += ' '; text += name; }
    return text.empty() ? "-" : text;
}

class ts_view {
    const rt_env& env_;
    ft_terminal terminal_;
    rpc_pending rpc_, watch_;
    std::tuple<cap_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t> watched_{};
    uint64_t watch_seq_{}, grant_seq_{}, watch_retry_{};
    std::array<cap_t, 2> owners_;
    std::vector<ts_task> tasks_, incoming_;
    std::vector<mg_device> devices_, device_scan_;
    std::vector<ts_fact> facts_, fact_scan_;
    std::optional<ts_scope> authority_;
    std::optional<ScopeObject> detail_;
    std::optional<CapView> ceiling_;
    ts_nav nav_;
    std::vector<ts_nav> history_;
    struct action { cap_t endpoint; msg_frame message; };
    std::optional<action> action_;
    bool rescan_{}, acting_{}, scanning_{}, deployment_{}, scan_dependencies_{}, scan_deployed_{}, scan_view_{}, scan_info_{}, redraw_{true};
    int page_{}, selected_{};
    unsigned scan_page_{}, owner_{}, sort_{};
    uint64_t cursor_{}, epoch_{}, refresh_{}, generation_{}, request_gen_{};
    mg_op request_op_{};
    std::string notice_{"Authorized owner samples; one CPU = 100%"}, filter_;
    std::vector<std::string> tabs_{"1 Tasks", "2 Devices", "3 Topology"}, labels_;
    std::vector<size_t> visible_;
    bool filtering_{};
    ftxui::Component tabs_ui_, filter_ui_, list_ui_, ui_;
    using Token = std::tuple<uint64_t, uint64_t, uint64_t, std::string>;
    std::optional<Token> selection_;
    auto token(size_t i) const -> Token {
        if (page_ == 0 && i < tasks_.size()) return {tasks_[i].owner, tasks_[i].info.id, tasks_[i].info.incarnation, {}};
        if (page_ == 1 && i < devices_.size()) return {0, devices_[i].id, devices_[i].incarnation, {}};
        if (page_ != 2 || i >= facts_.size()) return {};
        return std::visit([&](const auto& row) -> Token {
            using T = std::decay_t<decltype(row)>;
            if constexpr (std::same_as<T, CapInfo>) return {1, row.selector, row.grant_id, {}};
            else if constexpr (std::same_as<T, ScopeMember>) return {2, row.object_id, 0, {}};
            else if constexpr (std::same_as<T, LinkRecord>) return {3, row.link_id, row.first, {}};
            else if constexpr (std::same_as<T, mg_grant>) return {4, row.grant_id, 0, {}};
            else if constexpr (std::same_as<T, mg_dependency>) return {5, row.provider, row.incarnation, row.name};
            else return {6 + facts_[i].index(), nav_.object, 0, {}};
        }, facts_[i]);
    }
    auto index() const -> size_t {
        if (selected_ < 0 || static_cast<size_t>(selected_) >= visible_.size()) return SIZE_MAX;
        auto i = visible_[selected_];
        return selection_ && token(i) == *selection_ ? i : SIZE_MAX;
    }
    auto columns() const -> std::span<const ts_col> {
        static constexpr ts_col tasks[]{{"Owner",5},{"Job",4,true},{"Mbr",3,true},{"Name",11},
            {"State",10},{"CPU %",5,true},{"RSS KiB",7,true},{"Charge/Limit KiB",0,true}};
        static constexpr ts_col devices[]{{"Device",14},{"State",11},{"Requester",9,true},{"IRQ",5,true},{"IOSpace",0,true}};
        static constexpr ts_col caps[]{{"Slot",9,true},{"Grant",7,true},{"Object",7,true},{"Kind",9},{"State",10},{"Rights / relation",0}};
        static constexpr ts_col grants[]{{"Grant",7,true},{"Parent",7,true},{"Object",7,true},{"Kind",9},{"State",10},{"Rights",0}};
        static constexpr ts_col members[]{{"Object",9,true},{"Kind",12},{"State",0}};
        static constexpr ts_col links[]{{"Link",9,true},{"Relation",11},{"Target",8,true},{"State",12},{"Range",0}};
        static constexpr ts_col deps[]{{"Deployment",20},{"Provider",12,true},{"Incarnation",0,true}};
        static constexpr ts_col account[]{{"Used KiB",10,true},{"Limit KiB",10,true},{"Caps used/limit",16,true},{"Allocations",12,true},{"State",0}};
        static constexpr ts_col domain[]{{"CPU ticks",17,true},{"Work ticks",17,true},{"Debt",17,true},{"Escrow",0,true}};
        static constexpr ts_col operation[]{{"Phase",14},{"Result",12,true},{"Value",0,true}};
        if (page_ == 0) return tasks;
        if (page_ == 1) return devices;
        const auto type = facts_.empty() ? (nav_.view == 1 ? 3U : nav_.view == 0 ? 0U : 1U) : facts_.front().index();
        switch (type) {
        case 0: return caps;
        case 1: return members;
        case 2: return links;
        case 3: return grants;
        case 4: return deps;
        case 5: return account;
        case 6: return domain;
        default: return operation;
        }
    }
    auto fields(size_t i) const -> std::vector<std::string> {
        auto id = [](uint64_t value) -> std::string { return value ? ts_line("{:x}", value) : "-"; };
        if (page_ == 0) {
            const auto& row = tasks_[i]; const bool job = row.owner != owners_[0];
            return {job ? "jobs" : "root", job ? std::to_string(row.info.owner) : "-",
                job ? std::to_string(row.info.member) : "-", row.info.name, ts_states[std::min(row.info.state,7U)],
                row.measured ? ts_line("{}.{}",row.cpu/10,row.cpu%10) : "-", ts_memory(row.info.resident),
                ts_memory(row.info.charged)+"/"+ts_memory(row.info.limit)};
        }
        if (page_ == 1) {
            const auto& row = devices_[i];
            return {row.name, ts_states[std::min(row.state,7U)], std::to_string(row.requester),
                std::to_string(row.irq), std::to_string(row.iospace)};
        }
        return std::visit([&](const auto& row) -> std::vector<std::string> {
            using T = std::decay_t<decltype(row)>;
            if constexpr (std::same_as<T, CapInfo>) return {id(row.selector),id(row.grant_id),id(row.object_id),ts_kind(row.view.kind),
                row.state & CAP_INFO_BOUNDARY ? "boundary" : row.state & CAP_INFO_VALID ? "Live" : "gone",ts_rights(row.view.rights)};
            else if constexpr (std::same_as<T, ScopeMember>) return {id(row.object_id),ts_kind(row.kind),
                row.state == SCOPE_MEMBER_PENDING ? "Pending" : row.state == SCOPE_MEMBER_CLOSING ? "Closing" : "Live"};
            else if constexpr (std::same_as<T, LinkRecord>) return {id(row.link_id),
                row.kind == LINK_VM ? "VM" : row.kind == LINK_IO ? "DMA" : row.kind == LINK_ROOT ? "root" : row.kind == LINK_BIND ? "binding" : row.kind == LINK_PARENT ? "parent" : row.kind == LINK_ROUTE ? "IRQ route" : row.kind == LINK_PHYS ? "physical" : "use",
                id(row.object_id),row.flags & OBS_BOUNDARY ? "boundary" : row.state == LINK_LIVE ? "Live" : row.state == LINK_DRAINING ? "Draining" : "Invalidating",
                ts_line("{:x} + {}B",row.first,row.bytes)};
            else if constexpr (std::same_as<T, mg_grant>) return {id(row.grant_id),id(row.parent_id),id(row.object_id),ts_kind(row.kind),
                row.flags & OBS_BOUNDARY ? "boundary" : row.state == 2 ? "Revoked" : row.state ? "Closing" : "Live",ts_rights(row.rights)};
            else if constexpr (std::same_as<T, mg_dependency>) {
                if (columns().size() == 6) return {"-","-",id(row.provider),"Deploy","-",row.name};
                return {row.name,id(row.provider),std::to_string(row.incarnation)};
            } else if constexpr (std::same_as<T, AccountInfo>) return {ts_memory(row.memory_limit-row.memory_available),ts_memory(row.memory_limit),
                ts_line("{}/{}",row.cap_limit-row.caps_available,row.cap_limit),std::to_string(row.allocations),
                row.state == OBJECT_DRAINED ? "Drained" : row.state == OBJECT_CLOSING ? "Closing" : "Open"};
            else if constexpr (std::same_as<T, mg_domain>) return {std::to_string(row.runtime),std::to_string(row.work_runtime),std::to_string(row.debt),std::to_string(row.escrow)};
            else return {row.phase == OP_DONE ? "Done" : row.phase == OP_DRAINING ? "Draining" : row.phase == OP_COMMITTED ? "Committed" : "Accepted",
                std::to_string(row.status),std::to_string(row.value)};
        },facts_[i]);
    }
    auto fact_text(size_t i) const -> std::string {
        if (const auto* dependency = std::get_if<mg_dependency>(&facts_[i]))
            return ts_line("deployment {}  provider {}",dependency->name,dependency->provider);
        const auto data = fields(i); const auto cols = columns();
        std::string text;
        for (size_t n = 0; n < data.size(); ++n) {
            if (n) text += "  ";
            text += cols[n].title; text += ": "; text += data[n];
        }
        return text;
    }
    void rows() {
        visible_.clear(); labels_.clear(); selected_ = 0;
        const size_t count = page_ == 0 ? tasks_.size() : page_ == 1 ? devices_.size() : facts_.size();
        for (size_t i = 0; i < count; ++i) {
            auto label = page_ == 0 ? std::string{tasks_[i].info.name} + " " + ts_states[std::min(tasks_[i].info.state, 7U)]
                : page_ == 1 ? std::string{devices_[i].name} + " " + ts_states[std::min(devices_[i].state, 7U)] : fact_text(i);
            if (filter_.empty() || label.find(filter_) != std::string::npos) visible_.push_back(i);
        }
        std::stable_sort(visible_.begin(), visible_.end(), [&](size_t a, size_t b) {
            if (page_ == 0) {
                if (sort_ == 1) return tasks_[a].info.state < tasks_[b].info.state;
                if (sort_ == 2) return tasks_[a].cpu > tasks_[b].cpu;
                return std::string_view{tasks_[a].info.name} < tasks_[b].info.name;
            }
            if (page_ == 1) {
                if (sort_ == 1) return devices_[a].state < devices_[b].state;
                return std::string_view{devices_[a].name} < devices_[b].name;
            }
            return a < b;
        });
        if (!selection_ && !visible_.empty()) selection_ = token(visible_.front());
        for (auto i : visible_) {
            if (selection_ && token(i) == *selection_) selected_ = labels_.size();
            labels_.push_back(std::to_string(i));
        }
        redraw_ = true;
    }
    auto table(size_t i = SIZE_MAX) const -> ftxui::Element {
        const auto cols = columns();
        const auto data = i == SIZE_MAX ? std::vector<std::string>{} : fields(i);
        int remaining = static_cast<int>(terminal_.columns()) - 3 - 2 * static_cast<int>(cols.size()-1);
        for (const auto& col : cols) remaining -= col.width;
        ftxui::Elements cells{ftxui::text(i != SIZE_MAX && selection_ && token(i) == *selection_ ? "> " : "  ")};
        for (size_t n = 0; n < cols.size(); ++n) {
            if (n) cells.push_back(ftxui::text("  "));
            const int width = cols[n].width ? cols[n].width : std::max(1,remaining);
            auto value = i == SIZE_MAX ? std::string{cols[n].title} : data[n];
            if (value.size() > static_cast<size_t>(width)) value = value.substr(0,std::max(0,width-3)) + "...";
            auto cell = ftxui::text(value);
            if (cols[n].numeric) cell = ftxui::align_right(cell);
            cells.push_back(cell | ftxui::size(ftxui::WIDTH,ftxui::EQUAL,width));
        }
        return ftxui::hbox(std::move(cells));
    }
    void refresh(bool clear = false) {
        ++generation_; scanning_ = false; refresh_ = 0;
        if (clear) { facts_.clear(); detail_.reset(); ceiling_.reset(); selection_.reset(); }
        rows();
    }
    void navigate(ts_nav next) {
        if (history_.size() >= 32) { notice_ = "Truncated: navigation depth 32"; return; }
        if (next.object && next.object == nav_.object && next.view == nav_.view && next.grant == nav_.grant) {
            notice_ = "Reference to current object"; redraw_ = true; return;
        }
        if (next.object && std::ranges::any_of(history_, [&](const ts_nav& old) {
            return next.object == old.object && next.view == old.view && next.grant == old.grant;
        })) { notice_ = "Reference to an earlier object; h returns along the path"; redraw_ = true; return; }
        history_.push_back(nav_); nav_ = next; filter_.clear(); refresh(true);
    }
    void begin_scan() {
        scan_page_ = page_; cursor_ = epoch_ = 0; owner_ = 0; scanning_ = true;
        incoming_.clear(); device_scan_.clear(); fact_scan_.clear();
        scan_dependencies_ = scan_deployed_ = false;
        scan_view_ = scan_page_ == 2 && nav_.view == 1;
        scan_info_ = scan_page_ == 2 && nav_.view == 2 && nav_.object;
    }
    void complete_scan() {
        if (scan_page_ == 0) tasks_ = std::move(incoming_);
        if (scan_page_ == 1) devices_ = std::move(device_scan_);
        if (scan_page_ == 2) facts_ = std::move(fact_scan_);
        scanning_ = false; refresh_ = scan_page_ == 2 && !nav_.usage
            && !(detail_ && (detail_->kind == ObjAccount || detail_->kind == ObjVmOp || detail_->kind == ObjIoOp))
            && !std::exchange(rescan_, false)
            ? UINT64_MAX : rt_after_ms(500).value_or(0); rows();
    }
    void query() {
        if (rpc_.active()) return;
        if (action_) {
            acting_ = true;
            const auto status = rpc_.begin(action_->endpoint, env_, action_->message);
            if (status != STATUS_OK) { notice_ = ts_line("request error {}", status); action_.reset(); acting_ = false; }
            return;
        }
        if (!scanning_) { if (sys::clock_now().value < refresh_) return; begin_scan(); }
        cap_t endpoint{}; msg_frame request;
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
            if (scan_dependencies_) request = mg_request(mg_op::Dependencies, authority_->id, authority_->incarnation, cursor_);
            else {
                request_op_ = scan_view_ ? mg_op::GrantView : scan_info_ ? mg_op::Object : nav_.view == 1 ? mg_op::Grants
                    : nav_.view == 0 || (detail_ && detail_->kind == ObjCSpace && nav_.object) ? mg_op::Caps
                    : detail_ && (detail_->kind == ObjAccount || (detail_->kind == ObjDomain && nav_.usage)) ? mg_op::Usage
                    : detail_ && (detail_->kind == ObjVmOp || detail_->kind == ObjIoOp) ? mg_op::Ops
                    : !nav_.object || (detail_ && detail_->kind == ObjScope) ? mg_op::Members : mg_op::Links;
                mg_target target{.incarnation=authority_->incarnation, .argument=nav_.object, .epoch=epoch_,
                    .cursor=cursor_, .selector=nav_.selector, .selected=nav_.grant};
                if (request_op_ == mg_op::Caps) { target.argument = cursor_; target.selected = nav_.object; }
                request = mg_query(request_op_, authority_->id, target);
            }
        }
        request_gen_ = generation_; request.deadline = rt_after_ms(200).value_or(0);
        const auto status = rpc_.begin(endpoint, env_, request);
        if (status != STATUS_OK) { notice_ = ts_line("owner error {}", status); complete_scan(); }
    }
    void changed() {
        const auto target = page_ == 2 && authority_
            ? std::tuple{authority_->owner, authority_->id, authority_->incarnation,
                nav_.view == 1 && !deployment_ ? nav_.object : 0,
                nav_.view == 1 && !deployment_ ? nav_.selector : 0,
                nav_.view == 1 && !deployment_ ? nav_.grant : 0}
            : decltype(watched_){};
        if (target != watched_) {
            if (const auto status = watch_.close(); status != STATUS_OK) sys::exit(status);
            watched_ = target; watch_seq_ = grant_seq_ = watch_retry_ = 0;
        }
        if (!std::get<0>(target)) return;
        if (watch_.active()) {
            msg_frame message;
            const auto status = watch_.poll(message);
            if (status == STATUS_WOULD_BLOCK) return;
            if (status == STATUS_OK) {
                const auto previous = std::exchange(watch_seq_, message.value);
                uint64_t grant{};
                if (message.size == sizeof(grant)) ::memcpy(&grant, message.data, sizeof(grant));
                const auto previous_grant = std::exchange(grant_seq_, grant);
                if ((previous && previous != watch_seq_) || (previous_grant && previous_grant != grant_seq_)) {
                    notice_ = "Source changed; updating authorized facts"; rescan_ = true; redraw_ = true;
                    if (!scanning_) refresh_ = std::min(refresh_, rt_after_ms(200).value_or(0));
                }
            } else if (status != STATUS_TIMED_OUT) {
                if (status == STATUS_CLOSED || status == STATUS_STALE || status == STATUS_NOT_FOUND) {
                    rescan_ = true; refresh_ = std::min(refresh_, rt_after_ms(200).value_or(0));
                }
                notice_ = ts_line("Watch unavailable {}; r rescans", status); redraw_ = true;
                watch_retry_ = rt_after_ms(1000).value_or(UINT64_MAX);
            }
        }
        if (sys::clock_now().value < watch_retry_) return;
        auto request = mg_query(mg_op::Changed, std::get<1>(target),
            {.incarnation=std::get<2>(target), .argument=std::get<3>(target), .epoch=watch_seq_,
             .cursor=grant_seq_, .selector=std::get<4>(target), .selected=std::get<5>(target)});
        request.deadline = rt_after_ms(5000).value_or(0);
        const auto status = watch_.begin(std::get<0>(target), env_, request);
        if (status != STATUS_OK) {
            notice_ = ts_line("Watch error {}", status); redraw_ = true;
            watch_retry_ = rt_after_ms(1000).value_or(UINT64_MAX);
        }
    }
    void reply() {
        if (!rpc_.active()) return;
        msg_frame message; const auto status = rpc_.poll(message);
        if (status == STATUS_WOULD_BLOCK) return;
        if (!acting_ && request_gen_ != generation_) { scanning_ = false; refresh_ = 0; return; }
        if (acting_) {
            notice_ = (status == STATUS_OK || status == STATUS_PENDING) ? "Accepted; observing actual state" : status == STATUS_STALE ? "Stale target; refresh without replay" : ts_line("action error {}", status);
            action_.reset(); acting_ = false; refresh(); return;
        }
        if (status != STATUS_OK) {
            if (scan_page_ == 2 && !scan_dependencies_ && !scan_deployed_ && deployment_ && !nav_.object
                && nav_.view == 0 && request_op_ == mg_op::Caps && status == STATUS_NOT_FOUND) {
                scan_dependencies_ = scan_deployed_ = true; cursor_ = epoch_ = 0; return;
            }
            if (status == STATUS_STALE) {
                notice_ = "Stale sample; resampling without replacing selection";
                scanning_ = false; refresh_ = rt_after_ms(500).value_or(0); redraw_ = true; return;
            }
            else if (status != STATUS_NOT_FOUND && status != STATUS_INVALID_OP) notice_ = ts_line("owner snapshot error {}", status);
            if (scan_page_ == 0 && ++owner_ < owners_.size()) cursor_ = 0;
            else complete_scan();
            return;
        }
        auto unpack = [&]<class T>() { T record; ::memcpy(&record, message.data, sizeof(record)); return record; };
        if (scan_page_ == 0 && message.size == sizeof(mg_task)) {
            auto info = unpack.template operator()<mg_task>(); ts_task row{info, owners_[owner_], message.deadline};
            for (const auto& old : tasks_)
                if (old.owner == row.owner && old.info.id == info.id && old.info.incarnation == info.incarnation
                    && old.info.runtime != UINT64_MAX && info.runtime != UINT64_MAX && row.sampled > old.sampled && info.runtime >= old.info.runtime) {
                    row.cpu = (info.runtime - old.info.runtime) * 1000 / (row.sampled - old.sampled); row.measured = true;
                }
            incoming_.push_back(row); cursor_ = info.id;
        } else if (scan_page_ == 1 && message.size == sizeof(mg_device)) {
            auto row = unpack.template operator()<mg_device>(); device_scan_.push_back(row); cursor_ = row.id;
        } else if (scan_page_ == 2) {
            if (scan_dependencies_ && message.size == sizeof(mg_dependency)) {
                fact_scan_.emplace_back(unpack.template operator()<mg_dependency>()); cursor_ = message.value;
            } else if (scan_view_ && message.size == sizeof(CapView)) {
                ceiling_ = unpack.template operator()<CapView>(); scan_view_ = false; cursor_ = epoch_ = 0;
            } else if (scan_info_ && message.size == sizeof(ScopeObject)) {
                detail_ = unpack.template operator()<ScopeObject>(); scan_info_ = false; cursor_ = epoch_ = 0;
            } else if (request_op_ == mg_op::Usage || request_op_ == mg_op::Ops) {
                if (message.size == sizeof(AccountInfo)) fact_scan_.emplace_back(unpack.template operator()<AccountInfo>());
                else if (message.size == sizeof(mg_domain)) fact_scan_.emplace_back(unpack.template operator()<mg_domain>());
                else if (message.size == sizeof(mg_operation)) fact_scan_.emplace_back(unpack.template operator()<mg_operation>());
                else notice_ = "Invalid typed usage reply";
                complete_scan(); return;
            } else if (request_op_ == mg_op::Caps && message.size == sizeof(mg_cap)) {
                auto cap = mg_cap_info(message); fact_scan_.emplace_back(cap); cursor_ = cap.selector; epoch_ = message.value;
            } else {
                if (request_op_ == mg_op::Members && message.size == sizeof(ScopeMember)) fact_scan_.emplace_back(unpack.template operator()<ScopeMember>());
                else if (request_op_ == mg_op::Links && message.size == sizeof(LinkRecord)) fact_scan_.emplace_back(unpack.template operator()<LinkRecord>());
                else if (request_op_ == mg_op::Grants && message.size == sizeof(mg_grant)) fact_scan_.emplace_back(unpack.template operator()<mg_grant>());
                else if (message.size) { notice_ = "Invalid typed owner reply"; complete_scan(); return; }
                cursor_ = message.deadline; epoch_ = message.value;
            }
        } else { notice_ = "Invalid owner reply"; complete_scan(); return; }
        if (incoming_.size() + device_scan_.size() + fact_scan_.size() >= 1024) { notice_ = "Truncated: narrow the observed scope"; complete_scan(); }
    }
    void submit(cap_t owner, msg_frame request) {
        if (action_) { notice_ = "Action pending"; return; }
        request.deadline = rt_after_ms(300).value_or(0); action_ = action{owner, request}; notice_ = "Submitting"; redraw_ = true;
    }
    void expand() {
        const auto i = index();
        if (page_ != 2 || i >= facts_.size()) return;
        std::visit([&](const auto& row) {
            using T = std::decay_t<decltype(row)>;
            if constexpr (std::same_as<T, CapInfo>) navigate({1, nav_.object, row.selector, 0, row.view.rights});
            else if constexpr (std::same_as<T, mg_grant>) {
                if (i == 0) { if (row.object_id) navigate({2, row.object_id}); }
                else navigate({1, nav_.object, nav_.selector, row.grant_id, nav_.rights});
            } else if constexpr (std::same_as<T, ScopeMember> || std::same_as<T, LinkRecord>) {
                if (row.object_id) navigate({2, row.object_id}); else { notice_ = "Boundary: no authorized target to follow"; redraw_ = true; }
            }
        }, facts_[i]);
    }
    void key(ftxui::Event event) {
        if (filtering_) {
            if (event == ftxui::Event::Escape || event == ftxui::Event::Return) { filtering_ = false; list_ui_->TakeFocus(); }
            else { filter_ui_->OnEvent(event); selection_.reset(); rows(); }
            redraw_ = true; return;
        }
        const auto ch = event.is_character() && event.character().size() == 1 ? event.character()[0] : 0;
        if (ch >= '1' && ch <= '3') {
            if (ch == '3' && page_ == 0 && index() < tasks_.size()) {
                const auto& row = tasks_[index()];
                authority_ = ts_scope{row.owner, row.info.id, row.info.incarnation, row.info.actions, row.info.name};
                nav_ = {}; history_.clear();
            }
            if (ch == '3' && page_ == 1 && index() < devices_.size()) {
                const auto& row = devices_[index()];
                authority_ = ts_scope{env_.selector(mg_devices), row.id, row.incarnation, mg_observe, row.name};
                nav_ = {2}; history_.clear();
            }
            if (ch == '3' && !authority_) notice_ = "Select a task on Tasks, then press 3";
            page_ = ch - '1'; filter_.clear(); selection_.reset(); refresh(true); return;
        }
        if (ch == 'f' || ch == '/') { filtering_ = true; filter_ui_->TakeFocus(); redraw_ = true; return; }
        if (ch == 'o' && page_ != 2) { sort_ = (sort_ + 1) % (page_ == 1 ? 2 : 3); rows(); return; }
        if (event == ftxui::Event::CtrlL) { refresh(); return; }
        if (page_ == 2) {
            if ((ch == 'a' || ch == 'w') && detail_) {
                const auto object = ch == 'a' ? detail_->account_id : detail_->owner_id;
                if (object) navigate({2, object});
                else { notice_ = "Boundary: endpoint outside authorized scope"; redraw_ = true; }
                return;
            }
            if (ch == 't' && detail_ && detail_->kind == ObjDomain) { nav_.usage = !nav_.usage; refresh(); return; }
            if (ch == 'r') { refresh(); return; }
            if (ch == 'l') { deployment_ = !deployment_; refresh(); return; }
            if (ch == 'h') {
                if (!history_.empty()) { nav_ = history_.back(); history_.pop_back(); refresh(true); }
                else { page_ = 0; selection_.reset(); refresh(); }
                return;
            }
            if (ch == 'v') {
                auto next = ts_nav{(nav_.view + 1) % 3}; const auto i = index();
                if (i < facts_.size()) if (const auto* cap = std::get_if<CapInfo>(&facts_[i])) {
                    if (next.view == 1) next = {1, nav_.object, cap->selector, 0, cap->view.rights};
                    else if (next.view == 2) next.object = cap->object_id;
                }
                if (next.view == 1 && !next.selector) { notice_ = "Select a holder slot to inspect its Grant branch"; redraw_ = true; return; }
                navigate(next); return;
            }
            if (ch == 'b' && !nav_.object && authority_ && (authority_->actions & mg_control) && (nav_.rights & RIGHT_REVOKE)) {
                const auto i = index();
                if (i < facts_.size()) if (const auto* grant = std::get_if<mg_grant>(&facts_[i]))
                    submit(authority_->owner, mg_query(mg_op::Revoke, authority_->id,
                        {.incarnation=authority_->incarnation, .selector=nav_.selector, .selected=grant->grant_id}));
                return;
            }
            if (event == ftxui::Event::Return) { expand(); return; }
        }
        const auto i = index();
        if (ch == 'p' || ch == 'c' || ch == 'x' || ch == 's' || ch == 'r') {
            const auto action = ch == 'p' ? mg_action::Pause : ch == 'c' ? mg_action::Resume : ch == 'x' ? mg_action::Stop : ch == 's' ? mg_action::Start : mg_action::Restart;
            if (page_ == 0 && i < tasks_.size()) {
                const auto& row = tasks_[i];
                if (row.info.actions & (1U << (3 + static_cast<unsigned>(action)))) submit(row.owner, mg_request(mg_op::Action, row.info.id, row.info.incarnation, 0, action));
            } else if (page_ == 1 && i < devices_.size()) {
                const auto& row = devices_[i];
                if (row.actions & (1U << (3 + static_cast<unsigned>(action)))) submit(env_.selector(mg_devices), mg_request(mg_op::DeviceAction, row.id, row.incarnation, 0, action));
            } else if (page_ == 2 && ch == 'x' && i < facts_.size() && authority_ && (authority_->actions & mg_close)) {
                if (const auto* cap = std::get_if<CapInfo>(&facts_[i]); cap && !nav_.object)
                    submit(authority_->owner, mg_request(mg_op::CloseCap, authority_->id, authority_->incarnation, cap->selector));
            }
        }
        if (ch == 'j') event = ftxui::Event::ArrowDown;
        if (ch == 'k') event = ftxui::Event::ArrowUp;
        if (ch == 'd') event = ftxui::Event::PageDown;
        if (ch == 'u') event = ftxui::Event::PageUp;
        list_ui_->OnEvent(event);
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown || event == ftxui::Event::Home || event == ftxui::Event::End || event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
            if (selected_ >= 0 && static_cast<size_t>(selected_) < visible_.size()) selection_ = token(visible_[selected_]);
        redraw_ = true;
    }
    auto render() -> ftxui::Element {
        const auto i = index(); uint32_t rights{};
        std::string details = i == SIZE_MAX && selection_ ? "Selection gone; navigate to choose another target" : "";
        if (page_ == 0 && i < tasks_.size()) {
            const auto& row = tasks_[i]; rights = row.info.actions;
            details = ts_line("id {} incarnation {} owner {} ticks {}", row.info.id, row.info.incarnation, row.info.owner, row.sampled);
        } else if (page_ == 1 && i < devices_.size()) {
            const auto& row = devices_[i]; rights = row.actions;
            details = ts_line("driver {} MMIO {:x}+{} incarnation {}", row.driver, row.mmio, row.bytes, row.incarnation);
        } else if (page_ == 2 && i < facts_.size()) {
            details = fact_text(i);
            if (const auto* cap = std::get_if<CapInfo>(&facts_[i])) {
                details = ts_line("Slot {:x}  Grant {:x}  Object {:x}  {}",cap->selector,cap->grant_id,cap->object_id,ts_kind(cap->view.kind));
                if (authority_ && !nav_.object) rights = authority_->actions & mg_close;
            }
        }
        std::string extra;
        if (page_ == 2 && i < facts_.size()) if (const auto* cap = std::get_if<CapInfo>(&facts_[i]))
            extra = ts_rights(cap->view.rights) + ts_line(" | range {:x} {:x} {:x}",cap->view.words[0],cap->view.words[1],cap->view.words[2]);
        if (page_ == 2 && detail_) {
            if (!extra.empty()) extra += " | ";
            extra += ts_line("Owner {} @{}  Payer {} @{}  storage {}B / {} caps", detail_->owner_id, detail_->owner_epoch, detail_->account_id, detail_->billing_epoch, detail_->storage_memory, detail_->storage_caps);
        }
        if (page_ == 2 && i < facts_.size()) if (const auto* domain = std::get_if<mg_domain>(&facts_[i]))
            extra = ts_line("CPU budget {}/{} avail {} config {}/{} phase {} status {}", domain->budget, domain->period, domain->available, domain->applied, domain->requested, domain->phase, domain->status);
        if (page_ == 2 && ceiling_) extra = ts_line("Branch ceiling {:x} {:x} {:x} {:x} {:x} {:x}", ceiling_->words[0], ceiling_->words[1], ceiling_->words[2], ceiling_->words[3], ceiling_->words[4], ceiling_->words[5]);
        std::string actions;
        for (const auto& [bit, name] : std::array<std::pair<uint32_t, const char*>, 5>{{{mg_pause, "p pause  "}, {mg_resume, "c continue  "}, {mg_stop, "x stop  "}, {mg_start, "s start  "}, {mg_restart, "r restart  "}}})
            if (rights & bit) actions += name;
        if (page_ == 2) actions += "a payer  w owner  l deps  ";
        if (page_ == 2 && detail_ && detail_->kind == ObjDomain) actions += "t stats  ";
        if (page_ == 2 && (rights & mg_close)) actions += "x Close slot  ";
        if (page_ == 2 && authority_ && (authority_->actions & mg_control) && !nav_.object && nav_.view == 1 && (nav_.rights & RIGHT_REVOKE)) actions += "b Revoke branch";
        auto scope = page_ == 2 && authority_ ? ts_line("Scope: {} | {}", authority_->name, ts_views[nav_.view]) : "Scope: explicitly granted owners";
        if (page_ == 2 && detail_) scope += ts_line(" | {} o{:x} owner {} payer {}{}", ts_kind(detail_->kind), detail_->object_id, detail_->owner_id, detail_->account_id, detail_->flags & OBS_BOUNDARY ? " boundary" : "");
        if (page_ == 2 && detail_ && detail_->gate != OBJECT_OPEN) scope += " Closing";
        return ftxui::vbox({
            ftxui::hbox({ftxui::text("myos tasks ") | ftxui::bold, tabs_ui_->Render()}),
            ftxui::text(scope + (scanning_ ? " | updating" : " | sample")) | ftxui::dim,
            ftxui::hbox({ftxui::text("Filter: "), filter_ui_->Render()}) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1),
            ftxui::separator(), table() | ftxui::bold | ftxui::color(ftxui::Color::Cyan),
            list_ui_->Render() | ftxui::vscroll_indicator | ftxui::frame | ftxui::flex,
            ftxui::separator(), ftxui::paragraph(details), ftxui::paragraph(extra), ftxui::text(notice_), ftxui::paragraph(actions),
            ftxui::text(page_ == 2 ? "j/k move Enter follow h back v view f filter r scan q quit" : "j/k select  f search  o sort  1/2/3 page  q quit") | ftxui::dim});
    }
public:
    explicit ts_view(const rt_env& env) : env_(env), owners_{env.selector(mg_tasks), env.selector(mg_jobs)} {
        auto tabs = ftxui::MenuOption::Horizontal();
        tabs.entries_option.transform = [](const ftxui::EntryState& state) {
            auto label = ftxui::text(" " + state.label + " ");
            return state.state ? label | ftxui::inverted | ftxui::bold : label | ftxui::dim;
        };
        tabs_ui_ = ftxui::Menu(&tabs_, &page_, tabs);
        filter_ui_ = ftxui::Input(&filter_, "name, type or relation");
        auto options = ftxui::MenuOption::Vertical();
        options.entries_option.transform = [this](const ftxui::EntryState& state) {
            auto row = table(visible_[state.index]);
            if (selection_ && token(visible_[state.index]) == *selection_) row |= ftxui::inverted;
            return row;
        };
        list_ui_ = ftxui::Menu(&labels_, &selected_, options);
        ui_ = ftxui::Renderer(ftxui::Container::Vertical({tabs_ui_, filter_ui_, list_ui_}), [this] { return render(); });
        list_ui_->TakeFocus();
    }
    auto run() -> status_t {
        if (const auto status = terminal_.open(env_); status != STATUS_OK) return status;
        bool focused = terminal_.focused();
        for (;;) {
            changed(); reply(); query(); reply();
            size_t n{};
            for (; n < 64; ++n) {
                const auto input = terminal_.read();
                if (!input) return input.error();
                if (terminal_.redraw()) redraw_ = true;
                if (std::exchange(focused, terminal_.focused()) != terminal_.focused() && focused) refresh();
                if (!*input) break;
                if (!filtering_ && **input == ftxui::Event::Character('q')) return STATUS_OK;
                key(**input);
            }
            if (terminal_.deadline() && sys::clock_now().value >= terminal_.deadline()) redraw_ = true;
            if (terminal_.focused() && std::exchange(redraw_, false))
                if (const auto status = terminal_.present(ui_); status != STATUS_OK) return status;
            // A completed page or input batch continues immediately; pending
            // requests sleep on their actual Port sources and deadlines.
            if (n == 64 || (!rpc_.active() && (action_ || scanning_ || sys::clock_now().value >= refresh_))) continue;
            auto next = std::min({rpc_.active() ? UINT64_MAX : refresh_, rpc_.deadline(), watch_.deadline()});
            if (watch_retry_ > sys::clock_now().value) next = std::min(next, watch_retry_);
            if (terminal_.deadline()) next = std::min(next, terminal_.deadline());
            const auto status = terminal_.wait(next == UINT64_MAX ? 0 : next);
            if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
        }
    }
};
int main() { return ts_view{rt_startup()}.run(); }
