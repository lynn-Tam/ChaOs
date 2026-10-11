#pragma once

#include <sys/protocol.hpp>
#include <sys/start.hpp>
namespace file_fault_test {
inline constexpr boot::Import Ready{"test.ready", 0x54455354, ObjNotification};
inline constexpr boot::Import Go{"test.go", 0x54455354, ObjNotification};
}
