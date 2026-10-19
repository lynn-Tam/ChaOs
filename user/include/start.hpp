#pragma once

#include <uapi/start.h>
#include <span>
#include <string_view>
#include <cstddef>

extern "C" char** environ;

struct rt_req {
    const char* name;
    uint32_t protocol;
    uint16_t kind, major{1}, minor{};
};
struct rt_binding {
    cap_t cap;
    uint64_t bytes;
    uint32_t protocol;
    uint16_t kind, major, minor, reserved;
    uint32_t name, name_size;
};
struct rt_bindings { uint32_t version, size, count, reserved; };
inline constexpr uint32_t rt_bindings_version = 1;

// CRT validates once. This view only borrows immutable startup mappings.
struct rt_env {
    cap_t pool{}, scope{}, vspace{}, cspace{}, events{}, domain{};
    auto alloc() const noexcept -> CreateAuth { return {pool, scope}; }
    word_t cpus{}, ipc{};
    std::span<const std::byte> bindings{}, boot{};
    auto find(rt_req req) const noexcept -> const rt_binding* {
        if (bindings.empty()) return nullptr;
        const auto* h = reinterpret_cast<const rt_bindings*>(bindings.data());
        const auto* entries = reinterpret_cast<const rt_binding*>(h + 1);
        for (const auto& entry : std::span{entries, h->count}) {
            const std::string_view name{reinterpret_cast<const char*>(bindings.data()) + entry.name,
                                        entry.name_size};
            if (name == req.name && entry.kind == req.kind && entry.protocol == req.protocol &&
                entry.major == req.major && entry.minor >= req.minor) return &entry;
        }
        return nullptr;
    }
    auto selector(rt_req req) const noexcept -> cap_t {
        const auto* value = find(req);
        return value ? value->cap : 0;
    }
};
auto rt_startup() noexcept -> const rt_env&;
