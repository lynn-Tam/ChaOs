#pragma once

// Negative results from libsys; successful I/O returns a byte count.
enum errc {
    invalid = -4,
    not_found = -5,
    denied = -6,
    busy = -7,
    no_memory = -8,
    io_error = -11,
    canceled = -18,
};
