#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>

#include <libk/utility.hpp>
#include <libk/assert.hpp>
#include <user/lib/capability.hpp>

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    __builtin_trap();
}
} // namespace libk

namespace {

struct FakeBackend final {
    struct Call final {
        myos::cap::CapRef reference;
    };

    static inline Call calls[32]{};
    static inline size_t call_count{};
    static inline myos_status_t next_status{MYOS_STATUS_OK};
    static inline size_t fault_count{};
    static inline jmp_buf* fault_target{};

    static void reset() noexcept {
        call_count = 0;
        next_status = MYOS_STATUS_OK;
        fault_count = 0;
        fault_target = nullptr;
    }

    [[nodiscard]] static auto close(
        myos::cap::CapRef reference) noexcept -> myos_status_t {
        if (call_count < sizeof(calls) / sizeof(calls[0])) {
            calls[call_count++] = Call{reference};
        }
        const myos_status_t status = next_status;
        next_status = MYOS_STATUS_OK;
        return status;
    }

    [[noreturn]] static void ownership_fault(
        myos_status_t) noexcept {
        ++fault_count;
        if (fault_target != nullptr) {
            longjmp(*fault_target, 1);
        }
        for (;;) {}
    }
};

using Owner = myos::cap::BasicOwnedCap<FakeBackend>;
using Deployment = myos::cap::BasicDeploymentCaps<4, 4, FakeBackend>;

static_assert(myos::cap::CapBackend<FakeBackend>);
static_assert(!libk::is_copy_constructible_v<Deployment>);

template<typename T>
concept HasManagerRelease = requires(T& deployment) {
    deployment.release_manager();
};

template<typename T>
concept HasManagerClose = requires(T& deployment) {
    deployment.close_manager();
};

static_assert(!HasManagerRelease<Deployment>);
static_assert(!HasManagerClose<Deployment>);

[[nodiscard]] auto call_is(
    size_t index,
    myos_cap_t selector,
    myos_cap_t cspace) noexcept -> bool {
    return index < FakeBackend::call_count
        && FakeBackend::calls[index].reference
            == myos::cap::CapRef{selector, cspace};
}

[[nodiscard]] auto test_move_and_release() noexcept -> bool {
    FakeBackend::reset();
    Owner source{{7, 9}};
    Owner moved{libk::move(source)};
    if (source || !moved || moved.reference() != myos::cap::CapRef{7, 9}) {
        return false;
    }
    const auto released = moved.release();
    return !moved && released == myos::cap::CapRef{7, 9}
        && FakeBackend::call_count == 0;
}

[[nodiscard]] auto test_explicit_failure_retains_ownership() noexcept -> bool {
    FakeBackend::reset();
    Owner owner{{11, 0}};
    FakeBackend::next_status = MYOS_STATUS_BUSY;
    if (owner.close() != MYOS_STATUS_BUSY
        || !owner
        || owner.reference() != myos::cap::CapRef{11, 0}) {
        return false;
    }
    return owner.close() == MYOS_STATUS_OK
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
        FakeBackend::next_status = MYOS_STATUS_BUSY;
        return false;
    }
    FakeBackend::fault_target = nullptr;
    return FakeBackend::fault_count == 1
        && FakeBackend::call_count == 1
        && call_is(0, 13, 0);
}

[[nodiscard]] auto test_journal_capacity_closes_adopted_cap() noexcept -> bool {
    FakeBackend::reset();
    myos::cap::BasicCapJournal<1, FakeBackend> journal{};
    if (!journal.adopt(17, 0) || journal.adopt(19, 0)
        || journal.size() != 1
        || FakeBackend::call_count != 1
        || !call_is(0, 19, 0)) {
        return false;
    }
    if (journal.close() != MYOS_STATUS_OK
        || FakeBackend::call_count != 2
        || !call_is(1, 17, 0)) {
        return false;
    }
    return journal.size() == 0;
}

[[nodiscard]] auto test_remote_drains_before_current_roots() noexcept -> bool {
    FakeBackend::reset();
    myos::cap::BasicCapJournal<4, FakeBackend> journal{};
    if (!journal.adopt(1, 0)
        || !journal.adopt(2, 1)
        || !journal.adopt(3, 0)
        || journal.close() != MYOS_STATUS_OK
        || FakeBackend::call_count != 3) {
        return false;
    }
    return call_is(0, 2, 1)
        && call_is(1, 3, 0)
        && call_is(2, 1, 0);
}

[[nodiscard]] auto test_remote_failure_keeps_manager_armed() noexcept -> bool {
    FakeBackend::reset();
    myos::cap::BasicCapJournal<3, FakeBackend> journal{};
    if (!journal.adopt(31, 0) || !journal.adopt(32, 31)) {
        return false;
    }
    FakeBackend::next_status = MYOS_STATUS_BUSY;
    if (journal.close() != MYOS_STATUS_BUSY
        || FakeBackend::call_count != 1
        || !call_is(0, 32, 31)) {
        return false;
    }
    return journal.close() == MYOS_STATUS_OK
        && FakeBackend::call_count == 3
        && call_is(1, 32, 31)
        && call_is(2, 31, 0);
}

[[nodiscard]] auto test_remote_selector_is_exact() noexcept -> bool {
    FakeBackend::reset();
    Owner owner{{0xdead, 0xbeef}};
    if (owner.close() != MYOS_STATUS_OK
        || FakeBackend::call_count != 1) {
        return false;
    }
    return call_is(0, 0xdead, 0xbeef);
}

[[nodiscard]] auto test_remote_requires_armed_manager() noexcept -> bool {
    FakeBackend::reset();
    Deployment deployment{};
    Owner pending{{41, 0}};
    if (deployment.can_adopt_remote(pending)
        || deployment.adopt_remote(libk::move(pending))
        || !pending
        || deployment.remote_size() != 0
        || FakeBackend::call_count != 0) {
        return false;
    }
    return pending.close() == MYOS_STATUS_OK
        && FakeBackend::call_count == 1
        && call_is(0, 41, 0);
}

[[nodiscard]] auto test_remote_rejects_arbitrary_cspace() noexcept -> bool {
    FakeBackend::reset();
    Deployment deployment{};
    Owner manager{{45, 0}};
    Owner remote{{46, 999}};
    if (!deployment.install_manager(libk::move(manager))
        || deployment.can_adopt_remote(remote)
        || deployment.adopt_remote(libk::move(remote))
        || !remote
        || FakeBackend::call_count != 0) {
        return false;
    }
    return remote.close() == MYOS_STATUS_OK
        && deployment.close() == MYOS_STATUS_OK
        && FakeBackend::call_count == 2
        && call_is(0, 46, 999)
        && call_is(1, 45, 0);
}

[[nodiscard]] auto test_remote_accepts_exact_manager_identity() noexcept
    -> bool {
    FakeBackend::reset();
    Deployment deployment{};
    Owner manager{{101, 0}};
    Owner remote{{102, 101}};
    Owner current{{102, 0}};
    if (!deployment.install_manager(libk::move(manager))
        || !deployment.can_adopt_remote(remote)
        || !deployment.adopt_remote(libk::move(remote))
        || remote
        || deployment.can_adopt_remote(current)
        || !deployment.adopt_local(libk::move(current))
        || current
        || !deployment.borrow_remote(0)
        || deployment.borrow_remote(0).value()
            != myos::cap::CapRef{102, 101}
        || !deployment.borrow_local(0)
        || deployment.borrow_local(0).value()
            != myos::cap::CapRef{102, 0}) {
        return false;
    }
    return deployment.close() == MYOS_STATUS_OK
        && FakeBackend::call_count == 3
        && call_is(0, 102, 101)
        && call_is(1, 101, 0)
        && call_is(2, 102, 0);
}

[[nodiscard]] auto test_manager_installs_once_and_is_borrowed() noexcept
    -> bool {
    FakeBackend::reset();
    Deployment deployment{};
    Owner manager{{51, 0}};
    Owner replacement{{52, 0}};
    if (!deployment.install_manager(libk::move(manager))
        || manager
        || deployment.install_manager(libk::move(replacement))
        || !replacement
        || !deployment.borrow_manager()
        || deployment.borrow_manager().value()
            != myos::cap::CapRef{51, 0}) {
        return false;
    }
    if (deployment.close() != MYOS_STATUS_OK
        || replacement.close() != MYOS_STATUS_OK) {
        return false;
    }
    return FakeBackend::call_count == 2
        && call_is(0, 51, 0)
        && call_is(1, 52, 0);
}

[[nodiscard]] auto test_partial_drain_seals_future_adoption() noexcept
    -> bool {
    FakeBackend::reset();
    Deployment deployment{};
    Owner manager{{61, 0}};
    Owner remote{{62, 61}};
    Owner local{{63, 0}};
    if (!deployment.install_manager(libk::move(manager))
        || !deployment.adopt_remote(libk::move(remote))
        || !deployment.adopt_local(libk::move(local))) {
        return false;
    }

    FakeBackend::next_status = MYOS_STATUS_BUSY;
    Owner rejected_remote{{64, 61}};
    if (deployment.close() != MYOS_STATUS_BUSY
        || !deployment.sealed()
        || deployment.can_adopt_remote(rejected_remote)
        || deployment.adopt_remote(libk::move(rejected_remote))
        || !rejected_remote
        || FakeBackend::call_count != 1
        || !call_is(0, 62, 61)) {
        return false;
    }
    if (rejected_remote.close() != MYOS_STATUS_OK
        || deployment.close() != MYOS_STATUS_OK
        || FakeBackend::call_count != 5) {
        return false;
    }
    return call_is(1, 64, 61)
        && call_is(2, 62, 61)
        && call_is(3, 61, 0)
        && call_is(4, 63, 0);
}

[[nodiscard]] auto test_destructor_orders_remote_manager_local() noexcept
    -> bool {
    FakeBackend::reset();
    {
        Deployment deployment{};
        Owner manager{{71, 0}};
        Owner remote{{72, 71}};
        Owner local{{73, 0}};
        if (!deployment.install_manager(libk::move(manager))
            || !deployment.adopt_remote(libk::move(remote))
            || !deployment.adopt_local(libk::move(local))) {
            return false;
        }
    }
    return FakeBackend::call_count == 3
        && call_is(0, 72, 71)
        && call_is(1, 71, 0)
        && call_is(2, 73, 0);
}

[[nodiscard]] auto test_deployment_move_preserves_manager_relation() noexcept
    -> bool {
    FakeBackend::reset();
    Deployment source{};
    Owner manager{{81, 0}};
    Owner remote{{82, 81}};
    Owner local{{83, 0}};
    if (!source.install_manager(libk::move(manager))
        || !source.adopt_remote(libk::move(remote))
        || !source.adopt_local(libk::move(local))) {
        return false;
    }
    Deployment moved{libk::move(source)};
    if (!source.sealed()
        || source.remote_size() != 0
        || !moved.borrow_remote(0)
        || moved.borrow_remote(0).value()
            != myos::cap::CapRef{82, 81}) {
        return false;
    }
    return moved.close() == MYOS_STATUS_OK
        && FakeBackend::call_count == 3
        && call_is(0, 82, 81)
        && call_is(1, 81, 0)
        && call_is(2, 83, 0);
}

[[nodiscard]] auto test_deployment_capacity_closes_adopted_cap() noexcept
    -> bool {
    FakeBackend::reset();
    using SmallDeployment =
        myos::cap::BasicDeploymentCaps<1, 1, FakeBackend>;
    SmallDeployment deployment{};
    Owner manager{{91, 0}};
    Owner local{{92, 0}};
    Owner overflow{{93, 0}};
    Owner remote{{94, 91}};
    Owner remote_overflow{{95, 91}};
    if (!deployment.install_manager(libk::move(manager))
        || !deployment.adopt_local(libk::move(local))
        || deployment.adopt_local(libk::move(overflow))
        || overflow
        || !deployment.can_adopt_remote(remote)
        || !deployment.adopt_remote(libk::move(remote))
        || remote
        || deployment.can_adopt_remote(remote_overflow)
        || deployment.adopt_remote(libk::move(remote_overflow))
        || !remote_overflow
        || FakeBackend::call_count != 1
        || !call_is(0, 93, 0)) {
        return false;
    }
    return overflow.close() == MYOS_STATUS_OK
        && remote_overflow.close() == MYOS_STATUS_OK
        && deployment.close() == MYOS_STATUS_OK
        && FakeBackend::call_count == 5
        && call_is(1, 95, 91)
        && call_is(2, 94, 91)
        && call_is(3, 91, 0)
        && call_is(4, 92, 0);
}

struct Test final {
    const char* name;
    bool (*run)() noexcept;
};

constexpr Test tests[] = {
    {"move/release", test_move_and_release},
    {"explicit close failure retains owner", test_explicit_failure_retains_ownership},
    {"destructor ownership fault", test_destructor_fault_is_bounded},
    {"journal capacity closes adopted cap", test_journal_capacity_closes_adopted_cap},
    {"remote before current reverse close", test_remote_drains_before_current_roots},
    {"remote failure retains manager", test_remote_failure_keeps_manager_armed},
    {"remote selector exactness", test_remote_selector_is_exact},
    {"remote adoption requires manager", test_remote_requires_armed_manager},
    {"remote adoption rejects arbitrary CSpace", test_remote_rejects_arbitrary_cspace},
    {"remote adoption requires exact manager identity", test_remote_accepts_exact_manager_identity},
    {"manager installs once as borrowed authority", test_manager_installs_once_and_is_borrowed},
    {"partial drain seals future adoption", test_partial_drain_seals_future_adoption},
    {"destructor drains remote manager local", test_destructor_orders_remote_manager_local},
    {"deployment move preserves manager relation", test_deployment_move_preserves_manager_relation},
    {"deployment capacity closes adopted cap", test_deployment_capacity_closes_adopted_cap},
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
