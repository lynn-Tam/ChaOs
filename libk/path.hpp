#pragma once
#include <string>
#include <string_view>
#include <libk/mem.h>

namespace libk {
// Lexical, absolute path only; namespace permissions and link traversal belong
// to the filesystem. An empty result rejects embedded NUL or a relative base.
inline auto path_normal(std::string_view path, std::string_view base = "/") -> std::string {
    if (path.find('\0') != path.npos || base.find('\0') != base.npos ||
        (!path.starts_with('/') && !base.starts_with('/'))) return {};
    std::string result = path.starts_with('/') ? std::string{path} : std::string{base} + "/" + std::string{path};
    size_t size{}, cursor{};
    while (cursor < result.size()) {
        const auto begin = cursor; while (cursor < result.size() && result[cursor] != '/') ++cursor;
        const std::string_view part{result.data() + begin, cursor - begin};
        if (part == "..") { while (size && result[size - 1] != '/') --size; if (size) --size; }
        else if (!part.empty() && part != ".") {
            if (size) result[size++] = '/';
            ::memmove(result.data() + size, part.data(), part.size()); size += part.size();
        }
        ++cursor;
    }
    result.resize(size); result.insert(0, "/"); return result;
}
} // namespace libk
