#include <test/test.hpp>

#include <diag/console.hpp>

namespace {

constinit TestRegistry builtin_registry{};

void report_failure(const char* group, const char* name) noexcept {
    kernel::diag::console::print<"[FAIL] {}: {}\n">(group, name);
}

} // namespace

bool TestRegistry::add(const char* group, const char* name, TestFn fn) noexcept {
    if (count_ >= kMaxTests) {
        return false;
    }
    entries_[count_++] = Entry{group, name, fn};
    return true;
}

TestStats TestRegistry::run(const TestContext& ctx) noexcept {
    TestStats stats{};

    for (size_t i = 0; i < count_; ++i) {
        const Entry& entry = entries_[i];
        const bool ok = entry.fn(ctx);
        if (ok) {
            ++stats.passed;
        } else {
            ++stats.failed;
            report_failure(entry.group, entry.name);
        }
    }

    kernel::diag::console::print<
        "\n[test] summary\npassed={}\nfailed={}\n">(
        stats.passed, stats.failed);
    return stats;
}

void register_allocator_tests(TestRegistry& registry) noexcept;
void register_bootinfo_tests(TestRegistry& registry) noexcept;
void register_boot_bundle_tests(TestRegistry& registry) noexcept;
void register_cpu_topology_tests(TestRegistry& registry) noexcept;
void register_libk_tests(TestRegistry& registry) noexcept;
void register_sync_tests(TestRegistry& registry) noexcept;
void register_sched_tests(TestRegistry& registry) noexcept;
void register_cap_tests(TestRegistry& registry) noexcept;
void register_memory_tests(TestRegistry& registry) noexcept;
void register_translation_tests(TestRegistry& registry) noexcept;
void register_vspace_tests(TestRegistry& registry) noexcept;
void register_user_tests(TestRegistry& registry) noexcept;
void register_ipc_tests(TestRegistry& registry) noexcept;

void register_builtin_tests(TestRegistry& registry) noexcept {
    register_libk_tests(registry);
    register_sync_tests(registry);
    register_allocator_tests(registry);
    register_bootinfo_tests(registry);
    register_boot_bundle_tests(registry);
    register_cpu_topology_tests(registry);
    register_sched_tests(registry);
    register_cap_tests(registry);
    register_memory_tests(registry);
    register_translation_tests(registry);
    register_vspace_tests(registry);
    register_user_tests(registry);
    register_ipc_tests(registry);
}

auto run_builtin_tests(const kernel::boot::BootInfo& boot) noexcept
    -> TestStats {
    register_builtin_tests(builtin_registry);
    return builtin_registry.run(TestContext{boot});
}
