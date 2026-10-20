#include "mgr.hpp"

auto mgr_view(ubus_conn& directory, std::string_view prefix, uint64_t rights,
              cap_t scope, uint64_t quota) -> std::expected<cap_owner, status_t> {
    ubus_msg request, response;
    request.data.put("prefix", prefix).put("rights", rights).put("quota", quota);
    const auto index = request.attach(scope, RIGHT_INSPECT);
    if (!index) return std::unexpected(STATUS_NO_MEMORY);
    request.data.cap("lifetime", *index);
    const auto status = directory.invoke("View", std::move(request), response);
    if (status != STATUS_OK) return std::unexpected(status);
    auto connector = response.take(response.data["connector"]);
    if (!connector) return std::unexpected(STATUS_PEER_FAULT);
    return connector;
}
auto mgr_role(ubus_conn& directory, std::string_view alias, std::string_view publication) -> status_t {
    ubus_msg query, resolved; query.data.put("name", publication);
    auto status = directory.invoke("Resolve", std::move(query), resolved);
    if (status != STATUS_OK) return status;
    const auto instance = resolved.data["instance"].as<uint64_t>();
    if (!instance) return STATUS_PEER_FAULT;
    ubus_msg request, response; request.data.put("name", alias).put("instance", *instance);
    return directory.invoke("Role", std::move(request), response);
}
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
auto mgr::add(std::string name, std::string image, proc_quota quota) -> mgr_child& {
    auto child = std::make_unique<mgr_child>();
    child->id = ++next_; child->name = std::move(name); child->image = std::move(image);
    child->args.push_back(child->name); child->quota = quota;
    children_.push_back(std::move(child)); ++epoch_; return *children_.back();
}
auto mgr::find(uint64_t id) -> mgr_child* {
    for (const auto& child : children_) if (child->id == id) return child.get();
    return nullptr;
}
auto mgr::start(mgr_child& child) -> status_t {
    if (child.task || !child.desired) return STATUS_BUSY;
    for (const auto& item : child.owned) if (!item.cap) return STATUS_CLOSED;
    const auto image = image_(child.image);
    if (!image) return image.error();
    const auto& env = rt_startup();
    const auto auth = child.auth.value_or(proc_auth{env.alloc(), static_cast<uint32_t>(env.cpus), env.domain});
    child.task = std::make_unique<proc_task>(); ++child.gen;
    auto status = child.task->open(auth, child.quota);
    std::vector<proc_cap> grants = child.grants;
    for (auto& item : child.owned) grants.push_back({.key={item.name.c_str(), item.kind}, .bytes=item.bytes,
        .cap=item.linear ? std::variant<cap_t, cap_owner*>{&item.cap} : std::variant<cap_t, cap_owner*>{item.cap.selector()},
        .limit=item.linear ? word_t{0} : item.rights});
    cap_owner publisher;
    const auto has = [&](rt_key key) {
        return std::ranges::any_of(grants, [&](const auto& grant) { return std::string_view{grant.key.name} == key.name; });
    };
    if (status == STATUS_OK && directory_.endpoint() && !has(ubus_publisher)) {
        auto view = mgr_view(directory_, child.name, ubus_all, child.task->scope(), child.publications);
        if (!view) status = view.error();
        else {
            publisher = std::move(*view);
            grants.push_back({.key=ubus_publisher, .cap=publisher.selector(), .limit=word_t{RIGHT_SEND | RIGHT_DUPLICATE}});
        }
    }
    if (lookup_ && !has(ubus_directory)) grants.push_back({.key=ubus_directory, .cap=lookup_, .limit=word_t{RIGHT_SEND | RIGHT_DUPLICATE}});
    if (status == STATUS_OK && child.await_ready) {
        status = child.ready.open(env.alloc(), env.events);
        if (status == STATUS_OK) grants.push_back({.key=ubus_ready, .cap=child.ready.selector(), .limit=word_t{RIGHT_SIGNAL}});
    }
    std::vector<std::string_view> args{child.args.begin(), child.args.end()};
    if (status == STATUS_OK) status = proc_spawn(*child.task, auth, *image,
        {.argv=args, .caps=grants, .resources=child.quota, .events=env.events});
    if (status == STATUS_OK) status = child.task->start();
    child.state = status == STATUS_OK ? (child.await_ready ? "starting" : "running") : "draining";
    if (status != STATUS_OK) (void)child.task->stop(status);
    ++epoch_; return status;
}
auto mgr::action(mgr_child& child, std::string_view action) -> status_t {
    if (action == "start" || action == "restart") {
        child.desired = true;
        if (action == "start") return STATUS_OK;
    } else if (action == "stop") child.desired = false;
    else if (action == "pause" || action == "resume") {
        if (!child.task || child.state == "draining") return STATUS_CLOSED;
        const bool enabled = action == "resume";
        const auto status = child.task->execution(enabled).status;
        if (status == STATUS_OK) { child.state = enabled ? "running" : "suspending"; ++epoch_; }
        return status;
    } else return STATUS_BAD_ARGS;
    if (!child.task) return STATUS_OK;
    const auto status = child.task->stop();
    if (status == STATUS_OK) { child.state = "draining"; ++epoch_; }
    return status;
}
auto mgr::poll() -> status_t {
    for (const auto& entry : children_) {
        auto& child = *entry;
        if (!child.task) continue;
        if (child.state == "starting" && child.ready.take().status == STATUS_OK) { child.state = "running"; ++epoch_; }
        if (child.state == "suspending") {
            const auto info = child.task->info();
            if (info && info->applied == info->requested && !(info->flags & THREAD_APPLIED)) { child.state = "suspended"; ++epoch_; }
        }
        if (child.state == "starting") { const auto status = child.ready.arm(); if (status != STATUS_OK) return status; }
        const auto status = child.task->poll();
        if (status != STATUS_OK && child.task->closing() && child.state != "draining") { child.state = "draining"; ++epoch_; }
        if (status == STATUS_OK) {
            const auto result = child.task->collect(); child.result = static_cast<status_t>(result.value);
            child.task.reset(); (void)child.ready.close(); child.state = "stopped"; if (!child.reusable) child.desired = false; ++epoch_;
        } else if (status != STATUS_WOULD_BLOCK && !proc_retryable(status)) return status;
    }
    waiters_.poll();
    for (size_t count = 0; count < 32; ++count) {
        const auto status = object_.poll();
        if (status == STATUS_WOULD_BLOCK) return STATUS_OK;
        if (status != STATUS_OK) return status;
    }
    return object_.arm();
}
auto mgr::request(std::string_view method, ubus_request& request, ubus_msg& response) -> status_t {
    if (method == "List") {
        ubus_data items{ubus_type::Array};
        for (const auto& child : children_) {
            auto row = mgr_row(child->task.get(), child->id, child->gen, child->name, child->state, request.facet() & mgr_control);
            if (!child->task) row.put("result", int64_t(child->result));
            items.push(row);
        }
        response.data.put("epoch", epoch_).put("items", items); return STATUS_OK;
    }
    if (method == "Spawn" || method == "Activate") {
        const auto name = *request.data["name"].as<std::string_view>();
        if (name.empty() || name.find('/') != name.npos || name.find(char{}) != name.npos) return STATUS_BAD_ARGS;
        for (const auto& child : children_) if (child->name == name) {
            if (method == "Spawn") return STATUS_BUSY;
            child->desired = true;
            if (!child->task) {
                const auto status = start(*child);
                if (status != STATUS_OK) return status;
            }
            response.data.put("id", child->id).put("gen", child->gen); return STATUS_OK;
        }
        const auto program = request.data["program"].as<std::string_view>();
        if (!program) return STATUS_NOT_FOUND;
        if (!image_(*program)) return STATUS_NOT_FOUND;
        proc_quota quota{.bytes=4 * 1024 * 1024, .caps=160,
            .kinds=proc_base_kinds | OBJ_BIT(ObjChannel), .slots=160, .pages=24};
        const auto limits = request.data["resources"];
        if (limits) {
            quota.kinds = limits["kinds"].as<uint64_t>().value_or(quota.kinds);
            quota.bytes = limits["bytes"].as<uint64_t>().value_or(quota.bytes);
            quota.caps = limits["caps"].as<uint64_t>().value_or(quota.caps);
            quota.slots = quota.caps; quota.pages = (quota.slots + 7) / 8 + 4;
            if (limits["children"].as<bool>().value_or(false)) quota.pool_perms |= RIGHT_SPLIT;
        }
        std::vector<std::string> args{std::string{name}};
        for (const auto arg : request.data["argv"].values()) {
            const auto text = arg.template as<std::string_view>();
            if (!text || text->find(char{}) != text->npos) return STATUS_BAD_ARGS;
            args.emplace_back(*text);
        }
        // Inspect the receiving CSpace, so a pure Send cap need not itself
        // carry Inspect. Snapshot all received views before allocating grants.
        std::array<CapView, CHANNEL_MAX_CAPS> views{};
        if (request.count) {
            cap_t cursor{}; uint64_t epoch{};
            do {
                const auto status = sys::cspace_info(rt_startup().cspace, cursor, 0, epoch).status;
                if (status != STATUS_OK) return status;
                CSpaceInfo page; ::memcpy(&page, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(page));
                for (const auto& info : std::span{page.records, page.count})
                    for (size_t i = 0; i < request.count; ++i)
                        if (info.selector == request.caps[i].selector()) views[i] = info.view;
                cursor = page.cursor; epoch = page.epoch;
            } while (cursor);
        }
        std::deque<mgr_child::grant> owned;
        bool reusable{true};
        for (const auto item : request.data["caps"].values()) {
            const auto selector = request.capability(item["cap"]);
            size_t index{};
            while (index < request.count && request.caps[index].selector() != selector) ++index;
            const auto rights = item["rights"].template as<uint64_t>();
            if (index == request.count || !rights || item.name().empty() || item.name().find(char{}) != std::string_view::npos || !views[index].kind) return STATUS_BAD_ARGS;
            const auto& view = views[index];
            if ((*rights & ~view.rights) != 0) return STATUS_BAD_RIGHTS;
            const bool linear = !(view.rights & RIGHT_DUPLICATE);
            if (linear && *rights != view.rights) return STATUS_BAD_RIGHTS;
            reusable &= !linear;
            owned.push_back({std::string{item.name()}, request.take(item["cap"]), *rights, view.kind,
                item["bytes"].template as<uint64_t>().value_or(0), linear});
        }
        auto& child = add(std::string{name}, std::string{*program}, quota);
        child.args = std::move(args); child.owned = std::move(owned);
        child.await_ready = false; child.reusable = reusable;
        const auto status = start(child);
        if (status != STATUS_OK) { child.desired = false; return status; }
        response.data.put("id", child.id).put("gen", child.gen); return STATUS_OK;
    }
    auto* child = find(*request.data["id"].as<uint64_t>());
    if (!child) return STATUS_NOT_FOUND;
    if (request.data["gen"].as<uint64_t>() != child->gen) return STATUS_STALE;
    if (method == "Action") return action(*child, *request.data["action"].as<std::string_view>());
    if (!child->task) return STATUS_CLOSED;
    if (method == "Changed") return waiters_.accept(child->scope ? child->scope : child->task->scope(), child->task->cspace(), request, response);
    if (method == "Inspect" && request.data["facet"].as<std::string_view>() == "dependencies") {
        ubus_data items{ubus_type::Array};
        const auto append = [&](std::string_view name, uint16_t kind, word_t rights) {
            ubus_data item; item.put("name", name).put("kind", uint64_t(kind)).put("rights", rights); items.push(item);
        };
        for (const auto& grant : child->grants) {
            append(grant.key.name, grant.key.kind, std::holds_alternative<word_t>(grant.limit) ? std::get<word_t>(grant.limit) : std::get<CapView>(grant.limit).rights);
        }
        for (const auto& grant : child->owned) append(grant.name, grant.kind, grant.rights);
        response.data.put("items", items); return STATUS_OK;
    }
    if (method == "Inspect") return mgr_inspect(child->scope ? child->scope : child->task->scope(), child->task->cspace(), request.data.view(), response);
    if (method == "CloseCap") return sys::cap_close(*request.data["selector"].as<cap_t>(), child->task->cspace()).status;
    if (method == "Revoke") return mgr_revoke(child->task->cspace(), request.data.view(), response);
    return STATUS_INVALID_OP;
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

auto mgr::open() -> status_t {
    for (const auto name : {"List", "Action", "Inspect", "Changed", "CloseCap", "Revoke"}) {
        const auto status = mgr_method(object_, name, [this, name](auto& q, auto& r) { return request(name, q, r); });
        if (status != STATUS_OK) return status;
    }
    for (const auto name : {"Spawn", "Activate"}) {
        const auto status = object_.add(name, {{"name", ubus_type::String}, {"program", ubus_type::String, true},
            {"argv", ubus_type::Array, true, UINT64_MAX, {{"", ubus_type::String}}},
            {"caps", ubus_type::Record, true, UINT64_MAX, {{"", ubus_type::Record, false, UINT64_MAX,
                {{"cap", ubus_type::Cap}, {"rights", ubus_type::U64}, {"bytes", ubus_type::U64, true}}}}},
            {"resources", ubus_type::Record, true, UINT64_MAX,
                {{"bytes", ubus_type::U64, true}, {"caps", ubus_type::U64, true}, {"kinds", ubus_type::U64, true}, {"children", ubus_type::Bool, true}}}},
            {{"id", ubus_type::U64}, {"gen", ubus_type::U64}}, [this, name](auto& q, auto& r) { return request(name, q, r); }, mgr_control);
        if (status != STATUS_OK) return status;
    }
    return object_.open();
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
