#pragma once

#include <sys/protocol.hpp>

#include <sys/task.hpp>
#include <sys/channel.hpp>

inline void open(proc::package& pkg, const boot::BootView& info) {
    sys::require(pkg.open(info.selector(BOOT_VSPACE), info.selector(BOOT_BUNDLE),
        {0x10000000, proc::window::round_size(info.bundle_size())}, info.bundle_size()));
}
inline auto input(boot::Import contract, cap_t cap, word_t rights, word_t badge = 0) -> proc::cap_src {
    return {.contract = contract, .cap = cap, .limit = rights, .badge = badge};
}
inline auto host(cap_t cap, word_t rid) -> proc::cap_src {
    return {.role = BOOT_IO_HOST, .cap = cap, .limit = CapView{
        .version = CAP_ATTENUATION_VERSION_CURRENT, .kind = ObjHost,
        .size = CAP_ATTENUATION_SIZE, .rights = RIGHT_CONNECT,
        .words = {rid, 1}}};
}
template<size_t N>
auto start(proc::tasks<N>& tasks, proc::package& pkg, std::string_view name,
           proc::spawn_opts opts = {}) -> proc::task_id {
    const auto result = tasks.spawn(pkg, name, opts);
    sys::require(result.status);
    return result.id;
}
template<size_t N>
auto wait(proc::tasks<N>& tasks, proc::task_id id) -> status_t {
    for (;;) {
        const auto result = tasks.collect(id);
        if (result.status == STATUS_OK) return static_cast<status_t>(result.value);
        if (!proc::retryable(result.status) && result.status != STATUS_WOULD_BLOCK) return result.status;
        sys::yield();
    }
}
template<size_t N>
auto stop(proc::tasks<N>& tasks, proc::task_id id) -> status_t {
    const auto s = tasks.request_stop(id);
    return s == STATUS_OK ? wait(tasks, id) : s;
}
inline constexpr proc::quota io_quota{.bytes = 8 * 1024 * 1024, .caps = 256,
    .kinds = proc::base_kinds | OBJ_BIT(ObjIoSpace) | OBJ_BIT(ObjChannel)};
inline constexpr proc::quota files_quota{.bytes = 16 * 1024 * 1024, .caps = 1024,
    .kinds = proc::base_kinds | OBJ_BIT(ObjPager) | OBJ_BIT(ObjChannel), .slots = 1024, .pages = 132};
