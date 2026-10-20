#include <lib/mgr/mgr.hpp>
#include <lib/fs/fs.hpp>
#include <lib/stream/stream.hpp>

int main() {
    const auto& env = rt_startup();
    const auto lookup = env.selector(ubus_directory);
    proc_archive archive;
    const auto* bundle = env.find(fs_archive);
    if (!bundle) sys::exit(STATUS_INVALID_CAP);
    auto status = archive.open(bundle->cap, bundle->bytes);
    if (status != STATUS_OK) sys::exit(status);
    ubus_conn bus;
    if ((status = bus.bind(lookup)) != STATUS_OK) sys::exit(status);
    vm_area mapped;
    mgr owner{[&](std::string_view program) -> std::expected<proc_image, status_t> {
        if (const auto image = archive.image(program)) return image;
        ubus_conn files;
        auto status = files.connect(bus, "exec", "myos.files", 1, 1);
        if (status != STATUS_OK) return std::unexpected(status);
        auto image = fs_map(files, program, VM_READ | VM_EXECUTE);
        if (!image) return std::unexpected(image.error());
        auto mapping = vm_area::map(rt_vm(), std::move(image->memory), 0,
            (image->size + 4095) & ~size_t{4095}, VM_READ);
        if (!mapping) return std::unexpected(mapping.error());
        mapped = std::move(*mapping);
        return proc_image{.bytes={reinterpret_cast<const std::byte*>(mapped.address), image->size},
            .memory=mapped.memory.selector(), .offset=image->offset, .perms=VM_READ | VM_EXECUTE};
    }};
    if ((status = owner.open()) != STATUS_OK) sys::exit(status);
    if ((status = owner.directory(env.selector(mgr_children), lookup)) != STATUS_OK) sys::exit(status);
    auto& shell = owner.add("shell", "shell", {.bytes=32 * 1024 * 1024, .caps=1024,
        .kinds=proc_base_kinds | OBJ_BIT(ObjChannel), .slots=512, .pages=68,
        .pool_perms=RIGHT_CREATE | RIGHT_SPLIT});
    shell.await_ready = false;
    for (const auto& [key, rights] : std::array<std::pair<rt_key, word_t>, 3>{{
        {rt_stdin, RIGHT_RECEIVE | RIGHT_DUPLICATE}, {rt_stdout, RIGHT_SEND | RIGHT_DUPLICATE},
        {mgr_read_bus, RIGHT_SEND | RIGHT_DUPLICATE}
    }}) shell.grants.push_back({.key=key, .cap=env.selector(key), .limit=rights});
    if ((status = owner.start(shell)) != STATUS_OK) sys::exit(status);
    ubus_conn publisher;
    if ((status = publisher.bind(env.selector(ubus_publisher))) != STATUS_OK) sys::exit(status);
    for (const auto& [name, rights] : std::array<std::pair<std::string_view, word_t>, 2>{{
        {"control", mgr_observe | mgr_control}, {"view", mgr_observe}}})
        if (const auto published = owner.object().publish(publisher, name, rights); !published) sys::exit(published.error());
    if ((status = sys::notification_signal(env.selector(ubus_ready)).status) != STATUS_OK) sys::exit(status);
    for (;;) {
        if ((status = owner.poll()) != STATUS_OK) sys::exit(status);
        for (const auto& child : owner.children()) if (!child->task && child->desired)
            if ((status = owner.start(*child)) != STATUS_OK) { child->desired = false; child->state = "failed"; child->result = status; }
        if ((status = ev_wait(env.events, owner.deadline()).status) != STATUS_OK && status != STATUS_TIMED_OUT) sys::exit(status);
    }
}
