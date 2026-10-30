#pragma once
#include <cstddef>
#include <cstdint>

// Public file/block protocol bounds. Stat negotiates each session's actual
// limit/depth; payloads use BufferSlice rather than the control message body.
inline constexpr size_t io_limit = 64 * 1024, io_depth = 32;
enum : uint64_t { io_lookup = 1, io_modify = 2, io_execute = 4, io_admin = 8 };
enum : uint64_t { io_read = 1, io_write = 2, io_create = 4, io_truncate = 8, io_exclusive = 16, io_append = 32 };
enum : int32_t { io_exists = -256, io_not_dir, io_is_dir, io_not_empty, io_name_long, io_too_large, io_no_space, io_cross_device };

#include <ubus/stream.hpp>
