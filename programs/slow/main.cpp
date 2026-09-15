#include <user/lib/stream.hpp>
#include <user/lib/clock.hpp>

// A byte counter with a paced consumer; optional LIMIT exits before EOF.
extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto delay = info.argument_count() >= 2 ? decimal(info.argument(1)) : libk::nullopt;
    const auto limit = info.argument_count() == 3 ? decimal(info.argument(2)) : libk::optional<uint64_t>{UINT64_MAX};
    if (!delay || !limit || info.argument_count() > 3) exit(MYOS_STATUS_BAD_ARGS);
    Clock clock;
    service::require(clock.open());
    const auto timer = notification_create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL), 1);
    service::require(timer.status);
    stream::Reader input;
    service::require(input.open(service::capability(info, bootstrap::imports::Stdin),
        service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION)));
    uint64_t count{};
    while (count < *limit) {
        const auto deadline = clock.after_ms(*delay);
        if (!deadline) exit(MYOS_STATUS_BAD_ARGS);
        const auto timed = notification_wait(timer.value, *deadline);
        if (timed.status != MYOS_STATUS_TIMED_OUT) exit(timed.status);
        service::Message message;
        service::require(input.read(message));
        if (message.size == 0) break;
        count += message.size;
    }
    stream::Writer output{service::capability(info, bootstrap::imports::Stdout)};
    // No libc formatting or allocator is required for this bounded tool.
    char digits[21]{};
    size_t first = sizeof(digits);
    do { digits[--first] = '0' + count % 10; count /= 10; } while (count != 0);
    output.write("bytes: "); output.write(digits + first, sizeof(digits) - first); output.put('\n');
    exit();
}
