#pragma once
#include <vm.hpp>
#include <ubus/ubus.hpp>


#include <proc.hpp>
#include <libk/cpio.hpp>
#include <libk/elf.hpp>
#include <libk/checked_arithmetic.hpp>
#include <ipc.hpp>
#include <deque>
#include <memory>
#include <string>


struct test_image {
    _vm_area mapping;
    std::span<const std::byte> bytes;
    cap_t memory{};
    word_t perms{};
    auto open(cap_t vm, cap_t mem, size_t size, word_t access = VM_READ) -> status_t {
        memory = mem;
        perms = access;
        if (vm != _vm().space) return STATUS_BAD_ARGS;
        auto status = mapping.open(_vm(), 0, libk::checked_align_up(size, 4096).value_or(0), VM_READ);
        if (status == STATUS_OK)
            status = mapping.attach(mem, 0, mapping.size, VM_READ);
        if (status == STATUS_OK)
            bytes = {reinterpret_cast<const std::byte*>(mapping.address), size};
        return status;
    }
    auto find(std::string_view name) const -> std::optional<libk::elf_view> {
        const auto f = libk::cpio_view::parse(bytes).find(name);
        auto elf = f ? libk::elf_view::parse(f->data) : libk::elf_view{};
        return elf ? std::optional{elf} : std::nullopt;
    }
    auto entry() const -> std::optional<libk::elf_view> {
        auto elf = libk::elf_view::parse(bytes);
        return elf ? std::optional{elf} : find("init");
    }
};
struct test_id {
    size_t value{};
    auto valid() const -> bool { return value != 0; }
    auto operator==(const test_id&) const -> bool = default;
};
struct test_spawn {
    test_id id;
    status_t status;
};
class test_tasks {
    _proc_auth auth_;
    std::deque<std::unique_ptr<_proc>> tasks_;
    auto get(test_id id) -> _proc* {
        return id.value && id.value <= tasks_.size() ? tasks_[id.value - 1].get() : nullptr;
    }

  public:
    void open(CreateAuth pool, cap_t domain, [[maybe_unused]] uint32_t cpus) {
        auth_ = {pool, domain ? domain : _startup().domain};
    }
    auto spawn(test_image& image, std::string_view name, const _proc_opts& opts = {}) -> test_spawn {
        const auto mod = name.empty() ? image.entry() : image.find(name);
        if (!mod || !bool(*mod))
            return {{}, STATUS_BAD_ARGS};
        auto& task = tasks_.emplace_back(std::make_unique<_proc>());
        auto config = opts;
        const std::string_view arg = name.empty() ? "test" : name;
        if (config.argv.empty())
            config.argv = {&arg, 1};
        auto made = _proc_spawn(
            auth_,
            _proc_image{.bytes = mod->bytes(),
                     .memory = image.memory,
                     .offset = static_cast<uint64_t>(mod->bytes().data() - image.bytes.data()),
                     .perms = image.perms},
            config);
        if (!made) { tasks_.pop_back(); return {{}, made.error()}; }
        *task = std::move(*made);
        return {{tasks_.size()}, STATUS_OK};
    }
    auto observe(test_id id) -> _sys_result {
        auto* t = get(id);
        if (!t) return {.status=STATUS_INVALID_CAP};
        const auto progress = t->poll();
        if (progress != STATUS_OK) return {.status=progress};
        const auto result = t->result();
        return {.status=result ? STATUS_OK : STATUS_WOULD_BLOCK, .value=result ? word_t(result->code) : 0};
    }
    auto request_stop(test_id id) -> status_t {
        auto* t = get(id);
        return t ? t->retire() : STATUS_INVALID_CAP;
    }
    auto poll(test_id id) -> status_t { return observe(id).status; }
    auto collect(test_id id) -> _sys_result {
        auto* t = get(id);
        if (!t) return {.status=STATUS_INVALID_CAP};
        const auto progress = t->poll();
        if (progress != STATUS_OK) return {.status=progress};
        const auto result = t->result();
        if (!result) return {.status=STATUS_WOULD_BLOCK};
        const auto status = t->retire();
        if (status != STATUS_OK) return {.status=status};
        if (!t->retired()) return {.status=STATUS_WOULD_BLOCK};
        tasks_[id.value - 1].reset();
        return {.status=STATUS_OK, .value=word_t(result->code)};
    }

};

inline void open(test_image& image, const _env& info) {
    const auto cap = _root().bundle;
    const auto size = _root().bundle_bytes;
    if (const auto rc = image.open(info.vspace, cap, size);
        rc != STATUS_OK)
        _thread_exit(rc);
}
auto start(test_tasks& tasks, test_image& image, std::string_view name, _proc_opts opts = {})
    -> test_id {
    const auto result = tasks.spawn(image, name, opts);
    if (result.status != STATUS_OK)
        _thread_exit(result.status);
    return result.id;
}
auto wait(test_tasks& tasks, test_id id) -> status_t {
    for (;;) {
        const auto result = tasks.collect(id);
        if (result.status == STATUS_OK)
            return static_cast<status_t>(result.value);
        if (!(result.status == STATUS_BUSY || result.status == STATUS_RETRY) && result.status != STATUS_WOULD_BLOCK)
            return result.status;
        _yield();
    }
}
auto stop(test_tasks& tasks, test_id id) -> status_t {
    const auto s = tasks.request_stop(id);
    return s == STATUS_OK ? wait(tasks, id) : s;
}
