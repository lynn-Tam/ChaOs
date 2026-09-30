#include <stddef.h>
#include <servers/deploy/format.hpp>

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept { __builtin_trap(); }
}

auto validate_manifest(const void* bytes, size_t size) -> int {
    static deploy::ManifestWorkspace workspace;
    const auto parsed = deploy::ManifestView::parse(bytes, size, workspace);
    return parsed ? 0 : static_cast<int>(parsed.error());
}
