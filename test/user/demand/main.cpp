#include <user/lib/stream.hpp>
#include <user/lib/clock.hpp>
#include <user/lib/mapped_memory.hpp>
#include <user/lib/service.hpp>
#include <uapi/test_scenario.h>

namespace {
volatile uint8_t initialized[8197]{0x39, 0x82};
volatile uint8_t zeroed[16397];
void check(bool value) { if (!value) myos::exit(MYOS_STATUS_INTERNAL); }
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    check((info.argument_count() == 1 || info.argument_count() == 3)
        && service::equal(info.argument(0), "demand"));
    for (size_t i = 0; i < sizeof(initialized); ++i) {
        check(initialized[i] == (i == 0 ? 0x39 : i == 1 ? 0x82 : 0));
        initialized[i] = i % 251;
    }
    for (size_t i = 0; i < sizeof(zeroed); ++i) {
        check(zeroed[i] == 0);
        zeroed[i] = (i + 7) % 251;
    }
    const auto pool = service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const auto vspace = service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    auto stress = MappedMemory::create(pool, vspace, MYOS_TEST_PRESSURE_STRESS_ADDRESS, 4096);
    auto release = MappedMemory::create(pool, vspace, MYOS_TEST_PRESSURE_RELEASE_ADDRESS, 4096);
    check(stress && release);
    check(*reinterpret_cast<const volatile uint8_t*>(stress.value().address) == 0);
    for (size_t i = 0; i < sizeof(initialized); ++i) check(initialized[i] == i % 251);
    for (size_t i = 0; i < sizeof(zeroed); ++i) check(zeroed[i] == (i + 7) % 251);
    check(*reinterpret_cast<const volatile uint8_t*>(release.value().address) == 0);
    service::require(stress.value().close());
    service::require(release.value().close());
    service::require(vm_sync(vspace).status);
    auto reused = MappedMemory::create(pool, vspace, MYOS_TEST_PRESSURE_RELEASE_ADDRESS, 4096);
    check(reused && *reinterpret_cast<const volatile uint8_t*>(reused.value().address) == 0);
    service::require(reused.value().close());
    stream::Writer{service::capability(info, bootstrap::imports::Stdout)}
        .write("[demand] initialized data, BSS, private writes and VM reuse ok\n");
    if (info.argument_count() == 3) {
        const auto duration = decimal(info.argument(1));
        const auto seed = decimal(info.argument(2));
        check(duration && seed && *seed > 0 && *seed < 256);
        initialized[0] = *seed;
        zeroed[0] = *seed + 7;
        Clock clock;
        service::require(clock.open());
        const auto deadline = clock.after_ms(*duration);
        check(static_cast<bool>(deadline));
        stream::Writer{service::capability(info, bootstrap::imports::Stdout)}.write("[demand] holding private pages\n");
        check(notification_wait(service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION),
            *deadline).status == MYOS_STATUS_TIMED_OUT);
        check(initialized[0] == *seed && zeroed[0] == static_cast<uint8_t>(*seed + 7));
    }
    exit();
}
