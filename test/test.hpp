#pragma once

#include <stddef.h>

#include <boot/info.hpp>

namespace mm { class Pmm; }

struct TestContext {
    const BootInfo& boot;
    const mm::Pmm& memory;
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

    static constexpr size_t kMaxTests = 256;

    bool add(const char* group, const char* name, TestFn fn) noexcept;
    TestStats run(const TestContext& ctx) noexcept;

private:
    Entry entries_[kMaxTests]{};
    size_t count_{};
    size_t dropped_{};
};

void register_builtin_tests(TestRegistry& registry) noexcept;
[[nodiscard]] auto run_builtin_tests(
    const BootInfo& boot, const mm::Pmm& memory) noexcept -> TestStats;
