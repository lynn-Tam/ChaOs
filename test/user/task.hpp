#pragma once
#include <vm.hpp>


#include <proc.hpp>
#include <libk/cpio.hpp>
#include <libk/elf.hpp>
#include <libk/checked_arithmetic.hpp>
#include <ipc.hpp>
#include <deque>
#include <memory>
#include <string>

inline constexpr rt_key rt_image{"image", ObjMem};

struct test_image {
    vm_area mapping;
    std::span<const std::byte> bytes;
    cap_t memory{};
    word_t perms{};
    auto open(cap_t vm, cap_t mem, size_t size, word_t access = VM_READ) -> status_t {
        memory = mem;
        perms = access;
        if (vm != rt_vm().space) return STATUS_BAD_ARGS;
        auto status = mapping.open(rt_vm(), 0, libk::checked_align_up(size, 4096).value_or(0), VM_READ);
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
    proc_auth auth_;
    std::deque<std::unique_ptr<proc_task>> tasks_;
    auto get(test_id id) -> proc_task* {
        return id.value && id.value <= tasks_.size() ? tasks_[id.value - 1].get() : nullptr;
    }

  public:
    void open(CreateAuth pool, cap_t domain, uint32_t cpus) {
        auth_ = {pool, cpus, domain ? domain : rt_startup().domain};
    }
    auto spawn(test_image& image, std::string_view name, const proc_opts& opts = {}) -> test_spawn {
        const auto mod = name.empty() ? image.entry() : image.find(name);
        if (!mod || !bool(*mod))
            return {{}, STATUS_BAD_ARGS};
        auto& task = tasks_.emplace_back(std::make_unique<proc_task>());
        auto config = opts;
        const std::string_view arg = name.empty() ? "test" : name;
        if (config.argv.empty())
            config.argv = {&arg, 1};
        config.cpu = (tasks_.size() - 1) % auth_.cpus;
        auto status = proc_spawn(
            *task, auth_,
            proc_image{.bytes = mod->bytes(),
                     .memory = image.memory,
                     .offset = static_cast<uint64_t>(mod->bytes().data() - image.bytes.data()),
                     .perms = image.perms},
            config);
        if (status == STATUS_OK) status = task->start();
        if (status != STATUS_OK) static_cast<void>(task->stop(status));
        return {{tasks_.size()}, status};
    }
    auto info(test_id id) -> std::expected<ThreadInfo, status_t> {
        auto* task = get(id);
        return task ? task->info() : std::unexpected(STATUS_INVALID_CAP);
    }
    auto observe(test_id id) -> sys::SysResult {
        auto* t = get(id);
        return t ? t->observe() : sys::SysResult{.status = STATUS_INVALID_CAP};
    }
    auto request_stop(test_id id) -> status_t {
        auto* t = get(id);
        return t ? t->stop() : STATUS_INVALID_CAP;
    }
    auto poll(test_id id) -> status_t {
        auto* t = get(id);
        return t ? t->poll() : STATUS_INVALID_CAP;
    }
    auto collect(test_id id) -> sys::SysResult {
        auto* t = get(id);
        if (!t)
            return {.status = STATUS_INVALID_CAP};
        auto result = t->collect();
        if (result.status == STATUS_OK)
            tasks_[id.value - 1].reset();
        return result;
    }
};

inline void open(test_image& image, const rt_env& info) {
    const auto* binding = info.find(rt_image);
    const auto cap = binding ? binding->cap : rt_root().bundle;
    const auto size = binding ? binding->bytes : rt_root().bundle_bytes;
    if (const auto rc = image.open(info.vspace, cap, size);
        rc != STATUS_OK)
        sys::exit(rc);
}
inline auto input(rt_key contract, cap_t cap, word_t rights, word_t badge = 0) -> proc_cap {
    return {.key = contract, .cap = cap, .limit = rights, .badge = badge};
}
auto start(test_tasks& tasks, test_image& image, std::string_view name, proc_opts opts = {})
    -> test_id {
    const auto result = tasks.spawn(image, name, opts);
    if (result.status != STATUS_OK)
        sys::exit(result.status);
    return result.id;
}
auto wait(test_tasks& tasks, test_id id) -> status_t {
    for (;;) {
        const auto result = tasks.collect(id);
        if (result.status == STATUS_OK)
            return static_cast<status_t>(result.value);
        if (!proc_retryable(result.status) && result.status != STATUS_WOULD_BLOCK)
            return result.status;
        sys::yield();
    }
}
auto stop(test_tasks& tasks, test_id id) -> status_t {
    const auto s = tasks.request_stop(id);
    return s == STATUS_OK ? wait(tasks, id) : s;
}
