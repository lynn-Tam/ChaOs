#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>

#include <utility>
#include <libk/assert.hpp>
#include <sys/handle.hpp>

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    __builtin_trap();
}
} // namespace libk

namespace {

struct FakeBackend final {
    struct Call final {
        sys::cap::CapRef reference;
    };

    static inline Call calls[32]{};
    static inline size_t call_count{};
    static inline status_t next_status{STATUS_OK};
    static inline size_t fault_count{};
    static inline jmp_buf* fault_target{};

    static void reset() noexcept {
        call_count = 0;
        next_status = STATUS_OK;
        fault_count = 0;
        fault_target = nullptr;
    }

    [[nodiscard]] static auto close(
        sys::cap::CapRef reference) noexcept -> status_t {
        if (call_count < sizeof(calls) / sizeof(calls[0])) {
            calls[call_count++] = Call{reference};
        }
        const status_t status = next_status;
        next_status = STATUS_OK;
        return status;
    }

    [[noreturn]] static void ownership_fault(
        status_t) noexcept {
        ++fault_count;
        if (fault_target != nullptr) {
            longjmp(*fault_target, 1);
        }
        for (;;) {}
    }
};

using Owner = sys::cap::BasicOwnedCap<FakeBackend>;
static_assert(sys::cap::CapBackend<FakeBackend>);

[[nodiscard]] auto call_is(
    size_t index,
    cap_t selector,
    cap_t cspace) noexcept -> bool {
    return index < FakeBackend::call_count
        && FakeBackend::calls[index].reference
            == sys::cap::CapRef{selector, cspace};
}

[[nodiscard]] auto test_move_and_release() noexcept -> bool {
    FakeBackend::reset();
    Owner source{{7, 9}};
    Owner moved{std::move(source)};
    if (source || !moved || moved.reference() != sys::cap::CapRef{7, 9}) {
        return false;
    }
    const auto released = moved.release();
    return !moved && released == sys::cap::CapRef{7, 9}
        && FakeBackend::call_count == 0;
}

[[nodiscard]] auto test_explicit_failure_retains_ownership() noexcept -> bool {
    FakeBackend::reset();
    Owner owner{{11, 0}};
    FakeBackend::next_status = STATUS_BUSY;
    if (owner.close() != STATUS_BUSY
        || !owner
        || owner.reference() != sys::cap::CapRef{11, 0}) {
        return false;
    }
    return owner.close() == STATUS_OK
        && !owner
        && FakeBackend::call_count == 2
        && call_is(0, 11, 0)
        && call_is(1, 11, 0);
}

[[nodiscard]] auto test_destructor_fault_is_bounded() noexcept -> bool {
    FakeBackend::reset();
    jmp_buf target{};
    FakeBackend::fault_target = &target;
    if (setjmp(target) == 0) {
        Owner owner{{13, 0}};
        FakeBackend::next_status = STATUS_BUSY;
        return false;
    }
    FakeBackend::fault_target = nullptr;
    return FakeBackend::fault_count == 1
        && FakeBackend::call_count == 1
        && call_is(0, 13, 0);
}

struct Test final {
    const char* name;
    bool (*run)() noexcept;
};

constexpr Test tests[] = {
    {"move/release", test_move_and_release},
    {"explicit close failure retains owner", test_explicit_failure_retains_ownership},
    {"destructor ownership fault", test_destructor_fault_is_bounded},

};

} // namespace

int main() {
    size_t failures = 0;
    for (const Test& test : tests) {
        if (test.run()) {
            continue;
        }
        ++failures;
        (void)fprintf(stderr, "[FAIL] %s\n", test.name);
    }
    (void)fprintf(
        stdout,
        "capability-owner tests: %zu passed, %zu failed\n",
        sizeof(tests) / sizeof(tests[0]) - failures,
        failures);
    return failures == 0 ? 0 : 1;
}
