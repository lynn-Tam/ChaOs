#pragma once

#include <uapi/bootstrap.h>

namespace myos::bootstrap {

// Consumer contracts live in userspace. A protocol is independent of the
// local binding name; major must match and the provider minor must suffice.
struct Import final {
    const char* name;
    uint32_t protocol;
    uint16_t kind;
    uint16_t major{1};
    uint16_t minor{};
};

namespace imports {
inline constexpr Import ConsoleOutput{"console.output", 0x434f4e53, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import ConsoleInput{"console.input", 0x434f4e53, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import Process{"process", 0x50524f43, MYOS_OBJECT_KIND_CHANNEL, 3};
inline constexpr Import Files{"files", 0x46494c45, MYOS_OBJECT_KIND_CHANNEL, 3};
inline constexpr Import FilesRead{"files.read", 0x46494c45, MYOS_OBJECT_KIND_CHANNEL, 3};
inline constexpr Import Block{"block", 0x424c4f43, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import Pager{"pager", 0x50414745, MYOS_OBJECT_KIND_PAGER};
inline constexpr Import TargetMemory{"target.memory", 0x4d454d4f, MYOS_OBJECT_KIND_MEMORY};
inline constexpr Import StagingMemory{"staging.memory", 0x4d454d4f, MYOS_OBJECT_KIND_MEMORY};
inline constexpr Import StagingRegion{"staging.region", 0x56535043, MYOS_OBJECT_KIND_VSPACE};
} // namespace imports
} // namespace myos::bootstrap
