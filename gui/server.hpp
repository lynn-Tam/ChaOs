#pragma once
#include <ubus/ubus.hpp>

auto gui_serve(ubus_conn& parent, ubus_msg setup) -> status_t;
