#include <mgr/mgr.hpp>
#include <thread.hpp>

int main(int argc, char**) {
    const auto& env = _startup();
    ubus_conn parent;
    auto status = parent.bind(env.bootstrap); if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = parent.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    const auto message = setup.data["message"];
    if (argc == 2) {
        if (const auto proof = setup.capability(message["proof"])) {
            const std::array<word_t, 1> words{42};
            status = _ch_send(proof, words).status;
            if (status == STATUS_OK) status = _notification_signal(setup.capability(message["ready"])).status;
            if (status != STATUS_OK) return status;
        }
        for (;;) (void)_ev_wait(&_events());
    }
    ubus_conn files;
    ubus_msg query, response; query.data.put("name", "exec");
    status = parent.invoke("Get", std::move(query), response);
    if (status == STATUS_OK) status = files.bind(response.capability(response.data["connector"]));
    if (status != STATUS_OK) return status;
    query = {}; query.data.put("path", "/boot/supervisor.elf").put("perms", word_t{VM_READ | VM_EXECUTE});
    status = files.invoke("Map", std::move(query), response); if (status != STATUS_OK) return status;
    const auto buffer = response.data["buffer"].as<ubus_buffer>(); if (!buffer) return STATUS_PEER_FAULT;
    auto memory = _vm_area::map(_vm(), response.take(response.data["buffer"]), 0,
        (buffer->offset + buffer->size + 4095) & ~size_t{4095}, VM_READ);
    if (!memory) return memory.error();
    mgr owner{[&](mgr_child&) -> std::expected<_proc_image, status_t> {
        return _proc_image{.bytes={reinterpret_cast<const std::byte*>(memory->address + buffer->offset), size_t(buffer->size)},
            .memory=memory->memory.selector(), .offset=buffer->offset, .perms=VM_READ | VM_EXECUTE};
    }};
    status = owner.open(); if (status != STATUS_OK) return status;
    _ev_notice ready;
    status = ready.open(env.alloc(), &_events()); if (status != STATUS_OK) return status;
    auto recipe = mgr_recipe("descendant", "self", {.bytes=1024 * 1024, .caps=96, .slots=96, .pages=16});
    ubus_data args{ubus_type::Array}; args.push("hold"); recipe.data.put("argv", args);
    ubus_data payload;
    const auto proof_idx = recipe.give(setup.take(message["proof"]));
    if (!proof_idx) return proof_idx.error();
    const auto ready_idx = recipe.attach(ready.selector(), RIGHT_SIGNAL);
    if (!ready_idx) return ready_idx.error();
    payload.cap("proof", *proof_idx)
        .cap("ready", *ready_idx);
    recipe.data.put("setup", payload);
    const auto child = owner.configure(std::move(recipe)); if (!child) return child.error();
    status = owner.start(**child); if (status != STATUS_OK) return status;
    const auto crash = setup.capability(message["crash"]);
    _ev_watch crashed;
    status = crashed.open(&_events(), crash, SOURCE_READY); if (status != STATUS_OK) return status;
    bool running{};
    for (;;) {
        status = owner.poll(); if (status != STATUS_OK) return status;
        if (!running) { running = ready.take().status == STATUS_OK; if (!running) (void)ready.arm(); }
        if (running && _notification_take(crash).status == STATUS_OK) break;
        status = _ev_wait(&_events(), owner.deadline()).status;
        if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    }
    // Deliberately skip destructors with a live descendant: native ownership
    // must retire the subtree even when its userspace manager disappears.
    _thread_exit(STATUS_INTERNAL);
}
