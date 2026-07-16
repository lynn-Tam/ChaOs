#pragma once

#include <stddef.h>

namespace libk::fmt {

enum class errc {
    ok,
    bad_format,
    bad_spec,
    arg_missing,
    type_mismatch,
    output_truncated,
    unsupported,
};

struct result {
    size_t produced{};
    errc error{errc::ok};

    constexpr bool ok() const noexcept { return error == errc::ok; }
    constexpr explicit operator bool() const noexcept { return ok(); }
};

constexpr const char* error_name(errc error) noexcept {
    switch (error) {
    case errc::ok: return "ok";
    case errc::bad_format: return "bad_format";
    case errc::bad_spec: return "bad_spec";
    case errc::arg_missing: return "arg_missing";
    case errc::type_mismatch: return "type_mismatch";
    case errc::output_truncated: return "output_truncated";
    case errc::unsupported: return "unsupported";
    }
    return "unknown";
}

} // namespace libk::fmt
