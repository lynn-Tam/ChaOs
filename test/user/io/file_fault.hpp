#pragma once
#include <user/lib/imports.hpp>
namespace file_fault_test {
inline constexpr myos::bootstrap::Import Ready{"test.ready", 0x54455354, MYOS_OBJECT_KIND_NOTIFICATION};
inline constexpr myos::bootstrap::Import Go{"test.go", 0x54455354, MYOS_OBJECT_KIND_NOTIFICATION};
}
