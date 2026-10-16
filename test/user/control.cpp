#include <protocols/manage.hpp>
#include <protocols/stream.hpp>
#include <array>
#include <libk/fmt.hpp>
#include <source_location>

static void check(bool value, std::source_location at = std::source_location::current()) {
    if (value) return;
    str_writer output{rt_startup().selector(rt_stdout)};
    (void)libk::fmt::format_to<"[control] failed line {}\n">(output, at.line());
    sys::exit(STATUS_INTERNAL);
}
static auto snapshot(rpc_client& owner, uint64_t id, std::source_location at = std::source_location::current()) -> mg_task {
    auto request = mg_request(mg_op::Task, id);
    const auto status = owner.exchange(request);
    if (status != STATUS_OK) {
        str_writer output{rt_startup().selector(rt_stdout)};
        (void)libk::fmt::format_to<"[control] snapshot id {} status {}\n">(output, id, status);
    }
    check(status == STATUS_OK && request.size == sizeof(mg_task), at);
    mg_task row; ::memcpy(&row, request.data, sizeof(row)); return row;
}
int main() {
    const auto& env = rt_startup();
    rpc_client root, observer;
    check(root.open(env.selector(mg_tasks), env.pool, env.cspace, env.events) == STATUS_OK);
    check(observer.open(env.selector(mg_tasks_view), env.pool, env.cspace, env.events) == STATUS_OK);
    uint64_t cursor{};
    mg_task store{};
    for (;;) {
        auto request = mg_request(mg_op::Tasks, cursor);
        const auto status = root.exchange(request);
        if (status == STATUS_NOT_FOUND) break;
        check(status == STATUS_OK && request.size == sizeof(mg_task));
        mg_task row; ::memcpy(&row, request.data, sizeof(row)); cursor = row.id;
        if (std::string_view{row.name} == "store") store = row;
    }
    check(store.id && (store.actions & mg_control));
    const auto readonly = snapshot(observer, store.id);
    check(!(readonly.actions & mg_control));
    auto request = mg_request(mg_op::Action, store.id, store.incarnation, 0, mg_action::Stop);
    check(observer.exchange(request) == STATUS_DENIED);
    request = mg_request(mg_op::Action, store.id, store.incarnation, 0, mg_action::Restart);
    check(root.exchange(request) == STATUS_OK);
    const auto deadline = rt_after_ms(5000).value();
    const auto timer = sys::notification_create(env.pool, 1);
    check(timer.status == STATUS_OK);
    cap_owner tick{timer.value};
    mg_task current{};
    do {
        current = snapshot(root, store.id);
        if (current.incarnation != store.incarnation && current.state == static_cast<uint32_t>(mg_state::Running)) break;
        check(rt_admit(deadline) == STATUS_OK);
        check(sys::notification_wait(tick.selector(), rt_after_ms(10).value()).status == STATUS_TIMED_OUT);
    } while (true);
    request = mg_request(mg_op::Action, store.id, store.incarnation, 0, mg_action::Pause);
    check(root.exchange(request) == STATUS_STALE);
    request = mg_request(mg_op::CloseCap, store.id, store.incarnation, 1);
    check(root.exchange(request) == STATUS_STALE);
    check(snapshot(root, store.id).state == static_cast<uint32_t>(mg_state::Running));

    check(sys::group_info(env.pool).status == STATUS_OK);
    GroupInfo group; ::memcpy(&group, reinterpret_cast<void*>(env.ipc), sizeof(group));
    check(group.memory_available <= group.memory_limit && group.caps_available <= group.cap_limit);
    check(sys::vspace_info(env.vspace).status == STATUS_OK);
    VSpaceInfo memory; ::memcpy(&memory, reinterpret_cast<void*>(env.ipc), sizeof(memory));
    check(memory.resident >= 4096 && memory.mapped >= memory.resident);
    const auto view = sys::vm_slice(env.vspace, env.ipc, 4096, VM_READ | VM_WRITE, RIGHT_INSPECT);
    check(view.status == STATUS_OK);
    cap_owner scope{view.value};
    check(sys::vspace_info(scope.selector()).status == STATUS_DENIED);
    check(sys::vspace_records(scope.selector()).status == STATUS_OK);
    VmRecords mappings; ::memcpy(&mappings, reinterpret_cast<void*>(env.ipc), sizeof(mappings));
    check(mappings.count == 1 && mappings.records[0].address == env.ipc
        && mappings.records[0].bytes == 4096 && mappings.records[0].resident == 4096);

    std::array<cap_owner, 17> notifications;
    for (auto& cap : notifications) {
        const auto created = sys::notification_create(env.pool, 1);
        check(created.status == STATUS_OK); cap = cap_owner{created.value};
    }
    check(sys::cspace_info(env.cspace).status == STATUS_OK);
    CSpaceInfo caps; ::memcpy(&caps, reinterpret_cast<void*>(env.ipc), sizeof(caps));
    check(caps.count == CAP_INFO_PAGE && caps.cursor);
    bool found{};
    for (auto& cap : notifications) if (cap.selector() == caps.cursor) {
        check(cap.close() == STATUS_OK); found = true; break;
    }
    check(found && sys::cspace_info(env.cspace, caps.cursor).status == STATUS_STALE);
    const auto replacement = sys::notification_create(env.pool, 1);
    check(replacement.status == STATUS_OK && replacement.value != caps.cursor);
    cap_owner reused{replacement.value};
    check(sys::cspace_info(env.cspace, caps.cursor).status == STATUS_STALE);
    str_writer{env.selector(rt_stdout)}.write("[control] scoped badges, stale instances/cursors and real VM/account stats ok\n");
}
