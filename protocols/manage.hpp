#pragma once
#include <protocols/msg.hpp>
#include <string.h>

inline constexpr rt_req mg_tasks{"tasks.control", 0x5441534b, ObjChannel};
inline constexpr rt_req mg_tasks_view{"tasks.view", 0x5441534b, ObjChannel};
inline constexpr rt_req mg_devices_view{"devices.view", 0x44455653, ObjChannel};
inline constexpr rt_req mg_devices{"devices.control", 0x44455653, ObjChannel};
inline constexpr rt_req mg_jobs{"jobs.control", 0x4a4f4253, ObjChannel};
enum class mg_op : uint64_t { Tasks = 256, Task, Action, Caps, Dependencies, CloseCap, Devices, DeviceAction, Members, Object, Links, Grants, Usage, Ops, GrantView, Revoke, Changed };
enum class mg_action : int64_t { Pause, Resume, Stop, Start, Restart };
enum mg_right : uint32_t {
    mg_observe = 1, mg_control = 2, mg_close = 4,
    mg_pause = 8, mg_resume = 16, mg_stop = 32, mg_start = 64, mg_restart = 128,
};
struct mg_target { uint64_t incarnation{}, argument{}, epoch{}, cursor{}, selector{}, selected{}; };
struct mg_task {
    uint64_t id, incarnation, owner, runtime, resident, charged, limit;
    uint32_t state, actions, member;
    char name[12];
};
struct mg_device {
    uint64_t id, incarnation, driver, mmio, bytes;
    uint32_t requester, irq, state, rid;
    uint64_t iospace;
    uint32_t actions;
    char name[12];
};
// Deployment edges known to the owner, distinct from capability lineage.
struct mg_dependency {
    uint64_t provider, incarnation;
    uint32_t protocol;
    uint16_t major, minor;
    uint32_t flags; // 1 restart, 2 reconnect, 4 pipeline; 0 origin unknown.
    char name[32];
};
static_assert(sizeof(mg_task) == sizeof(msg_frame::data));
static_assert(sizeof(mg_device) == sizeof(msg_frame::data));

inline auto mg_request(mg_op operation, uint64_t id, uint64_t incarnation = 0,
                       uint64_t argument = 0, mg_action action = {}, uint64_t epoch = 0) noexcept -> msg_frame {
    msg_frame message{.operation = static_cast<uint64_t>(operation), .status = static_cast<int64_t>(action),
        .value = id, .size = sizeof(mg_target)};
    const mg_target target{incarnation, argument, epoch};
    ::memcpy(message.data, &target, sizeof(target));
    return message;
}
enum class mg_state : uint32_t { Stopped, Draining, Starting, Running, Failed, Suspending, Suspended, Done };
template<class T> inline void mg_pack(msg_frame& message, const T& value) noexcept {
    static_assert(sizeof(T) <= sizeof(message.data));
    message.status = STATUS_OK; message.size = sizeof(T);
    ::memcpy(message.data, &value, sizeof(T));
}

// Owner-local, bounded long polls. Scope watches retain no observed payload;
// closing the client's reply transport also wakes and removes its registration.
class mg_watch final {
    struct entry {
        cap_owner reply{};
        cap_t scope{};
        uint64_t object{}, selector{}, selected{};
        std::array<word_t, 3> watch{};
        std::array<word_t, 2> seen{};
        msg_frame response;
    };
    std::array<entry, 8> entries_;
    cap_t port_{};
    void clear(entry& item) noexcept {
        for (auto watch : item.watch) if (watch) (void)sys::port_unwatch(port_, watch);
        item = {};
    }
    static auto facts(const entry& item) noexcept {
        return std::array{sys::source_info(item.scope), item.selector
            ? sys::grant_source(item.scope, item.object, item.selector, item.selected)
            : sys::SysResult{STATUS_OK}};
    }
    static void pack(msg_frame& message, const auto& facts) noexcept {
        message.value = facts[0].value2; message.size = sizeof(uint64_t);
        const uint64_t grant = facts[1].value2; ::memcpy(message.data, &grant, sizeof(grant));
    }
public:
    explicit mg_watch(cap_t port) noexcept : port_(port) {}
    ~mg_watch() { for (auto& item : entries_) clear(item); }
    void accept(cap_t scope, cap_t cspace, const mg_target& query, cap_owner& reply,
                const msg_frame& request, msg_frame& response) noexcept {
        entry prepared{.scope=scope, .object=query.argument, .selector=query.selector, .selected=query.selected,
            .seen={query.epoch, query.cursor}, .response={}};
        if (prepared.selector && !prepared.object) {
            const auto result = sys::object_info(cspace);
            if (!cspace || result.status != STATUS_OK) { response.status = cspace ? result.status : STATUS_DENIED; return; }
            ObjectInfo info; ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info));
            prepared.object = info.object_id;
        }
        const auto sample = facts(prepared);
        response.status = sample[0].status != STATUS_OK ? sample[0].status : sample[1].status;
        pack(response, sample);
        if (response.status != STATUS_OK || sample[0].value2 != query.epoch || sample[1].value2 != query.cursor) return;
        if (!request.deadline || !request.sequence) { response.status = STATUS_BAD_ARGS; return; }
        for (auto& item : entries_) if (!item.reply) {
            const auto changed = sys::port_watch(port_, scope, 0, query.epoch, 0, WATCH_CHANGED);
            if (changed.status != STATUS_OK) { response.status = changed.status; return; }
            prepared.watch[0] = changed.value;
            if (prepared.selector) {
                const auto grant = sys::port_grant(port_, scope, prepared.object, prepared.selector, prepared.selected, query.cursor);
                if (grant.status != STATUS_OK) { clear(prepared); response.status = grant.status; return; }
                prepared.watch[1] = grant.value;
            }
            const auto terminal = sys::port_watch(port_, reply.selector(), SOURCE_TERMINAL, 0, 0, 0, SOURCE_WRITE);
            if (terminal.status != STATUS_OK) { clear(prepared); response.status = terminal.status; return; }
            prepared.watch[2] = terminal.value;
            prepared.response = response; prepared.response.deadline = request.deadline;
            prepared.reply = std::move(reply); item = std::move(prepared); response.status = STATUS_PENDING;
            return;
        }
        response.status = STATUS_BUSY;
    }
    auto poll() noexcept -> word_t {
        const auto now = sys::clock_now().value;
        word_t deadline{};
        for (auto& item : entries_) if (item.reply) {
            const auto peer = sys::source_info(item.reply.selector(), SOURCE_WRITE);
            if (peer.status != STATUS_OK || (peer.value & SOURCE_TERMINAL)) { clear(item); continue; }
            const auto sample = facts(item);
            const auto status = sample[0].status != STATUS_OK ? sample[0].status
                : sample[1].status != STATUS_OK ? sample[1].status
                : sample[0].value2 != item.seen[0] || sample[1].value2 != item.seen[1] ? STATUS_OK
                : now >= item.response.deadline ? STATUS_TIMED_OUT : STATUS_WOULD_BLOCK;
            if (status != STATUS_WOULD_BLOCK) {
                auto response = item.response; response.status = status; pack(response, sample);
                (void)msg_send(item.reply.selector(), response, false); clear(item); continue;
            }
            for (size_t i = 0; i < item.seen.size(); ++i)
                if (item.watch[i]) (void)sys::port_arm(port_, item.watch[i], item.seen[i]);
            if (!deadline || item.response.deadline < deadline) deadline = item.response.deadline;
        }
        return deadline;
    }
};

// Cap records use the reply's sample word for the local selector. The payload
// holds identities and the full typed View within the bounded service envelope.
struct mg_cap { uint64_t object_id, grant_id; CapView view; };
inline void mg_pack(msg_frame& message, const CapInfo& info) noexcept {
    mg_pack(message, mg_cap{info.object_id, info.grant_id, info.view});
    message.deadline = info.selector;
}
inline auto mg_cap_info(const msg_frame& message) noexcept -> CapInfo {
    mg_cap record; ::memcpy(&record, message.data, sizeof(record));
    return {message.deadline, record.object_id, record.grant_id,
        (record.view.kind != ObjInvalid ? CAP_INFO_VALID : CAP_INFO_UNAVAILABLE)
            | (!record.object_id ? CAP_INFO_BOUNDARY : 0U), 0, record.view};
}

// An owner projects its actual Scope, without exporting an Inspect cap.
inline auto mg_caps(cap_t scope, cap_t cspace, cap_t cursor, uint64_t epoch, uint64_t object = 0) noexcept -> std::expected<CSpaceInfo, status_t> {
    sys::SysResult result{};
    if (!object) {
        if (!cspace) return std::unexpected(STATUS_DENIED);
        result = sys::object_info(cspace);
        if (result.status != STATUS_OK) return std::unexpected(result.status);
        ObjectInfo info;
        ::memcpy(&info, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(info));
        object = info.object_id;
    }
    result = sys::scope_query(scope, object, SCOPE_CAPS, cursor, epoch);
    if (result.status != STATUS_OK) return std::unexpected(result.status);
    CSpaceInfo page;
    ::memcpy(&page, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(page));
    return page;
}

struct mg_domain {
    uint64_t budget, period, available, debt, escrow, runtime, work_runtime, requested, applied;
    uint32_t phase; int32_t status;
};
struct mg_operation { int64_t status; uint64_t value, phase; };
struct mg_grant {
    uint64_t grant_id, parent_id, object_id, rights;
    uint32_t kind, state, flags, reserved;
};
inline auto mg_query(mg_op operation, uint64_t task, mg_target query) noexcept -> msg_frame {
    auto message = mg_request(operation, task);
    ::memcpy(message.data, &query, sizeof(query));
    return message;
}
inline void mg_project(cap_t scope, cap_t cspace, mg_op op, const mg_target& query, msg_frame& message) noexcept {
    message.status = STATUS_NOT_FOUND;
    if (op == mg_op::Caps) {
        const auto page = mg_caps(scope, cspace, query.argument, query.epoch, query.selected);
        if (!page) { message.status = page.error(); return; }
        if (!page->count) return;
        mg_pack(message, page->records[0]); message.value = page->epoch;
        return;
    }
    const auto ipc = reinterpret_cast<void*>(rt_startup().ipc);
    auto object = query.argument;
    if (!object) {
        if ((op == mg_op::Grants || op == mg_op::GrantView) && !cspace) { message.status = STATUS_DENIED; return; }
        const auto status = sys::object_info(op == mg_op::Grants || op == mg_op::GrantView ? cspace : scope).status;
        if (status != STATUS_OK) { message.status = status; return; }
        ObjectInfo info; ::memcpy(&info, ipc, sizeof(info)); object = info.object_id;
    }
    const auto facet = op == mg_op::Members ? SCOPE_MEMBERS : op == mg_op::Object ? SCOPE_INFO
        : op == mg_op::Links ? SCOPE_LINKS : op == mg_op::Usage ? SCOPE_USAGE : op == mg_op::Ops ? SCOPE_OPS : SCOPE_GRANTS;
    if (facet == SCOPE_GRANTS) {
        const GrantQuery input{query.selector, query.selected, query.cursor, query.epoch};
        ::memcpy(ipc, &input, sizeof(input));
    }
    const auto result = sys::scope_query(scope, object, facet, query.cursor, query.epoch);
    if (facet == SCOPE_OPS && result.value2) {
        mg_pack(message, mg_operation{result.status, result.value, result.value2}); return;
    }
    const auto status = result.status;
    if (status != STATUS_OK) { message.status = status; return; }
    if (op == mg_op::Usage) {
        const auto size = static_cast<const uint32_t*>(ipc)[1];
        if (size == sizeof(AccountInfo)) { AccountInfo info; ::memcpy(&info, ipc, sizeof(info)); mg_pack(message, info); }
        else if (size == sizeof(DomainInfo)) {
            DomainInfo info; ::memcpy(&info, ipc, sizeof(info));
            mg_pack(message, mg_domain{info.budget, info.period, info.available, info.debt, info.escrow,
                info.runtime, info.work_runtime, info.requested, info.applied, info.phase, info.status});
        } else message.status = STATUS_INVALID_OP;
    } else if (op == mg_op::Members) {
        ScopeMembers page; ::memcpy(&page, ipc, sizeof(page));
        if (!page.count) {
            if (page.more) { message.status = STATUS_OK; message.size = 0; }
            message.deadline = page.next.last;
        } else { mg_pack(message, page.records[0]); message.deadline = page.records[0].object_id; }
        message.value = page.next.version;
    } else if (op == mg_op::Object) {
        ScopeObject info; ::memcpy(&info, ipc, sizeof(info)); mg_pack(message, info);
    } else if (op == mg_op::Links) {
        LinkRecords page; ::memcpy(&page, ipc, sizeof(page));
        if (!page.count) {
            if (page.more) { message.status = STATUS_OK; message.size = 0; }
            message.deadline = page.cursor;
        } else { mg_pack(message, page.records[0]); message.deadline = page.records[0].kind == LINK_VM || page.records[0].kind == LINK_IO
                ? page.records[0].first + page.records[0].bytes : page.records[0].link_id; }
        message.value = page.epoch;
    } else {
        GrantRecords page; ::memcpy(&page, ipc, sizeof(page));
        const auto* row = op == mg_op::GrantView || !query.cursor ? &page.root : page.count ? &page.records[0] : nullptr;
        if (!row) return;
        if (op == mg_op::GrantView) mg_pack(message, row->ceiling);
        else mg_pack(message, mg_grant{row->grant_id, row->parent_id, row->object_id, row->ceiling.rights,
            row->ceiling.kind, row->state, row->flags | page.flags, 0});
        message.deadline = row->grant_id; message.value = page.epoch;
    }
}
