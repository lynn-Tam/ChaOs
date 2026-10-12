#pragma once

#include <proto.hpp>
#include <servers/fw.hpp>

#include <proc.hpp>
#include <ch.hpp>

inline void open(pr_pkg& pkg, const rt_env& info) {
    const auto* image = info.find(rt_image);
    const auto cap = image ? image->cap : fw_root(info).bundle;
    const auto size = image ? image->bytes : fw_root(info).bundle_bytes;
    if (const auto rc = pkg.open(info.vspace, cap,
            {0, pr_window::round_size(size)}, size); rc != STATUS_OK) sys::exit(rc);
}
inline auto input(rt_req contract, cap_t cap, word_t rights, word_t badge = 0) -> pr_cap {
    return {.contract = contract, .cap = cap, .limit = rights, .badge = badge};
}
inline auto host(cap_t cap, word_t rid) -> pr_cap {
    return {.contract = io_host,
            .cap = cap,
            .limit = CapView{.version = CAP_ATTENUATION_VERSION_CURRENT,
                             .kind = ObjHost,
                             .size = CAP_ATTENUATION_SIZE,
                             .rights = RIGHT_CONNECT,
                             .words = {rid, 1}}};
}
auto start(pr_tasks& tasks, pr_pkg& pkg, std::string_view name, pr_opts opts = {}) -> pr_id {
    const auto result = tasks.spawn(pkg, name, opts);
    if (result.status != STATUS_OK) sys::exit(result.status);
    return result.id;
}
auto wait(pr_tasks& tasks, pr_id id) -> status_t {
    for (;;) {
        const auto result = tasks.collect(id);
        if (result.status == STATUS_OK)
            return static_cast<status_t>(result.value);
        if (!pr_retryable(result.status) && result.status != STATUS_WOULD_BLOCK)
            return result.status;
        sys::yield();
    }
}
auto stop(pr_tasks& tasks, pr_id id) -> status_t {
    const auto s = tasks.request_stop(id);
    return s == STATUS_OK ? wait(tasks, id) : s;
}
inline constexpr pr_quota io_quota{.bytes = 8 * 1024 * 1024,
                                   .caps = 256,
                                   .kinds =
                                       pr_base_kinds | OBJ_BIT(ObjIoSpace) | OBJ_BIT(ObjChannel)};
inline constexpr pr_quota files_quota{.bytes = 16 * 1024 * 1024,
                                      .caps = 1024,
                                      .kinds =
                                          pr_base_kinds | OBJ_BIT(ObjPager) | OBJ_BIT(ObjChannel),
                                      .slots = 1024,
                                      .pages = 132};
