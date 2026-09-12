#include <stddef.h>
#include <user/lib/deploy_manifest.hpp>

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept { __builtin_trap(); }
}

auto validate_manifest(const void* bytes, size_t size) -> int {
    static myos::deploy::ManifestWorkspace workspace;
    const auto parsed = myos::deploy::ManifestView::parse(bytes, size, workspace);
    return parsed ? 0 : static_cast<int>(parsed.error());
}
