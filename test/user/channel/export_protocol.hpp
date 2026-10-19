#pragma once


#include <start.hpp>


inline constexpr rt_req ct_provider{"provider.channel", 0x50524f56,
    ObjChannel};

inline constexpr rt_req ct_notice{"observe.source", 0, ObjNotification};

inline constexpr rt_req ct_perf{"payload.metrics", 0, ObjChannel};
