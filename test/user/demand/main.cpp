#include <string_view>
#include <vector>
#include <memory>
#include <protocols/stream.hpp>
#include <ipc.hpp>
#include <libk/parse.hpp>
#include <util.hpp>
#include <cap.hpp>

volatile uint8_t initialized[8197]{0x39, 0x82};
volatile uint8_t zeroed[16397];
static void check(bool value) {
    if (!value)
        sys::exit(STATUS_INTERNAL);
}

int main(int argc, char** argv) {

    const auto& info = rt_startup();
    check((static_cast<size_t>(argc) == 1 || static_cast<size_t>(argc) == 3) &&
          (std::string_view{argv[0]} == "demand"));
    for (size_t i = 0; i < sizeof(initialized); ++i) {
        check(initialized[i] == (i == 0 ? 0x39 : i == 1 ? 0x82 : 0));
        initialized[i] = i % 251;
    }
    for (size_t i = 0; i < sizeof(zeroed); ++i) {
        check(zeroed[i] == 0);
        zeroed[i] = (i + 7) % 251;
    }
    const auto pool = info.pool;
    const auto vspace = info.vspace;
    auto stress = vm_area::create(pool, vspace, 0x28000000, 4096);
    auto release = vm_area::create(pool, vspace, 0x28003000, 4096);
    check(stress && release);
    check(*reinterpret_cast<const volatile uint8_t*>(stress.value().address) == 0);
    for (size_t i = 0; i < sizeof(initialized); ++i)
        check(initialized[i] == i % 251);
    for (size_t i = 0; i < sizeof(zeroed); ++i)
        check(zeroed[i] == (i + 7) % 251);
    check(*reinterpret_cast<const volatile uint8_t*>(release.value().address) == 0);
    if (const auto rc = stress.value().close(); rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = release.value().close(); rc != STATUS_OK)
        sys::exit(rc);
    auto reused = vm_area::create(pool, vspace, 0x28003000, 4096);
    check(reused && *reinterpret_cast<const volatile uint8_t*>(reused.value().address) == 0);
    if (const auto rc = reused.value().close(); rc != STATUS_OK)
        sys::exit(rc);
    // Grow across backing chunks, free fragmented allocations, and reuse quota.
    // This runs in an ordinary app Group with the production runtime.
    for (size_t pass = 0; pass < 12; ++pass) {
        auto first = std::make_unique<uint8_t[]>(73);
        struct alignas(4096) Page {
            uint8_t bytes[4096];
        };
        auto page = std::make_unique<Page>();
        check(reinterpret_cast<uintptr_t>(page.get()) % alignof(Page) == 0);
        std::vector<uint64_t> values;
        for (size_t i = 0; i < 40'000; ++i)
            values.push_back(i + pass);
        first.reset();
        for (size_t i = 0; i < values.size(); ++i)
            check(values[i] == i + pass);
    }
    str_writer{info.selector(rt_stdout)}.write(
        "[demand] initialized data, BSS, private writes and VM reuse ok\n");
    if (static_cast<size_t>(argc) == 3) {
        const auto duration = libk::parse<uint64_t>(argv[1]);
        const auto seed = libk::parse<uint64_t>(argv[2]);
        check(duration && seed && *seed > 0 && *seed < 256);
        initialized[0] = *seed;
        zeroed[0] = *seed + 7;

        const auto deadline = rt_after_ms(*duration);
        check(static_cast<bool>(deadline));
        str_writer{info.selector(rt_stdout)}.write("[demand] holding private pages\n");
        check(sys::notification_wait(info.events, *deadline).status ==
              STATUS_TIMED_OUT);
        check(initialized[0] == *seed && zeroed[0] == static_cast<uint8_t>(*seed + 7));
    }
    sys::exit();
}
