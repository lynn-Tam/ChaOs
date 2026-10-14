#pragma once

#include <servers/fw.hpp>

#include <exec.hpp>
#include <libk/cpio.hpp>
#include <libk/elf.hpp>
#include <libk/checked_arithmetic.hpp>
#include <ipc.hpp>
#include <deque>
#include <memory>

struct test_image {
    vm_area mapping;
    std::span<const std::byte> bytes;
    cap_t memory{};
    word_t perms{};
    auto open(cap_t vm, cap_t mem, size_t size, word_t access = VM_READ) -> status_t {
        memory = mem;
        perms = access;
        auto status = mapping.open(vm, 0, libk::checked_align_up(size, 4096).value_or(0), VM_READ);
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
    pr_auth auth_;
    std::deque<std::unique_ptr<pr_task>> tasks_;
    auto get(test_id id) -> pr_task* {
        return id.value && id.value <= tasks_.size() ? tasks_[id.value - 1].get() : nullptr;
    }

  public:
    void open(cap_t pool, cap_t domain, cap_t vm, uint32_t cpus) {
        auth_ = {pool, domain, vm, cpus};
    }
    auto spawn(test_image& image, std::string_view name, const pr_opts& opts = {}) -> test_spawn {
        const auto mod = name.empty() ? image.entry() : image.find(name);
        if (!mod || !bool(*mod))
            return {{}, STATUS_BAD_ARGS};
        auto& task = tasks_.emplace_back(std::make_unique<pr_task>());
        auto config = opts;
        const std::string_view arg = name.empty() ? "test" : name;
        if (config.argv.empty())
            config.argv = {&arg, 1};
        const auto status = pr_exec(
            *task, auth_,
            pr_image{.bytes = mod->bytes(),
                     .memory = image.memory,
                     .offset = static_cast<uint64_t>(mod->bytes().data() - image.bytes.data()),
                     .perms = image.perms},
            config, tasks_.size() - 1);
        return {{tasks_.size()}, status};
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
    const auto cap = binding ? binding->cap : fw_root(info).bundle;
    const auto size = binding ? binding->bytes : fw_root(info).bundle_bytes;
    if (const auto rc = image.open(info.vspace, cap, size);
        rc != STATUS_OK)
        sys::exit(rc);
}
inline auto input(rt_req contract, cap_t cap, word_t rights, word_t badge = 0) -> pr_cap {
    return {.contract = contract, .cap = cap, .limit = rights, .badge = badge};
}
auto start(test_tasks& tasks, test_image& image, std::string_view name, pr_opts opts = {})
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
        if (!pr_retryable(result.status) && result.status != STATUS_WOULD_BLOCK)
            return result.status;
        sys::yield();
    }
}
auto stop(test_tasks& tasks, test_id id) -> status_t {
    const auto s = tasks.request_stop(id);
    return s == STATUS_OK ? wait(tasks, id) : s;
}
inline constexpr pr_quota files_quota{.bytes = 16 * 1024 * 1024,
                                      .caps = 1024,
                                      .kinds =
                                          pr_base_kinds | OBJ_BIT(ObjPager) | OBJ_BIT(ObjChannel),
                                      .slots = 1024,
                                      .pages = 132};
