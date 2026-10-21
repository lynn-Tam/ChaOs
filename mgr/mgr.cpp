#include "mgr.hpp"
#include <set>

auto mgr_row(proc_task* task, uint64_t id, uint64_t gen, std::string_view name,
             std::string_view state, bool control) -> ubus_data {
    ubus_data row;
    row.put("id", id).put("gen", gen).put("name", name).put("state", state).put("control", control);
    if (task) {
        if (const auto thread = task->info()) row.put("runtime", thread->runtime);
        if (const auto memory = task->memory_info()) row.put("resident", memory->resident);
        if (const auto quota = task->resource_info())
            row.put("charged", quota->memory_limit - quota->memory_available).put("limit", quota->memory_limit);
    }
    return row;
}
static auto mgr_text(ubus_value value) -> std::string_view { return value.as<std::string_view>().value_or(""); }
static auto mgr_name(std::string_view text) -> bool {
    return !text.empty() && text.size() <= 255 && text.find('\0') == text.npos;
}
auto mgr::add(std::string name, std::string image, proc_quota resources) -> mgr_child& {
    auto child = std::make_unique<mgr_child>();
    child->id = ++next_; child->name = std::move(name); child->image = std::move(image);
    child->args.push_back(child->name); child->resources = resources;
    const auto& env = rt_startup();
    child->auth = {env.alloc(), static_cast<uint32_t>(env.cpus), env.domain};
    child->cpu = (child->id - 1) % env.cpus;
    children_.push_back(std::move(child)); ++epoch_; return *children_.back();
}
auto mgr::find(uint64_t id) -> mgr_child* {
    for (const auto& child : children_) if (child->id == id) return child.get();
    return nullptr;
}
auto mgr::find(std::string_view name) -> mgr_child* {
    for (const auto& child : children_) if (child->name == name) return child.get();
    return nullptr;
}
auto mgr::environment(word_t token) -> mgr_child* {
    for (const auto& child : children_)
        if (token && child->environment == token && child->task && child->state != "draining") return child.get();
    return nullptr;
}
static auto mgr_own(ubus_msg& message) -> status_t {
    for (size_t i = 0; i < message.count; ++i) if (!message.caps[i]) {
        const auto cap = sys::cap_duplicate(message.transfers[i].source, rt_startup().cspace, message.transfers[i].rights);
        if (cap.status != STATUS_OK) return cap.status;
        message.caps[i] = cap_owner{cap.value};
    }
    return STATUS_OK;
}
auto mgr::configure(ubus_msg recipe, bool spawn, bool validate) -> std::expected<mgr_child*, status_t> {
    const auto name = mgr_text(recipe.data["name"]), program = mgr_text(recipe.data["program"]);
    if (!mgr_name(name) || !mgr_name(program)) return std::unexpected(STATUS_BAD_ARGS);
    auto* child = find(name);
    if (child && child->task) return std::unexpected(STATUS_BUSY);
    proc_quota q{.bytes=4 * 1024 * 1024, .caps=256, .slots=256, .pages=36};
    const auto limits = recipe.data["resources"];
    q.bytes = limits["bytes"].as<uint64_t>().value_or(q.bytes);
    q.caps = limits["caps"].as<uint64_t>().value_or(q.caps);
    q.kinds = limits["kinds"].as<uint64_t>().value_or(q.kinds);
    q.slots = limits["slots"].as<uint64_t>().value_or(q.caps);
    q.pages = limits["pages"].as<uint64_t>().value_or((q.slots + 7) / 8 + 4);
    q.watches = limits["watches"].as<uint64_t>().value_or(q.watches);
    q.hints = limits["hints"].as<uint64_t>().value_or(q.hints);
    q.max_bytes = limits["max_bytes"].as<uint64_t>().value_or(q.bytes);
    q.max_caps = limits["max_caps"].as<uint64_t>().value_or(q.caps);
    if (limits["children"].as<bool>().value_or(false)) q.pool_perms |= RIGHT_SPLIT;
    if (!q.bytes || !q.caps || q.max_bytes < q.bytes || q.max_caps < q.caps) return std::unexpected(STATUS_BAD_ARGS);
    std::vector<std::string> args{std::string{name}};
    for (const auto arg : recipe.data["argv"].values()) {
        const auto text = arg.as<std::string_view>();
        if (!text || text->find('\0') != text->npos) return std::unexpected(STATUS_BAD_ARGS);
        args.emplace_back(*text);
    }
    const auto routes = recipe.data["routes"], exports = recipe.data["exports"], setup = recipe.data["setup"];
    if ((routes && routes.type() != ubus_type::Record) || (exports && exports.type() != ubus_type::Array)
        || (setup && setup.type() != ubus_type::Record)) return std::unexpected(STATUS_BAD_ARGS);
    for (const auto item : exports.values()) if (!mgr_name(mgr_text(item))) return std::unexpected(STATUS_BAD_ARGS);
    std::vector<std::string> environ;
    if (const auto values = recipe.data["env"]; values && values.type() != ubus_type::Array) return std::unexpected(STATUS_BAD_ARGS);
    for (const auto entry : recipe.data["env"].values()) {
        const auto text = entry.as<std::string_view>();
        if (!text || text->find('\0') != text->npos) return std::unexpected(STATUS_BAD_ARGS);
        environ.emplace_back(*text);
    }
    const auto cpu = recipe.data["cpu"].as<uint32_t>();
    if (cpu && *cpu >= rt_startup().cpus) return std::unexpected(STATUS_BAD_ARGS);
    const auto reserved = std::ranges::count_if(pending_, [&](const auto& operation) {
        return (operation->operation == pending::Configure || operation->operation == pending::Spawn)
            && operation->name != name && !find(operation->name);
    });
    if (!child && children_.size() + size_t(reserved) >= max_pending_) return std::unexpected(STATUS_EXHAUSTED);
    if (validate) return child;
    if (const auto status = mgr_own(recipe); status != STATUS_OK) return std::unexpected(status);
    if (!child) child = &add(std::string{name}, std::string{program}, q);
    else {
        child->loading = {}; child->source = {}; child->snapshots.clear(); child->inbox.clear();
        child->image = program; child->resources = q; ++child->version;
    }
    child->state = "idle"; child->result = STATUS_OK;
    child->args = std::move(args); child->env = std::move(environ);
    child->cpu = cpu.value_or(child->cpu);

    child->routes = {}; if (routes) (void)child->routes.assign(routes.bytes());
    child->exports = ubus_data{ubus_type::Array}; if (exports) (void)child->exports.assign(exports.bytes());
    child->origin = {}; child->recipe = recipe.data;
    child->quota = recipe.data["quota"].as<uint64_t>().value_or(32);
    ubus_data message; if (setup) (void)message.assign(setup.bytes());
    recipe.data = std::move(message);
    child->setup = std::move(recipe); child->received = false; child->reusable = true;
    child->restart = child->recipe["restart"].as<bool>().value_or(false);
    child->desired = spawn || child->recipe["start"].as<bool>().value_or(false);
    ++epoch_;
    return child;
}
auto mgr::start(mgr_child& child) -> status_t {
    if (child.task) return STATUS_BUSY;
    const auto fail = [&](status_t status) {
        child.desired = false; child.state = "failed"; child.result = status;
        ++epoch_; return status;
    };
    if (!child.reusable || next_environment_ >= (UINT64_MAX - 2) / 8) return fail(STATUS_BUSY);
    if (const auto status = mgr_own(child.setup); status != STATUS_OK) return fail(status);
    child.desired = true; child.result = STATUS_OK;
    // The owner resolves the image and any per-execution grants before native
    // construction. Asynchronous loading remains owned by this deployment.
    auto image = [&]() -> std::expected<proc_image, status_t> {
        if (const auto buffer = child.recipe["image"].as<ubus_buffer>()) {
            const auto cap = child.setup.capability(child.recipe["image"]);
            if (!cap || !buffer->size || buffer->offset > UINT64_MAX - buffer->size - 4095) return std::unexpected(STATUS_BAD_ARGS);
            const auto copy = sys::cap_duplicate(cap, rt_startup().cspace, RIGHT_MAP);
            if (copy.status != STATUS_OK) return std::unexpected(copy.status);
            auto mapping = vm_area::map(rt_vm(), cap_owner{copy.value}, 0, (buffer->offset + buffer->size + 4095) & ~size_t{4095}, VM_READ);
            if (!mapping) return std::unexpected(mapping.error());
            child.source = std::move(*mapping);
            return proc_image{.bytes={reinterpret_cast<const std::byte*>(child.source.address + buffer->offset), size_t(buffer->size)}};
        }
        return child.loading ? child.loading() : load_(child);
    }();
    if (image || image.error() != STATUS_WOULD_BLOCK) child.loading = {};
    if (!image && image.error() == STATUS_WOULD_BLOCK) { child.state = "loading"; return STATUS_WOULD_BLOCK; }
    if (!image) return fail(image.error());
    if (!child.scope) {
        const auto created = sys::scope_create(child.auth.alloc);
        if (created.status != STATUS_OK) return fail(created.status);
        child.scope = cap_owner{created.value};
    }
    child.environment = ++next_environment_ * 4;
    child.received = false;
    auto door = object_.connector(child.environment);
    if (!door) return fail(door.error());
    child.task = std::make_unique<proc_task>();
    std::vector<std::string_view> args{child.args.begin(), child.args.end()}, env{child.env.begin(), child.env.end()};
    auto auth = child.auth; auth.alloc.scope = child.scope.selector();
    auto status = proc_spawn(*child.task, auth, *image,
        {.argv=args, .envp=env, .environment=door->selector(), .resources=child.resources, .cpu=child.cpu, .events=rt_startup().events});
    if (child.task->scope()) {
        const auto identity = sys::object_info(child.task->scope());
        if (identity.status == STATUS_OK) child.gen = reinterpret_cast<const ObjectInfo*>(rt_startup().ipc)->object_id;
        else if (status == STATUS_OK) status = identity.status;
    }
    if (status == STATUS_OK) status = child.task->start();
    child.state = status == STATUS_OK ? "starting" : "draining";
    if (status != STATUS_OK) { child.result = status; (void)child.task->stop(status); child.desired = false; }
    ++epoch_; return status;
}
void mgr::withdraw(mgr_child& child) {
    const auto removed = std::erase_if(publications_, [&](const auto& row) { return row.second.owner == child.id; });
    if (removed) ++epoch_;
}
auto mgr::erase(uint64_t id) -> status_t {
    auto* child = find(id); if (!child) return STATUS_NOT_FOUND;
    if (child->task) return STATUS_BUSY;
    if (child->scope) {
        const auto closed = sys::scope_close(child->scope.selector()); if (closed.status != STATUS_OK) return closed.status;
        const auto fact = sys::source_info(child->scope.selector()); if (fact.status != STATUS_OK) return fact.status;
        if (!(fact.value & STATE_DRAINED)) {
            const auto status = child->retired ? child->retired.arm(fact.value2).status
                : child->retired.open(rt_startup().events, child->scope.selector(), STATE_DRAINED);
            return status == STATUS_OK ? STATUS_WOULD_BLOCK : status;
        }
    }
    withdraw(*child);
    std::erase_if(children_, [&](const auto& item) { return item->id == id; }); ++epoch_; return STATUS_OK;
}
auto mgr::action(mgr_child& child, std::string_view action) -> status_t {
    if (std::ranges::any_of(pending_, [&](const auto& operation) {
        return (operation->operation >= pending::Configure) && operation->name == child.name;
    })) return STATUS_BUSY;
    if (action == "start" || action == "restart") {
        if (!child.task) { child.loading = {}; return start(child); }
        child.desired = true;
        if (action == "start") return STATUS_OK;
    } else if (action == "stop") child.desired = false;
    else if (action == "pause" || action == "resume") {
        if (!child.task || child.state == "draining") return STATUS_CLOSED;
        const auto status = child.task->execution(action == "resume").status;
        if (status == STATUS_OK) { child.state = action == "resume" ? "running" : "suspending"; ++epoch_; }
        return status;
    } else return STATUS_BAD_ARGS;
    if (!child.task) { child.loading = {}; child.state = "idle"; return STATUS_OK; }
    const auto status = child.task->stop();
    if (status == STATUS_OK) { child.state = "draining"; withdraw(child); ++epoch_; }
    return status;
}
auto mgr::cycle(uint64_t from, uint64_t target) const -> bool {
    std::set<uint64_t> reached;
    std::vector<uint64_t> work{target};
    while (!work.empty()) {
        const auto id = work.back(); work.pop_back();
        if (id == from) return true;
        if (!reached.insert(id).second) continue;
        for (const auto& wait : pending_)
            if (wait->operation == pending::Get && wait->source == id && wait->provider) work.push_back(wait->provider);
    }
    return false;
}
auto mgr::resolve(mgr_child* source, std::string_view name, ubus_msg& response, pending* wait) -> status_t {
    ubus_value rule;
    if (source) { rule = source->routes[name]; if (!rule) rule = source->routes["*"]; }
    if (source && !rule) return STATUS_NOT_FOUND;
    if (const auto rights = rule["manager"].as<uint64_t>()) {
        if (*rights & ~(mgr_observe | mgr_control)) return STATUS_BAD_ARGS;
        auto connector = object_.connector(*rights);
        if (!connector) return connector.error();
        response.data.cap("connector", *response.give(std::move(*connector))); return STATUS_OK;
    }
    mgr_child* local{};
    auto local_name = name;
    if (!source || rule["local"].as<bool>().value_or(false)) {
        const auto slash = name.find('/');
        if (slash != name.npos) { local = find(name.substr(0, slash)); local_name = name.substr(slash + 1); }
        else for (const auto& child : children_) for (const auto exported : child->exports.view().values())
            if (mgr_text(exported) == name) {
                if (local && local != child.get()) return STATUS_BUSY;
                local = child.get();
            }
    }
    const auto directory = source ? source->setup.capability(rule["directory"]) : 0;
    if ((rule["parent"] || directory) && !local) {
        const auto endpoint = directory ? directory : rt_startup().environment;
        if (!wait || !endpoint) return STATUS_NOT_FOUND;
        if (!wait->relay) {
            wait->relay = std::make_unique<ubus_conn>();
            const auto status = wait->relay->begin_bind(endpoint, {}, 0, UINT64_MAX, 0, wait->request.deadline);
            if (status != STATUS_OK) return status;
        }
        if (!wait->introduced) {
            auto status = wait->relay->poll_bind();
            if (status != STATUS_OK) return status;
            ubus_msg query; query.deadline = wait->request.deadline;
            const auto renamed = mgr_text(directory ? rule["name"] : rule["parent"]);
            query.data.put("name", renamed.empty() ? name : renamed);
            for (const auto field : {"interface", "minimum", "maximum"})
                if (const auto value = wait->request.data[field]) query.data.put(field, value);
            ubus_data trace{ubus_type::Array};
            for (const auto edge : wait->request.data["trace"].values()) trace.push(edge);
            ubus_data edge; edge.put("manager", identity_).put("name", wait->name); trace.push(edge);
            query.data.put("trace", trace);
            status = wait->relay->begin("Get", std::move(query));
            if (status != STATUS_OK) return status;
            wait->introduced = true;
        }
        const auto status = wait->relay->poll(response);
        if (status == STATUS_OK) { response.forward(); return response.status; }
        return status;
    }
    mgr_child* provider = local;
    auto exported = local_name;
    if (const auto target = mgr_text(rule["child"]); !target.empty()) {
        provider = find(target); if (!provider) return STATUS_NOT_FOUND;
        if (const auto alias = mgr_text(rule["name"]); !alias.empty()) exported = alias;
    }
    publication* match{};
    for (auto& [id, pub] : publications_) {
        if (pub.name != exported || (provider && pub.owner != provider->id)) continue;
        if (match) return STATUS_BUSY;
        match = &pub;
    }
    if (match) {
        const auto interface = mgr_text(wait ? wait->request.data["interface"] : ubus_value{});
        const auto minimum = wait ? wait->request.data["minimum"].as<uint64_t>().value_or(0) : 0;
        const auto maximum = wait ? wait->request.data["maximum"].as<uint64_t>().value_or(UINT64_MAX) : UINT64_MAX;
        if ((!interface.empty() && match->interface != interface) || match->version < minimum || match->version > maximum) return STATUS_BAD_ARGS;
        response.data.put("instance", match->id).put("interface", match->interface).put("version", match->version)
            .cap("connector", *response.attach(match->connector.selector(), RIGHT_SEND | RIGHT_DUPLICATE));
        return STATUS_OK;
    }
    if (!provider || !wait) return STATUS_NOT_FOUND;
    for (const auto& operation : pending_) if ((operation->operation >= pending::Configure)
        && operation->name == provider->name) return STATUS_WOULD_BLOCK;
    if (!provider->task) {
        if (provider->state == "failed") return provider->result;
        const auto status = start(*provider); if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) return status;
    }
    if (provider->state == "draining") return STATUS_CLOSED;
    if (source && cycle(source->id, provider->id)) return STATUS_DENIED;
    wait->provider = provider->id; wait->provider_version = provider->version; wait->gen = provider->state == "loading" ? 0 : provider->gen;
    return STATUS_WOULD_BLOCK;
}
auto mgr::lookup(std::string_view name) -> std::expected<cap_owner, status_t> {
    ubus_msg result;
    const auto status = resolve(nullptr, name, result, nullptr);
    if (status != STATUS_OK) return std::unexpected(status);
    const auto cap = result.transfers[0].source;
    const auto copied = sys::cap_duplicate(cap, rt_startup().cspace, result.transfers[0].rights);
    if (copied.status != STATUS_OK) return std::unexpected(copied.status);
    return cap_owner{copied.value};
}
auto mgr::defer(std::unique_ptr<pending> wait, ubus_request& request) -> status_t {
    if (pending_.size() >= max_pending_) return STATUS_EXHAUSTED;
    const auto count = std::ranges::count_if(pending_, [&](const auto& item) { return item->environment == wait->environment; });
    const auto* source = environment(wait->environment);
    if (source && static_cast<uint64_t>(count) >= source->quota) return STATUS_EXHAUSTED;
    const auto status = wait->terminal.open(rt_startup().events, request.reply.selector(), SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
    if (status != STATUS_OK) return status;
    wait->request = std::move(request); pending_.push_back(std::move(wait));
    return STATUS_PENDING;
}
auto mgr::deliver(mgr_child& child, ubus_msg& response) -> status_t {
    auto* message = !child.received ? &child.setup : child.inbox.empty() ? nullptr : &child.inbox.front();
    if (!message) return STATUS_WOULD_BLOCK;
    auto selected = message->slice(message->data.view()); if (!selected) return selected.error();
    response = std::move(*selected);
    for (size_t i = 0; i < response.count; ++i) {
        const auto cap = response.transfers[i].source;
        if (!cap) return STATUS_CLOSED;
        const auto info = cap_info(cap); if (!info) return info.error();
        response.transfers[i] = info->view.rights & RIGHT_DUPLICATE ? CapXfer{cap, info->view.rights, CAP_COPY, 0}
            : CapXfer{cap, 0, CAP_MOVE, 0};
    }
    return STATUS_OK;
}
auto mgr::request(std::string_view method, ubus_request& request, ubus_msg& response) -> status_t {
    const auto facet = request.facet();
    auto* source = environment(facet);
    const bool admin = facet <= 3 && (facet & mgr_control), observe = facet <= 3 && (facet & mgr_observe);
    if (!source && !admin && !observe) return STATUS_CLOSED;
    const auto checkpoint = [&](const ubus_msg& recipe, pending::kind operation, bool active) -> status_t {
        const auto key = mgr_text(recipe.data["name"]);
        for (const auto& wait : pending_) if (wait->operation >= pending::Configure && wait->name == key) return STATUS_BUSY;
        auto copy = recipe.clone();
        if (!copy && copy.error() != STATUS_BAD_RIGHTS && copy.error() != STATUS_DENIED && copy.error() != STATUS_CLOSED && copy.error() != STATUS_INVALID_CAP) return copy.error();
        const bool replayable = bool(copy);
        ubus_msg commit;
        if (copy) commit = std::move(*copy);
        else commit.data.put("name", recipe.data["name"]).put("program", recipe.data["program"]);
        ubus_data envelope;
        envelope.put("key", key).put("version", revisions_[std::string{key}])
            .put("replayable", replayable).put("active", active).put("recipe", commit.data);
        commit.data = std::move(envelope); commit.deadline = 0;
        auto wait = std::make_unique<pending>();
        wait->operation = operation; wait->name = key;
        wait->relay = std::make_unique<ubus_conn>();
        auto status = wait->relay->begin_bind(rt_startup().environment, {}, 0, UINT64_MAX, 0, request.deadline);
        if (status != STATUS_OK) return status;
        // Keep staged data in the original request; only the immutable copy
        // is committed outside this execution. No child starts before ack.
        wait->staged = std::move(commit);
        status = defer(std::move(wait), request);
        return status;
    };
    if (method == "Get" || method == "Receive" || method == "Send" || method == "Changed") {
        if (method == "Changed" && request.data["id"]) {
            if (!observe) return STATUS_DENIED;
        } else {
            auto wait = std::make_unique<pending>();
            wait->source = source ? source->id : 0; wait->environment = source ? source->environment : 0;
            wait->version = source ? source->version : 0;
            wait->operation = method == "Get" ? pending::Get : method == "Send" ? pending::Send
                : method == "Changed" ? pending::Changed : pending::Receive;
            if (method == "Receive" && !source) return STATUS_DENIED;
            if (method == "Get") {
                wait->name = mgr_text(request.data["name"]);
                if (!mgr_name(wait->name)) return STATUS_BAD_ARGS;
                size_t depth{};
                for (const auto edge : request.data["trace"].values()) {
                    if (++depth >= 64 || (edge["manager"].as<uint64_t>() == identity_
                        && mgr_text(edge["name"]) == wait->name)) return STATUS_DENIED;
                }
            } else if (method == "Send") {
                auto* target = find(request.data["id"].as<uint64_t>().value_or(0));
                if (!target || (!admin && (!source || !source->recipe["send"][target->name].as<bool>().value_or(false)))) return STATUS_DENIED;
                if (!target->task || request.data["gen"].as<uint64_t>() != target->gen) return STATUS_STALE;
                wait->provider = target->id; wait->gen = target->gen;
                if (!request.data["message"] || request.data["message"].type() != ubus_type::Record) return STATUS_BAD_ARGS;
            } else if (method == "Changed") wait->epoch = request.data["epoch"].as<uint64_t>().value_or(0);
            return defer(std::move(wait), request);
        }
    }
    if (method == "Checkpoint" || method == "Restore") {
        if (!source) return STATUS_DENIED;
        if (method == "Restore") {
            const auto key = mgr_text(request.data["after"]);
            const auto item = source->snapshots.upper_bound(key);
            if (item == source->snapshots.end()) { response.data.put("done", true); return STATUS_OK; }
            auto copy = item->second.recipe.clone();
            if (copy) response = std::move(*copy);
            else response.data.put("name", item->first).put("program", item->first);
            ubus_data envelope;
            envelope.put("key", item->first).put("version", item->second.version)
                .put("replayable", item->second.replayable && bool(copy)).put("active", item->second.active).put("recipe", response.data).put("done", false);
            response.data = std::move(envelope); return STATUS_OK;
        }
        const auto key = mgr_text(request.data["key"]);
        if (!mgr_name(key)) return STATUS_BAD_ARGS;
        const auto old = source->snapshots.find(key);
        const auto version = old == source->snapshots.end() ? 0 : old->second.version;
        if (request.data["version"].as<uint64_t>() != version) return STATUS_STALE;
        if ((old == source->snapshots.end() && source->snapshots.size() >= max_pending_) || version == UINT64_MAX) return STATUS_EXHAUSTED;
        auto selected = request.slice(request.data["recipe"]); if (!selected) return selected.error();
        auto owned = selected->clone(); if (!owned) return owned.error();
        source->snapshots[std::string{key}] = {version + 1, request.data["replayable"].as<bool>().value_or(false), request.data["active"].as<bool>().value_or(false), std::move(*owned)};
        response.data.put("version", version + 1); return STATUS_OK;
    }
    if (method == "Publish") {
        if (!source) return STATUS_DENIED;
        const auto name = mgr_text(request.data["name"]);
        if (!mgr_name(name)) return STATUS_BAD_ARGS;
        bool allowed{};
        for (const auto value : source->exports.view().values()) if (mgr_text(value) == "*" || mgr_text(value) == name) allowed = true;
        if (!allowed) return STATUS_DENIED;
        size_t used{};
        for (const auto& [id, pub] : publications_) if (pub.owner == source->id) {
            if (pub.name == name) return STATUS_BUSY;
            ++used;
        }
        if (used >= source->quota || next_publication_ == UINT64_MAX) return STATUS_EXHAUSTED;
        auto connector = request.take(request.data["connector"]);
        if (!connector) return STATUS_BAD_ARGS;
        const auto info = cap_info(connector.selector());
        if (!info || info->view.kind != ObjChannel || (info->view.rights & (RIGHT_SEND | RIGHT_DUPLICATE)) != (RIGHT_SEND | RIGHT_DUPLICATE)) return STATUS_BAD_RIGHTS;
        publication pub{++next_publication_, source->id, source->gen, request.data["version"].as<uint64_t>().value_or(1),
            std::string{name}, std::string{mgr_text(request.data["interface"])}, std::move(connector), {}};
        const auto watched = pub.terminal.open(rt_startup().events, pub.connector.selector(), SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
        if (watched != STATUS_OK) return watched;
        response.data.put("instance", pub.id);
        publications_.emplace(pub.id, std::move(pub)); source->state = "running"; ++epoch_; return STATUS_OK;
    }
    if (method == "Withdraw") {
        const auto it = publications_.find(request.data["instance"].as<uint64_t>().value_or(0));
        if (it == publications_.end()) return STATUS_NOT_FOUND;
        if (!source || source->id != it->second.owner) return STATUS_DENIED;
        publications_.erase(it); ++epoch_; return STATUS_OK;
    }
    if (method == "List") {
        ubus_data items{ubus_type::Array};
        if (observe) for (const auto& child : children_) {
            auto row = mgr_row(child->task.get(), child->id, child->gen, child->name, child->state, admin);
            row.put("config", child->version).put("delivered", child->received);
            row.put("available", uint64_t(std::ranges::count_if(publications_, [&](const auto& row) { return row.second.owner == child->id; })));
            if (!child->task) row.put("result", int64_t(child->result));
            items.push(row);
        } else for (const auto route : source->routes.view().values()) {
            if (route.name() == "*") continue;
            ubus_data row; row.put("name", route.name()); items.push(row);
        }
        response.data.put("epoch", epoch_).put("items", items); return STATUS_OK;
    }
    if (method == "Configure" || method == "Spawn") {
        if (!admin) return STATUS_DENIED;
        if (durable_) {
            ubus_msg validation; validation.data = request.data;
            const auto valid = configure(std::move(validation), false, true); if (!valid) return valid.error();
            return checkpoint(request, method == "Spawn" ? pending::Spawn : pending::Configure,
                method == "Spawn" || request.data["start"].as<bool>().value_or(false));
        }
        auto child = configure(std::move(static_cast<ubus_msg&>(request)), method == "Spawn");
        if (!child) return child.error();
        if ((*child)->desired) { const auto status = start(**child); if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) return status; }
        response.data.put("id", (*child)->id).put("gen", (*child)->gen).put("config", (*child)->version); return STATUS_OK;
    }
    if (!observe && !admin && method != "Resource") return STATUS_DENIED;
    auto* child = source && method == "Resource" ? source : find(request.data["id"].as<uint64_t>().value_or(0));
    if (!child) return STATUS_NOT_FOUND;
    if (!source && request.data["gen"].as<uint64_t>() != child->gen) return STATUS_STALE;
    if (method == "Action") {
        if (!admin) return STATUS_DENIED;
        const auto operation = mgr_text(request.data["action"]);
        if (durable_ && (operation == "stop" || operation == "start" || operation == "restart") && child->recipe["name"]) {
            ubus_msg recipe;
            recipe.data = child->origin["name"] ? child->origin : child->recipe;
            if (!child->origin["name"]) {
                recipe.count = child->setup.count;
                for (size_t i = 0; i < recipe.count; ++i) recipe.transfers[i] = {child->setup.caps[i].selector(), child->setup.transfers[i].rights, CAP_COPY, 0};
            }
            return checkpoint(recipe, pending::Action, operation != "stop");
        }
        return action(*child, operation);
    }
    if (!child->task) return STATUS_CLOSED;
    if (method == "Resource") {
        const auto operation = mgr_text(request.data["action"]);
        const auto number = request.data["loan"].as<uint64_t>().value_or(0);
        const auto memory = request.data["bytes"].as<uint64_t>(), caps = request.data["caps"].as<uint64_t>();
        auto target = child->task->pool();
        if (number) {
            const auto found = loans_.find(number);
            if (found == loans_.end() || found->second.source != child->id || found->second.gen != child->gen) return STATUS_STALE;
            target = found->second.account.selector();
        }
        if (operation == "close") {
            if (!number) return STATUS_BAD_ARGS;
            return sys::account_close(target).status;
        }
        if (!memory || !caps) return STATUS_BAD_ARGS;
        if (operation == "lend") {
            if (number || loans_.size() >= max_pending_ || next_loan_ == UINT64_MAX) return STATUS_EXHAUSTED;
            const auto count = std::ranges::count_if(loans_, [&](const auto& pair) { return pair.second.source == child->id; });
            if (uint64_t(count) >= child->quota) return STATUS_EXHAUSTED;
            const auto made = sys::account_create({target, rt_startup().scope}, *memory, *caps,
                request.data["kinds"].as<uint64_t>().value_or(child->resources.kinds),
                request.data["max_bytes"].as<uint64_t>().value_or(*memory), request.data["max_caps"].as<uint64_t>().value_or(*caps));
            if (made.status != STATUS_OK) return made.status;
            loan item{child->id, child->gen, cap_owner{made.value}, {}};
            const auto status = item.drained.open(rt_startup().events, made.value, STATE_DRAINED);
            if (status != STATUS_OK) { (void)sys::account_close(made.value); return status; }
            response.data.put("loan", ++next_loan_).put("bytes", *memory).put("caps", *caps)
                .cap("account", *response.attach(made.value, RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE));
            loans_.emplace(next_loan_, std::move(item)); return STATUS_OK;
        }
        if (!operation.empty() && operation != "resize") return STATUS_BAD_ARGS;
        const auto status = sys::account_resize(target, *memory, *caps, number ? child->task->pool() : child->auth.alloc.account).status;
        if (status == STATUS_OK) { response.data.put("bytes", *memory).put("caps", *caps); ++epoch_; }
        return status;
    }
    if (method == "Changed") return observers_.accept(child->task->scope(), child->task->cspace(), request, response);
    if (method == "Inspect") {
        if (mgr_text(request.data["facet"]) == "dependencies") {
            ubus_data items{ubus_type::Array}, deployment;
            deployment.put("name", child->name).put("id", child->id).put("gen", child->gen)
                .put("config", child->version).put("program", child->image).put("desired", child->desired).put("restart", child->restart);
            items.push(deployment);
            for (const auto route : child->routes.view().values()) {
                ubus_data edge; edge.put("name", route.name()).put("rule", route); items.push(edge);
            }
            response.data.put("items", items); return STATUS_OK;
        }
        return mgr_inspect(child->scope.selector(), child->task->cspace(), request.data.view(), response);
    }
    if (method == "CloseCap") return admin ? sys::cap_close(request.data["selector"].as<cap_t>().value_or(0), child->task->cspace()).status : STATUS_DENIED;
    if (method == "Revoke") return admin ? mgr_revoke(child->task->cspace(), request.data.view(), response) : STATUS_DENIED;
    return STATUS_INVALID_OP;
}
auto mgr::poll() -> status_t {
    for (size_t count = 0; count < 32; ++count) {
        const auto status = object_.poll();
        if (status == STATUS_WOULD_BLOCK) break;
        if (status != STATUS_OK) return status;
    }
    for (const auto& item : children_) {
        auto& child = *item;
        if (!child.task) {
            if (std::ranges::any_of(pending_, [&](const auto& operation) {
                return (operation->operation >= pending::Configure) && operation->name == child.name;
            })) continue;
            if (child.desired && child.state != "failed") (void)start(child);
            continue;
        }
        if (child.state == "starting") {
            const auto info = child.task->info();
            if (info && info->applied == info->requested && (info->flags & THREAD_APPLIED)) { child.state = "running"; ++epoch_; }
        }
        if (child.state == "suspending") {
            const auto info = child.task->info();
            if (info && info->applied == info->requested && !(info->flags & THREAD_APPLIED)) { child.state = "suspended"; ++epoch_; }
        }
        const auto status = child.task->poll();
        if (child.task->closing() && child.state != "draining") { child.state = "draining"; withdraw(child); ++epoch_; }
        if (status == STATUS_OK) {
            const auto result = child.task->collect();
            const bool failed = child.result != STATUS_OK; // Construction errors survive native cleanup.
            if (!failed) child.result = static_cast<status_t>(result.value);
            child.task.reset(); child.inbox.clear(); child.environment = 0;
            const bool restart = child.desired && child.restart && child.reusable;
            child.state = failed || (child.result != STATUS_OK && child.desired && !restart) ? "failed" : "idle";
            child.desired = restart; ++epoch_;
        } else if (status != STATUS_WOULD_BLOCK && !proc_retryable(status)) return status;
    }
    // These are real funded Accounts, outside the payer execution's Scope.
    // Closing a payer stops new allocations; the provider's actual objects must
    // drain before this obligation and the payer execution can be collected.
    for (auto it = loans_.begin(); it != loans_.end();) {
        auto& item = it->second;
        const auto* child = find(item.source);
        const auto facts = sys::source_info(item.account.selector());
        if (facts.status != STATUS_OK) return facts.status;
        if ((!child || !child->task || child->gen != item.gen || child->task->closing()) && (facts.value & STATE_OPEN)) {
            const auto status = sys::account_close(item.account.selector()).status;
            if (status != STATUS_OK) return status;
        }
        if (facts.value & STATE_DRAINED) {
            const auto status = item.drained.close(); if (status != STATUS_OK) return status;
            const auto retired = sys::object_destroy(item.account.selector()).status; if (retired != STATUS_OK) return retired;
            it = loans_.erase(it); continue;
        }
        ++it;
    }
    std::erase_if(publications_, [&](const auto& row) {
        const auto live = sys::source_info(row.second.connector.selector(), SOURCE_WRITE);
        if (live.status == STATUS_OK && !(live.value & SOURCE_TERMINAL)) return false;
        ++epoch_; return true;
    });
    observers_.poll(); deadline_ = observers_.deadline();
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto& wait = **it;
        if (!wait.request.reply) { it = pending_.erase(it); continue; }
        const auto peer = sys::source_info(wait.request.reply.selector(), SOURCE_WRITE);
        const bool committing = wait.operation >= pending::Configure;
        if (!committing && (peer.status != STATUS_OK || (peer.value & SOURCE_TERMINAL))) { it = pending_.erase(it); continue; }
        if (wait.operation >= pending::Configure) {
            ubus_msg response;
            auto status = wait.introduced ? STATUS_OK : wait.relay->poll_bind();
            if (!wait.introduced && status == STATUS_OK) {
                status = wait.relay->begin("Checkpoint", std::move(wait.staged));
                if (status == STATUS_OK) wait.introduced = true;
            }
            if (wait.introduced && status == STATUS_OK) {
                status = wait.relay->poll(response);
                if (status == STATUS_OK) status = response.status;
                else if (status == STATUS_WOULD_BLOCK) { ++it; continue; }
                if (status == STATUS_OK) {
                    revisions_[wait.name] = *response.data["version"].as<uint64_t>();
                    if (wait.operation == pending::Action) {
                        auto operation = std::move(*it); it = pending_.erase(it);
                        auto* child = find(operation->request.data["id"].as<uint64_t>().value_or(0));
                        response = {};
                        response.status = !child || child->gen != operation->request.data["gen"].as<uint64_t>() ? STATUS_STALE
                            : action(*child, mgr_text(operation->request.data["action"]));
                        if (response.status == STATUS_WOULD_BLOCK) response.status = STATUS_OK;
                        const auto sent = operation->request.respond(std::move(response)); if (sent != STATUS_OK) return sent;
                        continue;
                    }
                    const auto child = configure(std::move(static_cast<ubus_msg&>(wait.request)), wait.operation == pending::Spawn);
                    response = {};
                    if (!child) status = child.error();
                    else {
                        if ((*child)->desired) { status = start(**child); if (status == STATUS_WOULD_BLOCK) status = STATUS_OK; }
                        response.data.put("id", (*child)->id).put("gen", (*child)->gen).put("config", (*child)->version);
                    }
                }
            }
            if (status == STATUS_WOULD_BLOCK) { ++it; continue; }
            response.status = status;
            const auto sent = wait.request.respond(std::move(response)); if (sent != STATUS_OK) return sent;
            it = pending_.erase(it); continue;
        }
        auto* source = wait.source ? environment(wait.environment) : nullptr;
        ubus_msg response;
        auto status = rt_admit(wait.request.deadline);
        if (status == STATUS_OK && wait.source && (!source || source->version != wait.version)) status = STATUS_STALE;
        if (status == STATUS_OK && wait.provider) {
            const auto* target = find(wait.provider);
            if (target && !wait.gen) { if (target->state != "loading") wait.gen = target->gen; }
            if (target && wait.provider_version && target->version != wait.provider_version) status = STATUS_STALE;
            else if (!target || (wait.gen && target->gen != wait.gen)) status = STATUS_CLOSED;
            else if (!target->task && target->state == "loading" && target->desired) status = STATUS_OK;
            else if (!target->task) status = target->result == STATUS_OK ? STATUS_CLOSED : target->result;
            else if (target->state == "draining") {
                const auto ended = target->task->observe();
                status = ended.status != STATUS_OK ? ended.status : ended.value2 ? static_cast<status_t>(ended.value2) : STATUS_CLOSED;
            }
        }
        if (status == STATUS_OK) {
            if (wait.operation == pending::Get) status = resolve(source, wait.name, response, &wait);
            else if (wait.operation == pending::Changed) {
                status = epoch_ != wait.epoch ? STATUS_OK : STATUS_WOULD_BLOCK;
                response.data.put("epoch", epoch_);
            } else if (wait.operation == pending::Send) status = STATUS_WOULD_BLOCK;
            else {
                status = deliver(*source, response);
                if (status == STATUS_OK) {
                    auto& message = !source->received ? source->setup : source->inbox.front();
                    std::array<cap_t, CHANNEL_MAX_CAPS> moved{};
                    for (size_t i = 0; i < response.count; ++i) if (response.transfers[i].operation == CAP_MOVE) moved[i] = response.transfers[i].source;
                    ubus_data wrapped; wrapped.put("message", response.data); response.data = std::move(wrapped);
                    bool committed{};
                    const auto sent = wait.request.respond(std::move(response), &committed);
                    if (sent != STATUS_OK) return sent;
                    if (committed) {
                        for (size_t i = 0; i < message.count; ++i) if (message.caps[i]
                            && std::ranges::find(moved, message.caps[i].selector()) != moved.end()) {
                            (void)message.caps[i].release(); if (!source->received) source->reusable = false;
                        }
                        if (!source->received) { source->received = true; source->state = "running"; ++epoch_; }
                        else source->inbox.pop_front();
                    }
                    it = pending_.erase(it); continue;
                }
                if (status == STATUS_WOULD_BLOCK) for (const auto& candidate : pending_) {
                    auto& sender = *candidate;
                    if (sender.operation != pending::Send || sender.provider != source->id || sender.gen != source->gen || !sender.request.reply) continue;
                    const auto live = sys::source_info(sender.request.reply.selector(), SOURCE_WRITE);
                    if (live.status != STATUS_OK || (live.value & SOURCE_TERMINAL) || rt_admit(sender.request.deadline) != STATUS_OK) continue;
                    response.data.put("message", sender.request.data["message"]);
                    for (size_t i = 0; i < sender.request.count; ++i) (void)response.give(std::move(sender.request.caps[i]));
                    bool committed{};
                    const auto sent = wait.request.respond(std::move(response), &committed);
                    if (sent != STATUS_OK) return sent;
                    ubus_msg ack; ack.status = committed ? STATUS_OK : STATUS_CANCELED;
                    ack.data.put("delivered", committed).put("gen", source->gen);
                    (void)sender.request.respond(std::move(ack));
                    status = STATUS_PENDING; break;
                }
                if (status == STATUS_PENDING) { it = pending_.erase(it); continue; }
            }
        }
        if (status != STATUS_WOULD_BLOCK) {
            response.status = status;
            const auto sent = wait.request.respond(std::move(response));
            if (sent != STATUS_OK) return sent;
            it = pending_.erase(it); continue;
        }
        const auto due = wait.request.deadline;
        if (due && (!deadline_ || due < deadline_)) deadline_ = due;
        ++it;
    }
    // A completed retirement can leave desired work but no remaining Source.
    // Schedule the next turn explicitly; the embedding owner gets this turn's
    // post-poll boundary to finish device-specific reset/fence before restart.
    for (const auto& child : children_) if (!child->task && child->desired && child->state == "idle"
        && std::ranges::none_of(pending_, [&](const auto& operation) {
            return operation->operation >= pending::Configure && operation->name == child->name;
        })) deadline_ = sys::clock_now().value;
    for (const auto& child : children_) if (child->task && child->task->polling()) {
        const auto due = rt_after_ms(1).value_or(0);
        if (!deadline_ || due < deadline_) deadline_ = due;
    }
    return object_.arm();
}
auto mgr::recover(std::function<std::expected<ubus_msg, status_t>(ubus_msg)> restore) -> status_t {
    if (!rt_startup().environment) return STATUS_OK;
    ubus_conn journal;
    auto status = journal.bind(rt_startup().environment); if (status != STATUS_OK) return status;
    if (!journal.token("Restore")) return STATUS_OK;
    std::string after;
    for (;;) {
        ubus_msg request, response; request.data.put("after", after);
        status = journal.invoke("Restore", std::move(request), response); if (status != STATUS_OK) return status;
        if (response.data["done"].as<bool>().value_or(false)) break;
        after = mgr_text(response.data["key"]);
        revisions_[after] = response.data["version"].as<uint64_t>().value_or(0);
        const bool replayable = response.data["replayable"].as<bool>().value_or(false);
        const bool active = response.data["active"].as<bool>().value_or(false);
        ubus_data recipe; (void)recipe.assign(response.data["recipe"].bytes()); response.data = std::move(recipe);
        if (restore && replayable) {
            auto resolved = restore(std::move(response)); if (!resolved) return resolved.error();
            response = std::move(*resolved);
        }
        auto child = configure(std::move(response)); if (!child) return child.error();
        (*child)->version = revisions_[after];
        // Restoring configuration never replays a one-shot invocation or Move.
        (*child)->desired = replayable && active && (*child)->restart;
        if (!replayable) { (*child)->reusable = false; (*child)->state = "failed"; (*child)->result = STATUS_CLOSED; }
    }
    durable_ = true; return STATUS_OK;
}
auto mgr::open() -> status_t {
    const auto identity = sys::object_info(rt_startup().scope);
    if (identity.status != STATUS_OK) return identity.status;
    identity_ = reinterpret_cast<const ObjectInfo*>(rt_startup().ipc)->object_id;
    const auto add = [&](std::string name, ubus_schema input, ubus_schema output, word_t required = 0) {
        const auto method = name;
        return object_.add(std::move(name), std::move(input), std::move(output),
            [this, method](auto& q, auto& r) { return request(method, q, r); }, required);
    };
    const ubus_schema recipe{{"name", ubus_type::String}, {"program", ubus_type::String},
        {"argv", ubus_type::Array, true, UINT64_MAX, {{"", ubus_type::String}}},
        {"env", ubus_type::Array, true, UINT64_MAX, {{"", ubus_type::String}}}, {"cpu", ubus_type::U64, true},
        {"resources", ubus_type::Record, true}, {"routes", ubus_type::Record, true},
        {"exports", ubus_type::Array, true, UINT64_MAX, {{"", ubus_type::String}}},
        {"image", ubus_type::Buffer, true}, {"setup", ubus_type::Record, true}, {"send", ubus_type::Record, true},
        {"start", ubus_type::Bool, true}, {"restart", ubus_type::Bool, true}, {"quota", ubus_type::U64, true}};
    for (const auto status : {
        add("Checkpoint", {{"key", ubus_type::String}, {"version", ubus_type::U64}, {"replayable", ubus_type::Bool}, {"active", ubus_type::Bool}, {"recipe", ubus_type::Record}}, {{"version", ubus_type::U64}}),
        add("Restore", {{"after", ubus_type::String}}, {{"done", ubus_type::Bool}, {"key", ubus_type::String, true}, {"version", ubus_type::U64, true}, {"replayable", ubus_type::Bool, true}, {"active", ubus_type::Bool, true}, {"recipe", ubus_type::Record, true}}),
        add("Get", {{"name", ubus_type::String}, {"interface", ubus_type::String, true},
            {"minimum", ubus_type::U64, true}, {"maximum", ubus_type::U64, true},
            {"trace", ubus_type::Array, true, UINT64_MAX, {{"", ubus_type::Record, false, UINT64_MAX,
                {{"manager", ubus_type::U64}, {"name", ubus_type::String}}}}}},
            {{"connector", ubus_type::Cap}, {"instance", ubus_type::U64, true}, {"interface", ubus_type::String, true}, {"version", ubus_type::U64, true}}),
        add("Publish", {{"name", ubus_type::String}, {"connector", ubus_type::Cap},
            {"interface", ubus_type::String, true}, {"version", ubus_type::U64, true}}, {{"instance", ubus_type::U64}}),
        add("Withdraw", {{"instance", ubus_type::U64}}, {}),
        add("Receive", {}, {{"message", ubus_type::Record}}),
        add("Send", {{"id", ubus_type::U64}, {"gen", ubus_type::U64}, {"message", ubus_type::Record}},
            {{"delivered", ubus_type::Bool}, {"gen", ubus_type::U64}}),
        add("List", {}, {{"epoch", ubus_type::U64}, {"items", ubus_type::Array}}),
        add("Changed", {{"epoch", ubus_type::U64}, {"id", ubus_type::U64, true}, {"gen", ubus_type::U64, true},
            {"grant", ubus_type::U64, true}, {"object", ubus_type::U64, true}, {"selector", ubus_type::U64, true}, {"selected", ubus_type::U64, true}},
            {{"epoch", ubus_type::U64}, {"grant", ubus_type::U64, true}}),
        add("Configure", recipe, {{"id", ubus_type::U64}, {"gen", ubus_type::U64}, {"config", ubus_type::U64}}, mgr_control),
        add("Spawn", recipe, {{"id", ubus_type::U64}, {"gen", ubus_type::U64}, {"config", ubus_type::U64}}, mgr_control),
        add("Resource", {{"bytes", ubus_type::U64, true}, {"caps", ubus_type::U64, true}, {"id", ubus_type::U64, true}, {"gen", ubus_type::U64, true},
            {"action", ubus_type::String, true}, {"loan", ubus_type::U64, true}, {"kinds", ubus_type::U64, true},
            {"max_bytes", ubus_type::U64, true}, {"max_caps", ubus_type::U64, true}},
            {{"bytes", ubus_type::U64, true}, {"caps", ubus_type::U64, true}, {"loan", ubus_type::U64, true}, {"account", ubus_type::Cap, true}})}) if (status != STATUS_OK) return status;
    for (const auto name : {"Action", "Inspect", "CloseCap", "Revoke"}) {
        const auto status = mgr_method(object_, name, [this, name](auto& q, auto& r) { return request(name, q, r); });
        if (status != STATUS_OK) return status;
    }
    return object_.open(32);
}

auto mgr_method(ubus_object& object, std::string name, ubus_handler handler) -> status_t {
    ubus_schema input, output;
    if (std::string_view{name} != "List") input = {{"id", ubus_type::U64}, {"gen", ubus_type::U64}};
    if (std::string_view{name} == "Action") input.push_back({"action", ubus_type::String});
    if (name == "Inspect" || name == "Changed") {
        if (name == "Inspect") input.push_back({"facet", ubus_type::String});
        else { input.push_back({"epoch", ubus_type::U64}); input.push_back({"grant", ubus_type::U64, true}); }
        for (const auto field : {"object", "selector", "selected"}) input.push_back({field, ubus_type::U64, true});
        if (name == "Inspect") for (const auto field : {"cursor", "epoch"}) input.push_back({field, ubus_type::U64, true});
    } else if (std::string_view{name} == "CloseCap" || std::string_view{name} == "Revoke") {
        input.push_back({"selector", ubus_type::U64}); input.push_back({"selected", ubus_type::U64, true});
    }
    if (std::string_view{name} == "List") output = {{"epoch", ubus_type::U64}, {"items", ubus_type::Array}};
    if (std::string_view{name} == "Inspect") output = {{"items", ubus_type::Array}, {"epoch", ubus_type::U64, true}};
    if (name == "Changed") output = {{"epoch", ubus_type::U64}, {"grant", ubus_type::U64}};
    if (name == "Revoke") output = {{"state", ubus_type::U64}};
    const bool observe = std::string_view{name} == "List" || std::string_view{name} == "Inspect" || name == "Changed";
    return object.add(std::move(name), std::move(input), std::move(output), std::move(handler), observe ? mgr_observe : mgr_control);
}

static auto mgr_cap_view(const CapView& view) -> ubus_data {
    ubus_data row;
    row.put("kind", uint64_t(view.kind)).put("rights", view.rights)
        .array("words", std::span{view.words});
    return row;
}
auto mgr_revoke(cap_t cspace, ubus_value query, ubus_msg& result) -> status_t {
    const auto status = sys::cap_revoke(*query["selector"].as<cap_t>(), cspace,
        CAP_REVOKE_ASYNC, query["selected"].as<uint64_t>().value_or(0));
    if (status.status != STATUS_OK && status.status != STATUS_PENDING) return status.status;
    result.data.put("state", status.value);
    return STATUS_OK; // Accepted; native grant observation distinguishes drain.
}
auto mgr_inspect(cap_t scope, cap_t cspace, ubus_value query, ubus_msg& result) -> status_t {
    const auto facet = *query["facet"].as<std::string_view>();
    auto cursor = query["cursor"].as<uint64_t>().value_or(0), epoch = query["epoch"].as<uint64_t>().value_or(0);
    auto object = query["object"].as<uint64_t>().value_or(0);
    const auto selector = query["selector"].as<cap_t>().value_or(0), selected = query["selected"].as<uint64_t>().value_or(0);
    const auto ipc = reinterpret_cast<void*>(rt_startup().ipc);
    if (!object) {
        const auto status = sys::object_info(facet == "caps" || facet == "grants" ? cspace : scope).status;
        if (status != STATUS_OK) return status;
        ObjectInfo info; ::memcpy(&info, ipc, sizeof(info)); object = info.object_id;
    }
    const auto kind = facet == "caps" ? SCOPE_CAPS : facet == "members" ? SCOPE_MEMBERS
        : facet == "info" ? SCOPE_INFO : facet == "links" ? SCOPE_LINKS : facet == "usage" ? SCOPE_USAGE
        : facet == "ops" ? SCOPE_OPS : facet == "grants" ? SCOPE_GRANTS : 0U;
    if (!kind) return STATUS_BAD_ARGS;
    ubus_data items{ubus_type::Array};
    uint64_t next{}, generation{};
    bool more{};
    // The observer may be a descendant of this Scope. Creating a new RPC body
    // between native pages would itself invalidate the recursive epoch.
    // Sample all pages inside one invocation; concurrent mutation still yields
    // STALE, never a partially assembled snapshot.
    do {
        if (kind == SCOPE_GRANTS) { const GrantQuery request{selector, selected, cursor, epoch}; ::memcpy(ipc, &request, sizeof(request)); }
        const auto status = sys::scope_query(scope, object, kind, cursor, epoch);
        next = generation = 0; more = false;
#define FIELD(field) row.put(#field, info.field)
        if (kind == SCOPE_OPS && status.value2) {
            ubus_data row; row.put("status", int64_t(status.status)).put("value", status.value).put("phase", status.value2); items.push(row);
        } else if (status.status != STATUS_OK) return status.status;
        else if (kind == SCOPE_CAPS) {
            CSpaceInfo page; ::memcpy(&page, ipc, sizeof(page));
            for (const auto& info : std::span{page.records, page.count}) {
                auto row = mgr_cap_view(info.view); FIELD(selector); FIELD(object_id); FIELD(grant_id); FIELD(state); items.push(row);
            }
            next = page.cursor; generation = page.epoch; more = page.cursor != 0;
        } else if (kind == SCOPE_MEMBERS) {
            ScopeMembers page; ::memcpy(&page, ipc, sizeof(page));
            for (const auto& info : std::span{page.records, page.count}) {
                ubus_data row; FIELD(object_id); FIELD(epoch); FIELD(kind); FIELD(state); items.push(row);
            }
            next = page.next.last; generation = page.next.version; more = page.more;
        } else if (kind == SCOPE_LINKS) {
            LinkRecords page; ::memcpy(&page, ipc, sizeof(page));
            for (const auto& info : std::span{page.records, page.count}) {
                ubus_data row; FIELD(link_id); FIELD(object_id); FIELD(grant_id); FIELD(first); FIELD(bytes); FIELD(offset);
                FIELD(kind); FIELD(state); FIELD(perms); FIELD(flags); items.push(row);
            }
            next = page.cursor; generation = page.epoch; more = page.more;
        } else if (kind == SCOPE_GRANTS) {
            GrantRecords page; ::memcpy(&page, ipc, sizeof(page));
            const auto append = [&](const GrantRecord& info) {
                auto row = mgr_cap_view(info.ceiling); FIELD(grant_id); FIELD(parent_id); FIELD(object_id); FIELD(state); FIELD(flags); items.push(row);
            };
            if (!cursor) append(page.root);
            for (const auto& info : std::span{page.records, page.count}) append(info);
            next = page.last; generation = page.epoch; more = page.more;
        } else if (kind == SCOPE_INFO) {
            ScopeObject info; ::memcpy(&info, ipc, sizeof(info));
            ubus_data row; FIELD(object_id); FIELD(owner_id); FIELD(owner_epoch); FIELD(account_id); FIELD(billing_epoch);
            FIELD(storage_memory); FIELD(storage_caps); FIELD(kind); FIELD(gate); FIELD(flags); items.push(row);
        } else if (kind == SCOPE_USAGE) {
            const auto size = static_cast<const uint32_t*>(ipc)[1];
            ubus_data row;
            if (size == sizeof(AccountInfo)) {
                AccountInfo info; ::memcpy(&info, ipc, sizeof(info));
                FIELD(memory_limit); FIELD(memory_available); FIELD(cap_limit); FIELD(caps_available); FIELD(allocations); FIELD(reservations); FIELD(state);
            } else if (size == sizeof(DomainInfo)) {
                DomainInfo info; ::memcpy(&info, ipc, sizeof(info));
                FIELD(cpus); FIELD(budget); FIELD(period); FIELD(available); FIELD(debt); FIELD(escrow); FIELD(weight); FIELD(phase);
                FIELD(requested); FIELD(applied); FIELD(runtime); FIELD(work_runtime); FIELD(status);
            } else return STATUS_INVALID_OP;
            items.push(row);
        }
#undef FIELD
        cursor = next; epoch = generation;
    } while (more);
    result.data.put("items", items).put("epoch", generation);
    return STATUS_OK;
}


// The observer can live inside this Scope. Keep ordinary Scope waits inline
// (id/gen/epoch); a deliberately large request can itself change that Scope.
auto mgr_wait::entry::facts(ubus_msg& response) const -> status_t {
    const auto source = sys::source_info(scope.selector());
    if (source.status != STATUS_OK) return source.status;
    if (source.value & (STATE_CLOSING | STATE_DRAINED)) return STATUS_CLOSED;
    const auto branch = selector ? sys::grant_source(scope.selector(), object, selector, selected) : sys::SysResult{};
    if (branch.status != STATUS_OK) return branch.status;
    response.data.put("epoch", source.value2).put("grant", branch.value2);
    return source.value2 != epoch || branch.value2 != grant ? STATUS_OK : STATUS_WOULD_BLOCK;
}
auto mgr_wait::accept(cap_t scope, cap_t cspace, ubus_request& request, ubus_msg& response) -> status_t {
    auto waiter = std::make_unique<entry>();
    const auto held = sys::cap_duplicate(scope, rt_startup().cspace, RIGHT_INSPECT);
    if (held.status != STATUS_OK) return held.status;
    waiter->scope = cap_owner{held.value}; waiter->object = request.data["object"].as<uint64_t>().value_or(0);
    waiter->selector = request.data["selector"].as<uint64_t>().value_or(0);
    waiter->selected = request.data["selected"].as<uint64_t>().value_or(0);
    waiter->epoch = *request.data["epoch"].as<uint64_t>();
    waiter->grant = request.data["grant"].as<uint64_t>().value_or(0);
    if (waiter->selector && !waiter->object) {
        const auto status = sys::object_info(cspace).status;
        if (status != STATUS_OK) return status;
        ObjectInfo info; ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info));
        waiter->object = info.object_id;
    }
    auto status = waiter->facts(response);
    if (status != STATUS_WOULD_BLOCK) return status;
    const auto port = rt_startup().events;
    status = waiter->watches[0].open(port, scope, 0, waiter->epoch, WATCH_CHANGED);
    if (status == STATUS_OK && waiter->selector)
        status = waiter->watches[1].grant(port, scope, waiter->object, waiter->selector, waiter->selected, waiter->grant);
    if (status == STATUS_OK) status = waiter->watches[2].open(port, request.reply.selector(), SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
    if (status != STATUS_OK) return status;
    waiter->request = std::move(request);
    const auto due = waiter->request.deadline;
    if (due && (!deadline_ || due < deadline_)) deadline_ = due;
    entries_.push_back(std::move(waiter));
    return STATUS_PENDING;
}
void mgr_wait::poll() {
    deadline_ = 0;
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        auto& waiter = **entry;
        const auto peer = sys::source_info(waiter.request.reply.selector(), SOURCE_WRITE);
        if (peer.status != STATUS_OK || (peer.value & SOURCE_TERMINAL)) { entry = entries_.erase(entry); continue; }
        ubus_msg response;
        auto status = waiter.facts(response);
        if (status == STATUS_WOULD_BLOCK && rt_admit(waiter.request.deadline) != STATUS_OK) status = STATUS_TIMED_OUT;
        if (status != STATUS_WOULD_BLOCK) {
            response.status = status; (void)waiter.request.respond(std::move(response));
            entry = entries_.erase(entry); continue;
        }
        (void)waiter.watches[0].arm(waiter.epoch);
        if (waiter.watches[1]) (void)waiter.watches[1].arm(waiter.grant);
        const auto due = waiter.request.deadline;
        if (due && (!deadline_ || due < deadline_)) deadline_ = due;
        ++entry;
    }
}
