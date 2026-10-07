#pragma once

#include <stdint.h>

namespace sys::process {
// Spawn carries a sequence of NUL-terminated arguments in data[0..size).
// Wait optionally carries an absolute clock deadline (uint64_t); a timeout or
// CancelWait removes the wait only. Stop terminates; the first terminal result wins.
// argv[0] selects the package. Foreground requests authorize terminal stdin
// for this launch; ordinary Spawn/Pipeline receive EOF or a pipe. A successful
// reply carries the task token in id.
enum class op : uint64_t { Spawn = 1, Wait, Stop, CancelWait, Pipeline,
    ForegroundSpawn, ForegroundPipeline };

}
