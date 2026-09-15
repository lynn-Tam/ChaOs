#pragma once

#include <stddef.h>
#include <uapi/bootstrap.h>

namespace myos::bootstrap {

inline auto valid_arguments(const myos_bootstrap_arguments& args) noexcept -> bool {
    if (args.count > MYOS_BOOTSTRAP_ARG_MAX || args.size > MYOS_BOOTSTRAP_ARG_BYTES) return false;
    size_t cursor{};
    for (size_t i = 0; i < args.count; ++i) {
        if (args.offsets[i] != cursor) return false;
        while (cursor < args.size && args.bytes[cursor] != 0) ++cursor;
        if (cursor == args.size) return false;
        ++cursor;
    }
    return cursor == args.size;
}

// Owned, bounded argument bytes. The wire form uses offsets, never pointers
// into the sender's address space. argv[0] names the selected application.
class Arguments final {
    myos_bootstrap_arguments args_{};
public:
    auto append(const char* text, size_t size) noexcept -> bool {
        if (text == nullptr || args_.count == MYOS_BOOTSTRAP_ARG_MAX
            || size >= MYOS_BOOTSTRAP_ARG_BYTES - args_.size) return false;
        for (size_t i = 0; i < size; ++i) if (text[i] == 0) return false;
        args_.offsets[args_.count++] = args_.size;
        for (size_t i = 0; i < size; ++i) args_.bytes[args_.size++] = text[i];
        args_.bytes[args_.size++] = 0;
        return true;
    }
    auto decode(const char* bytes, size_t size) noexcept -> bool {
        args_ = {};
        for (size_t first = 0; first < size;) {
            size_t end = first;
            while (end < size && bytes[end] != 0) ++end;
            if (end == size || !append(bytes + first, end - first)) { args_ = {}; return false; }
            first = end + 1;
        }
        return true;
    }
    auto data() const noexcept -> const myos_bootstrap_arguments& { return args_; }
    auto count() const noexcept -> size_t { return args_.count; }
    auto argument(size_t index) const noexcept -> const char* {
        return index < args_.count ? args_.bytes + args_.offsets[index] : nullptr;
    }
};
} // namespace myos::bootstrap
