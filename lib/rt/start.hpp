#pragma once

#include <uapi/start.h>
#include <span>
#include <string_view>
#include <cstddef>

extern "C" char** environ;

struct rt_key {
    const char* name;
    uint16_t kind;
};
struct rt_binding {
    cap_t cap;
    uint64_t bytes;
    uint16_t kind, reserved;
    uint32_t name, name_size;
};
struct rt_bindings { uint32_t version, size, count, reserved; };
inline constexpr uint32_t rt_bindings_version = 2;

// CRT validates once. This view only borrows immutable startup mappings.
struct rt_env {
    cap_t pool{}, scope{}, vspace{}, cspace{}, events{}, domain{};
    auto alloc() const noexcept -> CreateAuth { return {pool, scope}; }
    word_t cpus{}, ipc{};
    std::span<const std::byte> bindings{}, boot{};
    auto find(std::string_view key) const noexcept -> const rt_binding* {
        if (bindings.empty()) return nullptr;
        const auto* h = reinterpret_cast<const rt_bindings*>(bindings.data());
        const auto* entries = reinterpret_cast<const rt_binding*>(h + 1);
        for (const auto& entry : std::span{entries, h->count}) {
            const std::string_view name{reinterpret_cast<const char*>(bindings.data()) + entry.name,
                                        entry.name_size};
            if (name == key) return &entry;
        }
        return nullptr;
    }
    auto find(rt_key key) const noexcept -> const rt_binding* {
        const auto* value = find(std::string_view{key.name});
        return value && value->kind == key.kind ? value : nullptr;
    }
    auto selector(rt_key req) const noexcept -> cap_t {
        const auto* value = find(req);
        return value ? value->cap : 0;
    }
};
auto rt_startup() noexcept -> const rt_env&;

auto rt_root() noexcept -> const RootInfo&;
