#pragma once

#include <proto.hpp>
#include <start.hpp>

inline constexpr rt_req ft_ready{"test.ready", 0x54455354, ObjNotification};
inline constexpr rt_req ft_go{"test.go", 0x54455354, ObjNotification};
