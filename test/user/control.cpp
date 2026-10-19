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
static auto cap_record(cap_t selector) -> CapInfo {
    const auto& env = rt_startup();
    cap_t cursor{}; uint64_t epoch{};
    do {
        check(sys::cspace_info(env.cspace, cursor, 0, epoch).status == STATUS_OK);
        CSpaceInfo page; ::memcpy(&page, reinterpret_cast<void*>(env.ipc), sizeof(page));
        check(page.version == CSPACE_INFO_VERSION && page.size == sizeof(page));
        for (uint32_t i = 0; i < page.count; ++i)
            if (page.records[i].selector == selector) return page.records[i];
        cursor = page.cursor; epoch = page.epoch;
    } while (cursor);
    check(false);
    return {};
}
[[noreturn]] static void waiting_thread(cap_t source, word_t deadline) {
    const auto result = sys::wait_one(source, 0, 0, deadline);
    sys::exit(result.status == STATUS_TIMED_OUT ? STATUS_OK : STATUS_INTERNAL);
}
static void migrate_waiter(const rt_env& env) {
    auto stack = vm_area::create(env.alloc(), env.vspace, 0, 16384);
    check(bool(stack));
    // This scenario migrates the timed Wait, not its initial page faults.
    *reinterpret_cast<volatile uint8_t*>(stack->address + stack->size - 1) = 0;
    (void)*reinterpret_cast<volatile const uint8_t*>(&waiting_thread);
    const auto storage = sys::memory_create(env.alloc(), 4096, VM_READ | VM_WRITE);
    check(storage.status == STATUS_OK);
    cap_owner descriptor{storage.value};
    const auto deadline = rt_after_ms(1000).value();
    const ThreadInit init{.version=THREAD_START_VERSION, .flags=0,
        .entry=reinterpret_cast<word_t>(&waiting_thread), .stack=stack->address + stack->size,
        .arguments={env.events, deadline}, .ipc={}};
    ::memcpy(reinterpret_cast<void*>(env.ipc), &init, sizeof(init));
    check(sys::memory_write(descriptor.selector(), 0, 0, sizeof(init)).status == STATUS_OK);
    const auto created = sys::thread_create(env.alloc(), env.vspace, env.cspace, descriptor.selector(), 0, 0);
    check(created.status == STATUS_OK);
    cap_owner thread{created.value};
    const auto allocated = sys::sc_create(env.alloc(), env.domain, 0, 0, 1024,
        (word_t{1} << env.cpus) - 1);
    check(allocated.status == STATUS_OK);
    cap_owner sc{allocated.value};
    check(sys::sc_bind(sc.selector(), thread.selector()).status == STATUS_OK);
    check(sys::execution_set(thread.selector(), true).status == STATUS_OK);
    ThreadInfo info;
    do {
        sys::yield();
        check(sys::thread_info(thread.selector()).status == STATUS_OK);
        ::memcpy(&info, reinterpret_cast<void*>(env.ipc), sizeof(info));
    } while (!(info.flags & THREAD_WAITING));
    for (word_t cpu = 0; cpu < env.cpus; ++cpu) {
        const auto moved = sys::execution_move(thread.selector(), cpu, info.home_epoch);
        check(moved.status == STATUS_OK);
        do {
            sys::yield();
            check(sys::thread_info(thread.selector()).status == STATUS_OK);
            ::memcpy(&info, reinterpret_cast<void*>(env.ipc), sizeof(info));
        } while (info.flags & THREAD_MIGRATING);
        check(info.cpu == cpu && info.home_epoch == moved.value && (info.flags & THREAD_WAITING));
    }
    check(sys::wait_one(thread.selector(), SOURCE_READY, 0, 0, 0, SOURCE_EXIT).status == STATUS_OK);
    const auto ended = sys::exit_query(thread.selector());
    check(ended.status == STATUS_OK && ended.value && ended.value2 == STATUS_OK && sys::clock_now().value >= deadline);
    check(sys::object_destroy(thread.selector()).status == STATUS_OK);
    check(sys::object_destroy(sc.selector()).status == STATUS_OK);
}
[[noreturn]] static void busy_thread(word_t deadline) {
    while (sys::clock_now().value < deadline) {}
    sys::exit(STATUS_OK);
}
static void move_domain(const rt_env& env) {
    const auto mask = (word_t{1} << env.cpus) - 1;
    auto create = [&](cap_t parent) {
        const auto made = sys::domain_create(env.alloc(), parent, 0, 0, 1024, mask);
        check(made.status == STATUS_OK); return cap_owner{made.value};
    };
    auto old = create(env.domain), next = create(env.domain);
    auto branch = create(old.selector()), leaf = create(branch.selector());
    check(sys::domain_move(branch.selector(), leaf.selector()).status == STATUS_BAD_ARGS);
    auto stack = vm_area::create(env.alloc(), env.vspace, 0, 16384);
    check(bool(stack));
    const auto memory = sys::memory_create(env.alloc(), 4096, VM_READ | VM_WRITE);
    check(memory.status == STATUS_OK);
    cap_owner descriptor{memory.value};
    const auto deadline = rt_after_ms(300).value();
    const ThreadInit init{.version=THREAD_START_VERSION, .flags=0,
        .entry=reinterpret_cast<word_t>(&busy_thread), .stack=stack->address + stack->size,
        .arguments={deadline}, .ipc={}};
    ::memcpy(reinterpret_cast<void*>(env.ipc), &init, sizeof(init));
    check(sys::memory_write(descriptor.selector(), 0, 0, sizeof(init)).status == STATUS_OK);
    const auto made = sys::thread_create(env.alloc(), env.vspace, env.cspace, descriptor.selector(), 0, 0);
    check(made.status == STATUS_OK);
    cap_owner thread{made.value};
    const auto context = sys::sc_create(env.alloc(), leaf.selector(), 0, 0, 1024, mask);
    check(context.status == STATUS_OK);
    cap_owner sc{context.value};
    check(sys::sc_bind(sc.selector(), thread.selector()).status == STATUS_OK);
    check(sys::execution_set(thread.selector(), true).status == STATUS_OK);
    for (unsigned i = 0; i < 20; ++i) sys::yield();
    const auto moved = sys::domain_move(branch.selector(), next.selector());
    check(moved.status == STATUS_OK);
    DomainInfo info;
    do {
        sys::yield();
        check(sys::domain_info(branch.selector()).status == STATUS_OK);
        ::memcpy(&info, reinterpret_cast<void*>(env.ipc), sizeof(info));
    } while (info.applied != moved.value);
    check(info.phase == DOMAIN_OPEN && info.status == STATUS_OK && !info.escrow);
    check(sys::domain_close(old.selector()).status == STATUS_OK);
    check(sys::wait_one(old.selector(), DOMAIN_DRAINED).status == STATUS_OK);
    check(sys::wait_one(thread.selector(), SOURCE_READY, 0, 0, 0, SOURCE_EXIT).status == STATUS_OK);
    const auto ended = sys::exit_query(thread.selector());
    check(ended.status == STATUS_OK && ended.value && ended.value2 == STATUS_OK);
    check(sys::object_destroy(thread.selector()).status == STATUS_OK);
    check(sys::object_destroy(sc.selector()).status == STATUS_OK);
    check(sys::domain_close(next.selector()).status == STATUS_OK);
    check(sys::wait_one(next.selector(), DOMAIN_DRAINED).status == STATUS_OK);
}
int main() {
    const auto& env = rt_startup();
    // The same user frame and exclusive SC cross every actual home.
    for (word_t cpu = 0; cpu < env.cpus; ++cpu) {
        check(sys::thread_info(0).status == STATUS_OK);
        ThreadInfo before; ::memcpy(&before, reinterpret_cast<void*>(env.ipc), sizeof(before));
        const auto moved = sys::execution_move(0, cpu, before.home_epoch);
        check(moved.status == STATUS_OK);
        ThreadInfo after;
        do {
            sys::yield();
            check(sys::thread_info(0).status == STATUS_OK);
            ::memcpy(&after, reinterpret_cast<void*>(env.ipc), sizeof(after));
        } while (after.flags & THREAD_MIGRATING);
        check(after.cpu == cpu && after.home_epoch == moved.value);
        if (after.home_epoch != before.home_epoch)
            check(sys::execution_move(0, cpu, before.home_epoch).status == STATUS_STALE);
        check(sys::wait_one(env.events, 0, 0, rt_after_ms(1).value()).status == STATUS_TIMED_OUT);
    }
    migrate_waiter(env);
    move_domain(env);
    rpc_client root, observer;
    check(root.open(env.selector(mg_tasks), env.alloc()) == STATUS_OK);
    check(observer.open(env.selector(mg_tasks_view), env.alloc()) == STATUS_OK);
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
    mg_task current{};
    do {
        current = snapshot(root, store.id);
        if (current.incarnation != store.incarnation && current.state == static_cast<uint32_t>(mg_state::Running)) break;
        check(rt_admit(deadline) == STATUS_OK);
        check(sys::wait_one(env.events, 0, 0, rt_after_ms(10).value()).status == STATUS_TIMED_OUT);
    } while (true);
    request = mg_request(mg_op::Action, store.id, store.incarnation, 0, mg_action::Pause);
    check(root.exchange(request) == STATUS_STALE);
    request = mg_request(mg_op::CloseCap, store.id, store.incarnation, 1);
    check(root.exchange(request) == STATUS_STALE);
    check(snapshot(root, store.id).state == static_cast<uint32_t>(mg_state::Running));

    // A read-only owner Watch remains idle after registration, then observes
    // the actual execution transition. The reply route is reused on renewal.
    request = mg_request(mg_op::Action, current.id, current.incarnation, 0, mg_action::Pause);
    check(root.exchange(request) == STATUS_OK);
    while (snapshot(root, current.id).state != static_cast<uint32_t>(mg_state::Suspended)) {
        check(rt_admit(deadline) == STATUS_OK); sys::yield();
    }
    request = mg_request(mg_op::Changed, current.id, current.incarnation);
    check(observer.exchange(request) == STATUS_OK && request.value);
    const auto baseline = request.value;
    rpc_pending watch;
    request = mg_request(mg_op::Changed, current.id, current.incarnation, 0, {}, baseline);
    request.deadline = rt_after_ms(1000).value();
    check(watch.begin(env.selector(mg_tasks_view), env, request) == STATUS_OK);
    for (unsigned i = 0; i < 10; ++i) {
        msg_frame response;
        check(watch.poll(response) == STATUS_WOULD_BLOCK);
        check(sys::wait_one(env.events, 0, 0, rt_after_ms(1).value()).status == STATUS_TIMED_OUT);
    }
    request = mg_request(mg_op::Action, current.id, current.incarnation, 0, mg_action::Resume);
    check(root.exchange(request) == STATUS_OK);
    msg_frame changed;
    for (;;) {
        const auto status = watch.poll(changed);
        if (status == STATUS_OK) break;
        check(status == STATUS_WOULD_BLOCK);
        (void)ev_wait(env.events, rt_after_ms(10).value());
    }
    check(changed.value != baseline && watch.close() == STATUS_OK);

    check(sys::account_info(env.pool).status == STATUS_OK);
    AccountInfo group; ::memcpy(&group, reinterpret_cast<void*>(env.ipc), sizeof(group));
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

    // A public Op's Done permits immediate reuse of the same VM provider.
    vm_area area;
    check(area.open(env.vspace, 0, 4096) == STATUS_OK);
    VmReq clear{};
    ::memcpy(reinterpret_cast<void*>(env.ipc), &clear, sizeof(clear));
    const auto submitted = sys::vm_submit(env.alloc(), area.region.selector(), VM_OP_CLEAR);
    check(submitted.status == STATUS_PENDING && submitted.value);
    cap_owner operation{submitted.value};
    check(sys::wait_one(operation.selector(), OP_DONE).status == STATUS_OK);
    const auto completed = sys::op_query(operation.selector());
    check(completed.status == STATUS_OK && completed.value2 == OP_DONE);
    check(sys::op_cancel(operation.selector()).status == STATUS_TOO_LATE);
    check(area.close() == STATUS_OK);
    check(operation.close() == STATUS_OK);

    std::array<cap_owner, 17> notifications;
    for (auto& cap : notifications) {
        const auto created = sys::notification_create(env.alloc(), 1);
        check(created.status == STATUS_OK); cap = cap_owner{created.value};
    }
    // Copy shares authority; Derive creates an independently revocable branch.
    const auto original = cap_record(notifications[0].selector());
    check(original.state == CAP_INFO_VALID && original.object_id && original.grant_id);
    const auto copy = sys::cap_duplicate(notifications[0].selector(), 0, RIGHT_INSPECT | RIGHT_SIGNAL);
    const auto child = sys::cap_delegate(notifications[0].selector(), 0,
        RIGHT_INSPECT | RIGHT_SIGNAL | RIGHT_REVOKE);
    check(copy.status == STATUS_OK && child.status == STATUS_OK);
    cap_owner copied{copy.value}, derived{child.value};
    const auto copied_info = cap_record(copied.selector()), child_info = cap_record(derived.selector());
    check(copied_info.object_id == original.object_id && copied_info.grant_id == original.grant_id);
    check(child_info.object_id == original.object_id && child_info.grant_id != original.grant_id);
    check(sys::cap_revoke(derived.selector()).status == STATUS_OK);
    const auto revoked = cap_record(derived.selector());
    check(revoked.state == CAP_INFO_UNAVAILABLE && revoked.grant_id == child_info.grant_id
        && revoked.object_id == original.object_id);
    check(sys::notification_signal(copied.selector()).status == STATUS_OK);
    check(cap_record(notifications[0].selector()).grant_id == original.grant_id);
    check(copied.close() == STATUS_OK && derived.close() == STATUS_OK);
    check(sys::cspace_info(env.cspace).status == STATUS_OK);
    CSpaceInfo caps; ::memcpy(&caps, reinterpret_cast<void*>(env.ipc), sizeof(caps));
    check(caps.count == CAP_INFO_PAGE && caps.cursor);
    bool found{};
    for (auto& cap : notifications) if (cap.selector() == caps.cursor) {
        check(cap.close() == STATUS_OK); found = true; break;
    }
    check(found && sys::cspace_info(env.cspace, caps.cursor, 0, caps.epoch).status == STATUS_STALE);
    const auto replacement = sys::notification_create(env.alloc(), 1);
    check(replacement.status == STATUS_OK && replacement.value != caps.cursor);
    cap_owner reused{replacement.value};
    check(sys::cspace_info(env.cspace, caps.cursor, 0, caps.epoch).status == STATUS_STALE);
    str_writer{env.selector(rt_stdout)}.write("[control] scoped badges, stale instances/cursors and real VM/account stats ok\n");
}
