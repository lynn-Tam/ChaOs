#include <lib/mgr/mgr.hpp>
#include <lib/fs/fs.hpp>
#include <lib/stream/stream.hpp>
#include <servers/devices.hpp>

static auto grant(rt_key name, cap_t cap, word_t rights, uint64_t bytes = 0, word_t badge = 0) -> proc_cap {
    return {.key=name, .bytes=bytes, .cap=cap, .limit=rights, .badge=badge};
}
int main() {
    const auto& env = rt_startup();
    const auto* bundle = env.find(fs_archive);
    if (!bundle) sys::exit(STATUS_INVALID_CAP);
    proc_archive archive;
    auto status = archive.open(bundle->cap, bundle->bytes);
    if (status != STATUS_OK) sys::exit(status);
    mgr owner{[&](auto name) { return archive.image(name); }};
    if ((status = owner.open()) != STATUS_OK) sys::exit(status);

    ch_pair bootstrap;
    if ((status = bootstrap.open(env.alloc(), 2, 2)) != STATUS_OK) sys::exit(status);
    auto& registry = owner.add("ubusd", "ubusd", {.bytes=4 * 1024 * 1024, .caps=256,
        .kinds=proc_base_kinds | OBJ_BIT(ObjChannel), .slots=256, .pages=36});
    registry.await_ready = false;
    registry.grants = {grant(ubus_boot, bootstrap.ends[1].selector(), RIGHT_SEND | RIGHT_RECEIVE, 0, 1),
        grant(ubus_owner, env.scope, RIGHT_INSPECT | RIGHT_DUPLICATE)};
    if ((status = owner.start(registry)) != STATUS_OK) sys::exit(status);
    if ((status = bootstrap.ends[1].close()) != STATUS_OK) sys::exit(status);
    const auto sender = sys::channel_mint(bootstrap.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
    if (sender.status != STATUS_OK) return sender.status;
    cap_owner boot_sender{sender.value};
    ubus_msg config, ready; config.data.put("quota", uint64_t{1024});
    if ((status = ubus_send(boot_sender.selector(), config)) != STATUS_OK) sys::exit(status);
    if ((status = ubus_receive_event(bootstrap.ends[0].selector(), ready, true)) != STATUS_OK) sys::exit(status);
    auto admin = ready.take(ready.data["connector"]);
    ubus_conn directory;
    if ((status = directory.bind(admin.selector())) != STATUS_OK) sys::exit(status);
    auto lookup = mgr_view(directory, "", ubus_list | ubus_resolve | ubus_wait, env.scope, 32);
    auto public_bus = mgr_view(directory, "public", ubus_list | ubus_resolve | ubus_wait, env.scope, 32);
    auto read_bus = mgr_view(directory, "readonly", ubus_list | ubus_resolve | ubus_wait, env.scope, 32);
    if (!lookup) sys::exit(lookup.error());
    if (!public_bus) sys::exit(public_bus.error());
    if (!read_bus) sys::exit(read_bus.error());
    if ((status = owner.directory(admin.selector(), lookup->selector())) != STATUS_OK) sys::exit(status);
    for (const auto& [name, rights] : std::array<std::pair<std::string_view, word_t>, 2>{{
        {"system.control", mgr_observe | mgr_control}, {"system.view", mgr_observe}}})
        if (const auto published = owner.object().publish(directory, name, rights); !published) sys::exit(published.error());

    ch_pair input, output;
    if ((status = input.open(env.alloc(), 16, 0)) != STATUS_OK) sys::exit(status);
    if ((status = output.open(env.alloc(), 16, 0)) != STATUS_OK) sys::exit(status);
    auto& boot = owner.add("boot", "bootfs", {.bytes=4 * 1024 * 1024, .caps=512,
        .kinds=proc_base_kinds | OBJ_BIT(ObjChannel), .slots=512, .pages=68});
    // Immutable executable snapshots belong to deployment, independently of
    // the current provider execution and of the consumers using those images.
    const auto image_budget = sys::account_create(env.alloc(), ((bundle->bytes + 4095) & ~uint64_t{4095}) + 8 * 1024 * 1024,
        0, OBJ_BIT(ObjMem));
    if (image_budget.status != STATUS_OK) return image_budget.status;
    cap_owner images{image_budget.value};
    const auto image_owner = sys::scope_create(env.alloc());
    if (image_owner.status != STATUS_OK) return image_owner.status;
    cap_owner image_scope{image_owner.value};
    boot.grants = {grant(fs_archive, bundle->cap, RIGHT_MAP | RIGHT_DUPLICATE, bundle->bytes),
        grant({"image.pool", ObjAccount}, images.selector(), RIGHT_CREATE | RIGHT_DUPLICATE),
        grant({"image.scope", ObjScope}, image_scope.selector(), RIGHT_CREATE | RIGHT_DUPLICATE)};
    auto& devices = owner.add("devices", "devices", {.bytes=20 * 1024 * 1024, .caps=768,
        .kinds=OBJ_KINDS, .slots=512, .pages=68, .pool_perms=RIGHT_CREATE | RIGHT_SPLIT});
    devices.publications = 192;
    devices.grants = {grant(fs_archive, bundle->cap, RIGHT_MAP | RIGHT_DUPLICATE, bundle->bytes),
        grant(dv_firmware, env.selector(dv_firmware), RIGHT_MAP | RIGHT_DUPLICATE),
        grant(dv_physical, env.selector(dv_physical), RIGHT_CREATE | RIGHT_CONTROL | RIGHT_DUPLICATE),
        grant(dv_config, env.selector(dv_config), RIGHT_MAP | RIGHT_DUPLICATE, env.find(dv_config)->bytes),
        grant(rt_stdin, output.ends[1].selector(), RIGHT_RECEIVE | RIGHT_DUPLICATE),
        grant(rt_stdout, input.ends[1].selector(), RIGHT_SEND | RIGHT_DUPLICATE, 0, 1)};
    auto& vfs = owner.add("vfs", "vfs", {.bytes=4 * 1024 * 1024, .caps=256,
        .kinds=proc_base_kinds | OBJ_BIT(ObjChannel), .slots=256, .pages=36});
    auto& session = owner.add("session", "session", {.bytes=40 * 1024 * 1024, .caps=1408,
        .kinds=proc_base_kinds | OBJ_BIT(ObjChannel), .slots=256, .pages=36, .pool_perms=RIGHT_CREATE | RIGHT_SPLIT});
    session.publications = 16;
    auto user_publish = mgr_view(directory, "public/providers", ubus_all, env.scope, 192);
    if (!user_publish) sys::exit(user_publish.error());
    session.grants = {grant(mgr_children, user_publish->selector(), RIGHT_SEND | RIGHT_DUPLICATE),grant(fs_archive, bundle->cap, RIGHT_MAP | RIGHT_DUPLICATE, bundle->bytes),grant(ubus_directory, public_bus->selector(), RIGHT_SEND | RIGHT_DUPLICATE),
        grant(mgr_read_bus, read_bus->selector(), RIGHT_SEND | RIGHT_DUPLICATE),
        grant(rt_stdout, output.ends[0].selector(), RIGHT_SEND | RIGHT_DUPLICATE, 0, 1),
        grant(rt_stdin, input.ends[0].selector(), RIGHT_RECEIVE | RIGHT_DUPLICATE)};
    auto device_bus = mgr_view(directory, "devices", ubus_list | ubus_resolve | ubus_wait, env.scope, 16);
    if (!device_bus) sys::exit(device_bus.error());
    mgr_child* store{};
    if (archive.image("store")) {
        store = &owner.add("store", "store", {.bytes=8 * 1024 * 1024, .caps=256,
            .kinds=proc_base_kinds | OBJ_BIT(ObjChannel), .slots=256, .pages=36});
        store->grants = {grant(ubus_directory, device_bus->selector(), RIGHT_SEND | RIGHT_DUPLICATE),grant({"image.pool", ObjAccount}, images.selector(), RIGHT_CREATE | RIGHT_DUPLICATE),
            grant({"image.scope", ObjScope}, image_scope.selector(), RIGHT_CREATE | RIGHT_DUPLICATE)};
    }
    const auto await = [&](mgr_child& child) -> status_t {
        auto status = owner.start(child);
        if (status != STATUS_OK) sys::exit(status);
        while (child.state != "running") {
            if ((status = owner.poll()) != STATUS_OK) sys::exit(status);
            if (!child.task || !registry.task) sys::exit(child.result ? child.result : STATUS_PEER_CLOSED);
            if (child.state != "running") {
                status = ev_wait(env.events, owner.deadline()).status;
                if (status != STATUS_OK) sys::exit(status);
            }
        }
        return STATUS_OK;
    };
    if ((status = await(boot)) != STATUS_OK) sys::exit(status);
    if ((status = mgr_role(directory, "boot", "boot/files")) != STATUS_OK) sys::exit(status);
    if ((status = await(devices)) != STATUS_OK) sys::exit(status);
    if (store) {
        if ((status = await(*store)) != STATUS_OK) sys::exit(status);
        if ((status = mgr_role(directory, "store", "store/files")) != STATUS_OK) sys::exit(status);
    }
    if ((status = await(vfs)) != STATUS_OK) sys::exit(status);
    const auto configure = [&]() -> status_t {
        ubus_conn admin; auto status = admin.connect(directory, "vfs/admin");
        if (status != STATUS_OK) sys::exit(status);
        const auto mount = [&](std::string_view path, std::string_view service, bool read_only) {
            ubus_msg request, response;
            request.data.put("path", path).put("service", service).put("read_only", read_only);
            return admin.invoke("Mount", std::move(request), response);
        };
        if (store && (status = mount("/", "store", false)) != STATUS_OK) sys::exit(status);
        return mount("/boot", "boot", true);
    };
    if ((status = configure()) != STATUS_OK) sys::exit(status);
    const auto roles = [&]() -> status_t {
        for (const auto& [alias, source] : std::array<std::pair<std::string_view, std::string_view>, 9>{{
            {"public/files", "vfs/files"}, {"public/exec", "vfs/exec"}, {"public/read", "vfs/read"},
            {"public/system", "system.control"}, {"public/devices", "devices/control"},
            {"readonly/files", "vfs/read"}, {"readonly/exec", "vfs/exec"},
            {"readonly/system", "system.view"}, {"readonly/devices", "devices/view"}
        }}) if (const auto status = mgr_role(directory, alias, source); status != STATUS_OK) sys::exit(status);
        return store ? mgr_role(directory, "public/store.admin", "store/admin") : STATUS_OK;
    };
    if ((status = roles()) != STATUS_OK) sys::exit(status);
    if ((status = await(session)) != STATUS_OK) sys::exit(status);
    if ((status = mgr_role(directory, "public/session", "session/control")) != STATUS_OK) sys::exit(status);
    if ((status = mgr_role(directory, "readonly/session", "session/view")) != STATUS_OK) sys::exit(status);
    // A registry or supervisor loss rebuilds the owned subtree through init.
    // Restarting an individual provider preserves clients until their actual
    // endpoint closes; only subsequent lookups select its new publication.
    uint64_t boot_generation = boot.gen, vfs_generation = vfs.gen,
        device_generation = devices.gen, store_generation = store ? store->gen : 0;
    ubus_conn changes;
    if ((status = changes.bind(admin.selector())) != STATUS_OK) sys::exit(status);
    uint64_t seen{};
    for (;;) {
        if (changes.active()) {
            ubus_msg event; const auto status = changes.poll(event);
            if (status == STATUS_OK) {
                if (event.status != STATUS_OK) sys::exit(event.status);
                seen = event.data["epoch"].as<uint64_t>().value_or(0);
            } else if (status != STATUS_WOULD_BLOCK) sys::exit(status);
        }
        if (!changes.active()) {
            ubus_msg request; request.data.put("epoch", seen);
            if ((status = changes.begin("Changed", std::move(request))) != STATUS_OK) sys::exit(status);
        }
        if ((status = owner.poll()) != STATUS_OK) sys::exit(status);
        if (!registry.task || !session.task || !devices.task) sys::exit(STATUS_PEER_CLOSED);
        for (const auto& child : owner.children()) {
            if (child->task || !child->desired || child.get() == &registry) continue;
            if (child.get() == store) {
                ubus_msg request, response; request.data.put("name", "devices/block");
                const auto available = directory.invoke("Resolve", std::move(request), response);
                if (available == STATUS_NOT_FOUND) continue;
                if (available != STATUS_OK) sys::exit(available);
            }
            if ((status = owner.start(*child)) != STATUS_OK) sys::exit(status);
        }
        if (boot.state == "running" && boot.gen != boot_generation) {
            if ((status = mgr_role(directory, "boot", "boot/files")) != STATUS_OK) sys::exit(status);
            boot_generation = boot.gen;
        }
        if (devices.gen != device_generation) sys::exit(STATUS_PEER_CLOSED);
        if (store && store->state == "running" && store->gen != store_generation) {
            if ((status = mgr_role(directory, "store", "store/files")) != STATUS_OK) sys::exit(status);
            if ((status = mgr_role(directory, "public/store.admin", "store/admin")) != STATUS_OK) sys::exit(status);
            store_generation = store->gen;
        }
        if (vfs.state == "running" && vfs.gen != vfs_generation) {
            if ((status = configure()) != STATUS_OK || (status = roles()) != STATUS_OK) sys::exit(status);
            vfs_generation = vfs.gen;
        }
        if ((status = ev_wait(env.events, owner.deadline()).status) != STATUS_OK && status != STATUS_TIMED_OUT) sys::exit(status);
    }
}
