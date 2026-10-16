#pragma once
#include <protocols/msg.hpp>
#include <string.h>

inline constexpr rt_req mg_tasks{"tasks.control", 0x5441534b, ObjChannel};
inline constexpr rt_req mg_tasks_view{"tasks.view", 0x5441534b, ObjChannel};
inline constexpr rt_req mg_devices_view{"devices.view", 0x44455653, ObjChannel};
inline constexpr rt_req mg_devices{"devices.control", 0x44455653, ObjChannel};
inline constexpr rt_req mg_jobs{"jobs.control", 0x4a4f4253, ObjChannel};
enum class mg_op : uint64_t { Tasks = 256, Task, Action, Caps, Dependencies, CloseCap, Devices, DeviceAction };
enum class mg_action : int64_t { Pause, Resume, Stop, Start, Restart };
enum mg_right : uint32_t {
    mg_observe = 1, mg_control = 2, mg_close = 4,
    mg_pause = 8, mg_resume = 16, mg_stop = 32, mg_start = 64, mg_restart = 128,
};
struct mg_target { uint64_t incarnation, argument; };
struct mg_task {
    uint64_t id, incarnation, owner, runtime, resident, charged, limit;
    uint32_t state, actions;
    char name[16];
};
struct mg_device {
    uint64_t id, incarnation, driver, mmio, bytes;
    uint32_t requester, irq, state, rid;
    uint64_t iospace;
    uint32_t actions;
    char name[12];
};
// Deployment edges known to the owner, distinct from capability lineage.
struct mg_dependency {
    uint64_t provider, incarnation;
    uint32_t protocol;
    uint16_t major, minor;
    uint32_t flags; // 1 restart, 2 reconnect, 4 pipeline; 0 origin unknown.
    char name[32];
};
static_assert(sizeof(mg_task) == sizeof(msg_frame::data));
static_assert(sizeof(mg_device) == sizeof(msg_frame::data));

inline auto mg_request(mg_op operation, uint64_t id, uint64_t incarnation = 0,
                       uint64_t argument = 0, mg_action action = {}) noexcept -> msg_frame {
    msg_frame message{.operation = static_cast<uint64_t>(operation), .status = static_cast<int64_t>(action),
        .value = id, .size = sizeof(mg_target)};
    const mg_target target{incarnation, argument};
    ::memcpy(message.data, &target, sizeof(target));
    return message;
}
enum class mg_state : uint32_t { Stopped, Draining, Starting, Running, Failed, Suspending, Suspended, Done };
template<class T> inline void mg_pack(msg_frame& message, const T& value) noexcept {
    static_assert(sizeof(T) <= sizeof(message.data));
    message.status = STATUS_OK; message.size = sizeof(T);
    ::memcpy(message.data, &value, sizeof(T));
}
