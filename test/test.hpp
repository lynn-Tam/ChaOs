#pragma once

#include <stddef.h>

#include <boot/boot_info.hpp>

struct TestContext {
    const kernel::boot::BootInfo& boot;
};

struct TestStats {
    size_t passed{};
    size_t failed{};
};

using TestFn = bool (*)(const TestContext&) noexcept;

class TestRegistry {
public:
    struct Entry {
        const char* group;
        const char* name;
        TestFn fn;
    };

    static constexpr size_t kMaxTests = 192;

    bool add(const char* group, const char* name, TestFn fn) noexcept;
    TestStats run(const TestContext& ctx) noexcept;

private:
    Entry entries_[kMaxTests]{};
    size_t count_{};
};

void register_builtin_tests(TestRegistry& registry) noexcept;
[[nodiscard]] auto run_builtin_tests(
    const kernel::boot::BootInfo& boot) noexcept -> TestStats;
