#include <mgr/mgr.hpp>
#include <util.hpp>

int main(int argc, char**) {
    const auto& env = rt_startup();
    ubus_conn parent;
    auto status = parent.bind(env.environment); if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = parent.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    const auto message = setup.data["message"];
    if (argc == 2) {
        if (const auto proof = setup.capability(message["proof"])) {
            const std::array<word_t, 1> words{42};
            status = ch_send(proof, words).status;
            if (status == STATUS_OK) status = sys::notification_signal(setup.capability(message["ready"])).status;
            if (status != STATUS_OK) return status;
        }
        for (;;) (void)ev_wait(rt_events());
    }
    ubus_conn files;
    ubus_msg query, response; query.data.put("name", "exec");
    status = parent.invoke("Get", std::move(query), response);
    if (status == STATUS_OK) status = files.bind(response.capability(response.data["connector"]));
    if (status != STATUS_OK) return status;
    query = {}; query.data.put("path", "/boot/supervisor.elf").put("perms", word_t{VM_READ | VM_EXECUTE});
    status = files.invoke("Map", std::move(query), response); if (status != STATUS_OK) return status;
    const auto buffer = response.data["buffer"].as<ubus_buffer>(); if (!buffer) return STATUS_PEER_FAULT;
    auto memory = vm_area::map(rt_vm(), response.take(response.data["buffer"]), 0,
        (buffer->offset + buffer->size + 4095) & ~size_t{4095}, VM_READ);
    if (!memory) return memory.error();
    mgr owner{[&](mgr_child&) -> std::expected<proc_image, status_t> {
        return proc_image{.bytes={reinterpret_cast<const std::byte*>(memory->address + buffer->offset), size_t(buffer->size)},
            .memory=memory->memory.selector(), .offset=buffer->offset, .perms=VM_READ | VM_EXECUTE};
    }};
    status = owner.open(); if (status != STATUS_OK) return status;
    ev_notice ready;
    status = ready.open(env.alloc(), rt_events()); if (status != STATUS_OK) return status;
    auto& child = owner.add("descendant", "self", {.bytes=1024 * 1024, .caps=96, .slots=96, .pages=16});
    child.args.push_back("hold");
    const auto proof_idx = child.setup.give(setup.take(message["proof"]));
    if (!proof_idx) return proof_idx.error();
    const auto ready_idx = child.setup.attach(ready.selector(), RIGHT_SIGNAL);
    if (!ready_idx) return ready_idx.error();
    child.setup.data.cap("proof", *proof_idx)
        .cap("ready", *ready_idx);
    status = owner.start(child); if (status != STATUS_OK) return status;
    const auto crash = setup.capability(message["crash"]);
    ev_watch crashed;
    status = crashed.open(rt_events(), crash, SOURCE_READY); if (status != STATUS_OK) return status;
    bool running{};
    for (;;) {
        status = owner.poll(); if (status != STATUS_OK) return status;
        if (!running) { running = ready.take().status == STATUS_OK; if (!running) (void)ready.arm(); }
        if (running && sys::notification_take(crash).status == STATUS_OK) break;
        status = ev_wait(rt_events(), owner.deadline()).status;
        if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    }
    // Deliberately skip destructors with a live descendant: native ownership
    // must retire the subtree even when its userspace manager disappears.
    sys::exit(STATUS_INTERNAL);
}
