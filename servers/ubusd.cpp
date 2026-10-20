#include <ubus/ubus.hpp>
#include <map>

struct dir_view {
    uint64_t id{}, parent{}, rights{}, quota{}, used{};
    std::string prefix;
    cap_owner lifetime;
    ev_watch watch;
};
struct dir_entry {
    uint64_t id{}, owner{}, version{};
    std::string name, interface;
    cap_owner connector;
};
struct dir_wait {
    uint64_t owner{}, epoch{};
    ubus_request request;
    ev_watch terminal;
};
static std::map<uint64_t, std::unique_ptr<dir_view>> views;
static std::map<uint64_t, dir_entry> entries;
struct dir_role { uint64_t instance, owner; };
static std::map<std::string, dir_role, std::less<>> roles;
static std::vector<dir_wait> waiters;
static uint64_t generation{1}, next_view{1}, next_entry{};
static ubus_object directory{"ubus.directory"};

static auto view_of(const ubus_request& request) -> dir_view* {
    const auto found = views.find(((request.badge - 2) / 2) >> 8);
    return found == views.end() ? nullptr : found->second.get();
}
static auto path(const dir_view& view, std::string_view name) -> std::optional<std::string> {
    if (name.empty() || name.front() == '/' || name.back() == '/') return {};
    for (size_t begin = 0; begin < name.size();) {
        const auto slash = name.find('/', begin);
        const auto end = slash == name.npos ? name.size() : slash;
        const auto component = name.substr(begin, end - begin);
        if (component.empty() || component == "." || component == ".." || component.find('\0') != component.npos) return {};
        begin = end + 1;
    }
    return view.prefix + std::string{name};
}
static auto visible(const dir_view& view, const dir_entry& entry) -> bool { return entry.name.starts_with(view.prefix); }
static auto metadata(const dir_view& view, const dir_entry& entry, std::string_view name = {}) -> ubus_data {
    if (name.empty()) name = entry.name;
    ubus_data value;
    value.put("instance", entry.id).put("name", name.substr(view.prefix.size()))
        .put("interface", entry.interface).put("version", entry.version);
    return value;
}
static void withdraw(std::map<uint64_t, dir_entry>::iterator entry) {
    const auto status = sys::cap_revoke(entry->second.connector.selector()).status;
    if (status != STATUS_OK && status != STATUS_INVALID_CAP && status != STATUS_CLOSED) sys::exit(status);
    if (const auto owner = views.find(entry->second.owner); owner != views.end()) --owner->second->used;
    std::erase_if(roles, [&](const auto& role) {
        if (role.second.instance != entry->first) return false;
        if (auto owner = views.find(role.second.owner); owner != views.end()) --owner->second->used;
        return true;
    });
    entries.erase(entry); ++generation;
}
static auto publish(ubus_request& request, ubus_msg& response) -> status_t {
    auto* view = view_of(request);
    if (!view) return STATUS_DENIED;
    const auto name = path(*view, *request.data["name"].as<std::string_view>());
    if (!name) return STATUS_BAD_ARGS;
    if (view->used == view->quota || next_entry == UINT64_MAX) return STATUS_EXHAUSTED;
    auto connector = request.take(request.data["connector"]);
    const auto checked = sys::cap_duplicate(connector.selector(), rt_startup().cspace,
        RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE);
    if (checked.status != STATUS_OK) return checked.status;
    connector = cap_owner{checked.value};
    const auto source = sys::source_info(connector.selector(), SOURCE_WRITE);
    if (source.status != STATUS_OK || (source.value & SOURCE_TERMINAL)) return STATUS_BAD_ARGS;
    dir_entry entry{++next_entry, view->id, *request.data["version"].as<uint64_t>(), *name,
        std::string{*request.data["interface"].as<std::string_view>()}, std::move(connector)};
    response.data.put("instance", entry.id);
    entries.emplace(entry.id, std::move(entry)); ++view->used; ++generation;
    return STATUS_OK;
}
static auto resolve(ubus_request& request, ubus_msg& response) -> status_t {
    const auto* view = view_of(request);
    if (!view) return STATUS_DENIED;
    const auto name = path(*view, *request.data["name"].as<std::string_view>());
    if (!name) return STATUS_BAD_ARGS;
    const auto selected = request.data["instance"].as<uint64_t>();
    const dir_entry* match{};
    if (const auto role = roles.find(*name); role != roles.end() && !selected) {
        const auto entry = entries.find(role->second.instance);
        if (entry != entries.end()) match = &entry->second;
    } else for (const auto& [id, entry] : entries) {
        if (entry.name != *name || (selected && id != *selected)) continue;
        if (match) return STATUS_BUSY;
        match = &entry;
    }
    if (!match) return STATUS_NOT_FOUND;
    response.data = metadata(*view, *match, *name);
    response.data.cap("connector", *response.attach(match->connector.selector(), RIGHT_SEND | RIGHT_DUPLICATE));
    return STATUS_OK;
}
static auto list(ubus_request& request, ubus_msg& response) -> status_t {
    const auto* view = view_of(request);
    if (!view) return STATUS_DENIED;
    uint64_t cursor{};
    const auto state = request.data["cursor"];
    if (state && !state.data().empty()) {
        const auto epoch = state["epoch"].as<uint64_t>(), index = state["index"].as<uint64_t>();
        if (!epoch || !index) return STATUS_BAD_ARGS;
        if (*epoch != generation) return STATUS_STALE;
        cursor = *index;
    }
    const auto limit = request.data["limit"].as<uint64_t>().value_or(std::min<uint64_t>(view->quota, 64));
    if (!limit || limit > view->quota) return STATUS_BAD_ARGS;
    struct row { std::string_view name; const dir_entry* entry; };
    std::vector<row> rows;
    for (const auto& [id, entry] : entries) if (visible(*view, entry)) rows.push_back({entry.name, &entry});
    for (const auto& [name, role] : roles) if (name.starts_with(view->prefix))
        if (const auto entry = entries.find(role.instance); entry != entries.end()) rows.push_back({name, &entry->second});
    std::ranges::sort(rows, [](const auto& a, const auto& b) {
        return a.name == b.name ? a.entry->id < b.entry->id : a.name < b.name;
    });
    rows.erase(std::unique(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.name == b.name && a.entry->id == b.entry->id;
    }), rows.end());
    if (cursor > rows.size()) return STATUS_BAD_ARGS;
    const auto end = cursor + std::min<uint64_t>(limit, rows.size() - cursor);
    ubus_data records{ubus_type::Array};
    for (size_t i = cursor; i < end; ++i) records.push(metadata(*view, *rows[i].entry, rows[i].name));
    response.data.put("epoch", generation).put("items", records);
    if (end != rows.size()) { ubus_data next; next.put("epoch", generation).put("index", end); response.data.put("cursor", next); }
    return STATUS_OK;
}
static auto unpublish(ubus_request& request, ubus_msg&) -> status_t {
    const auto* view = view_of(request);
    if (!view) return STATUS_DENIED;
    const auto entry = entries.find(*request.data["instance"].as<uint64_t>());
    if (entry == entries.end()) return STATUS_NOT_FOUND;
    if (entry->second.owner != view->id && !(view->rights & ubus_manage)) return STATUS_DENIED;
    if (!visible(*view, entry->second)) return STATUS_DENIED;
    withdraw(entry); return STATUS_OK;
}
static auto role(ubus_request& request, ubus_msg&) -> status_t {
    const auto* view = view_of(request);
    if (!view) return STATUS_DENIED;
    const auto name = path(*view, *request.data["name"].as<std::string_view>());
    if (!name) return STATUS_BAD_ARGS;
    const auto instance = *request.data["instance"].as<uint64_t>();
    const auto current = roles.find(*name);
    if (!instance) {
        if (current != roles.end()) { --views.find(current->second.owner)->second->used; roles.erase(current); ++generation; }
        return STATUS_OK;
    }
    const auto entry = entries.find(instance);
    if (entry == entries.end() || !visible(*view, entry->second)) return STATUS_NOT_FOUND;
    if (current == roles.end()) {
        if (view->used == view->quota) return STATUS_EXHAUSTED;
        roles.emplace(*name, dir_role{instance, view->id}); ++views.find(view->id)->second->used;
    } else current->second.instance = instance;
    ++generation; return STATUS_OK;
}
static auto new_view(ubus_request& request, ubus_msg& response) -> status_t {
    auto* parent = view_of(request);
    if (!parent) return STATUS_DENIED;
    const auto rights = *request.data["rights"].as<uint64_t>(), quota = *request.data["quota"].as<uint64_t>();
    const auto prefix = *request.data["prefix"].as<std::string_view>();
    const auto qualified = prefix.empty() ? std::optional{parent->prefix} : path(*parent, prefix);
    if (!qualified || (rights & ~parent->rights) || !quota) return STATUS_BAD_ARGS;
    if (quota > parent->quota - parent->used || next_view == (UINT64_MAX >> 9)) return STATUS_EXHAUSTED;
    auto view = std::make_unique<dir_view>();
    view->id = ++next_view; view->parent = parent->id; view->rights = rights; view->quota = quota;
    view->prefix = *qualified + (prefix.empty() ? "" : "/");
    view->lifetime = request.take(request.data["lifetime"]);
    const auto state = sys::scope_info(view->lifetime.selector());
    if (state.status != STATUS_OK) return state.status;
    const auto source = sys::source_info(view->lifetime.selector());
    if (source.status != STATUS_OK || !(source.value & STATE_OPEN)) return STATUS_CLOSED;
    auto status = view->watch.open(rt_startup().events, view->lifetime.selector(), STATE_CLOSING | STATE_DRAINED);
    if (status != STATUS_OK) return status;
    auto connector = directory.connector((view->id << 8) | rights);
    if (!connector) return connector.error();
    response.data.cap("connector", *response.offer(std::move(*connector), RIGHT_SEND | RIGHT_DUPLICATE));
    parent->used += quota; views.emplace(view->id, std::move(view)); ++generation;
    return STATUS_OK;
}
static auto changed(ubus_request& request, ubus_msg& response) -> status_t {
    auto* view = view_of(request);
    if (!view) return STATUS_DENIED;
    response.data.put("epoch", generation);
    const auto seen = *request.data["epoch"].as<uint64_t>();
    if (seen != generation) return STATUS_OK;
    if (view->used == view->quota) return STATUS_EXHAUSTED;
    dir_wait waiter; waiter.owner = view->id; waiter.epoch = seen;
    const auto status = waiter.terminal.open(rt_startup().events, request.reply.selector(), SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
    if (status != STATUS_OK) return status;
    waiter.request = std::move(request); waiters.push_back(std::move(waiter)); ++view->used;
    return STATUS_PENDING;
}
static void retire(uint64_t id) {
    for (;;) {
        const auto child = std::ranges::find_if(views, [&](const auto& view) { return view.second->parent == id; });
        if (child == views.end()) break;
        retire(child->first);
    }
    for (auto entry = entries.begin(); entry != entries.end();) {
        const auto current = entry++;
        if (current->second.owner == id) withdraw(current);
    }
    std::erase_if(roles, [&](const auto& role) { return role.second.owner == id; });
    auto view = views.find(id);
    if (view->second->parent) views.find(view->second->parent)->second->used -= view->second->quota;
    views.erase(view); ++generation;
}
static auto reap() -> word_t {
    for (auto view = views.begin(); view != views.end();) {
        const auto state = sys::source_info(view->second->lifetime.selector());
        if (state.status != STATUS_OK || !(state.value & STATE_OPEN)) { retire(view->first); view = views.begin(); }
        else ++view;
    }
    const auto now = sys::clock_now().value;
    word_t deadline{};
    for (auto waiter = waiters.begin(); waiter != waiters.end();) {
        const auto owner = views.find(waiter->owner);
        const auto state = sys::source_info(waiter->request.reply.selector(), SOURCE_WRITE);
        status_t status = owner == views.end() ? STATUS_CLOSED : state.status != STATUS_OK || (state.value & SOURCE_TERMINAL)
            ? STATUS_CANCELED : waiter->epoch != generation ? STATUS_OK
            : waiter->request.deadline && now >= waiter->request.deadline ? STATUS_TIMED_OUT : STATUS_PENDING;
        if (status != STATUS_PENDING) {
            if (status != STATUS_CANCELED) {
                ubus_msg response; response.status = status; response.data.put("epoch", generation);
                (void)waiter->request.respond(std::move(response));
            }
            if (owner != views.end()) --owner->second->used;
            waiter = waiters.erase(waiter);
        } else { if (waiter->request.deadline) deadline = !deadline ? waiter->request.deadline : std::min(deadline, waiter->request.deadline); ++waiter; }
    }
    return deadline;
}
int main() {
    const auto& env = rt_startup();
    const auto bootstrap = env.selector(ubus_boot);
    ubus_msg config;
    auto status = ubus_receive_event(bootstrap, config, true);
    if (status != STATUS_OK) return status;
    const auto quota = config.data["quota"].as<uint64_t>();
    if (!quota || !*quota) return STATUS_BAD_ARGS;
    auto root = std::make_unique<dir_view>(); root->id = 1; root->rights = ubus_all; root->quota = *quota;
    const auto lease = sys::cap_duplicate(env.selector(ubus_owner), env.cspace, RIGHT_INSPECT | RIGHT_DUPLICATE);
    if (lease.status != STATUS_OK) return lease.status;
    root->lifetime = cap_owner{lease.value};
    status = root->watch.open(env.events, lease.value, STATE_CLOSING | STATE_DRAINED);
    if (status != STATUS_OK) return status;
    views.emplace(1, std::move(root));
    for (const auto result : {
        directory.add("Publish", {{"name", ubus_type::String}, {"connector", ubus_type::Cap}, {"interface", ubus_type::String}, {"version", ubus_type::U64}}, {{"instance", ubus_type::U64}}, publish, ubus_publish),
        directory.add("Resolve", {{"name", ubus_type::String}, {"instance", ubus_type::U64, true}}, {{"instance", ubus_type::U64}, {"name", ubus_type::String}, {"interface", ubus_type::String}, {"version", ubus_type::U64}, {"connector", ubus_type::Cap}}, resolve, ubus_resolve),
        directory.add("List", {{"cursor", ubus_type::Record, true}, {"limit", ubus_type::U64, true}}, {{"epoch", ubus_type::U64}, {"cursor", ubus_type::Record, true}, {"items", ubus_type::Array}}, list, ubus_list),
        directory.add("Withdraw", {{"instance", ubus_type::U64}}, {}, unpublish, ubus_publish),
        directory.add("Role", {{"name", ubus_type::String}, {"instance", ubus_type::U64}}, {}, role, ubus_manage),
        directory.add("View", {{"prefix", ubus_type::String}, {"rights", ubus_type::U64}, {"quota", ubus_type::U64}, {"lifetime", ubus_type::Cap}}, {{"connector", ubus_type::Cap}}, new_view, ubus_view),
        directory.add("Changed", {{"epoch", ubus_type::U64}}, {{"epoch", ubus_type::U64}}, changed, ubus_wait)})
        if (result != STATUS_OK) return result;
    if ((status = directory.open()) != STATUS_OK) return status;
    auto connector = directory.connector((1 << 8) | ubus_all);
    if (!connector) return connector.error();
    ubus_msg ready; ready.data.cap("connector", *ready.offer(std::move(*connector), RIGHT_SEND | RIGHT_DUPLICATE));
    if ((status = ubus_send(bootstrap, ready)) != STATUS_OK) return status;
    for (;;) {
        const auto deadline = reap();
        status = directory.poll();
        if (status == STATUS_WOULD_BLOCK) status = ev_wait(env.events, deadline).status;
        if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    }
}
