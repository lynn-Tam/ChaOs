#pragma once
#include <start.hpp>
#include <ipc.hpp>

inline constexpr rt_req sv_service{"service.control", 0x53564354, ObjChannel, 3};
inline constexpr rt_req sv_wake{"service.wake", 0x53564357, ObjNotification};
inline constexpr rt_req sv_ready{"ready", 0, ObjNotification};
enum class sv_op : uint64_t { Restart, List };
inline constexpr uint64_t sv_running = 1, sv_recovering = 2;
