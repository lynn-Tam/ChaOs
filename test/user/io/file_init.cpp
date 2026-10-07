#include <utility>
#include <servers/runtime/service.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/uart/port.hpp>

namespace { deploy::program program;
#ifdef TEST_FILE_FAILURE
constexpr size_t TaskCount = 4;
constexpr status_t ExpectedStatus = STATUS_PEER_FAULT;
#else
constexpr size_t TaskCount = 3;
constexpr status_t ExpectedStatus = STATUS_OK;
#endif
deploy::tasks<TaskCount> supervisor;
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    auto mapping = MappedMemory::map(service::capability(info, BOOT_VSPACE),
        cap::OwnedCap{{service::capability(info, boot::UartMem), 0}},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    uart::Printer printer{uart::Writer{port}};
    service::require(supervisor.load(program, info));
    supervisor.open(info);
    service::require(supervisor.add_boot_sources(info));
    service::require(supervisor.add("block.device", service::initial_device(info),
        OBJECT_KIND_DEVICE, RIGHT_DUPLICATE | RIGHT_CONNECT));
    constexpr const char* sources[][2] = {
        {"block.client", "block.server"}, {"files.client", "files.server"}};
    cap::OwnedCap endpoints[4];
    for (size_t i = 0; i < 2; ++i) {
        const auto pair = channel_create(service::capability(info, BOOT_POOL),
            i == 0 ? 1 : 8, CHANNEL_MAX_WORDS, 4, 2);
        service::require(pair.status);
        endpoints[i * 2] = cap::OwnedCap{{pair.value, 0}};
        endpoints[i * 2 + 1] = cap::OwnedCap{{pair.value2, 0}};
        constexpr auto rights = RIGHT_SEND | RIGHT_RECEIVE | RIGHT_DUPLICATE;
        service::require(supervisor.add(sources[i][0], pair.value, OBJECT_KIND_CHANNEL, rights, 0));
        service::require(supervisor.add(sources[i][1], pair.value2, OBJECT_KIND_CHANNEL, rights, 1));
    }
    constexpr const char* names[] = {"block", "files", "file-client", "file-client"};
    deploy::tasks<TaskCount>::handle ids[TaskCount]{};
    bool finished[TaskCount]{};
    size_t remaining = TaskCount - 2;
    for (size_t i = 0; i < TaskCount; ++i) {
        status_t status{};
        auto child = supervisor.launch(program, names[i], status);
        if (!child) {
            (void)printer.print<"[file-session] launch {} failed status={}\n">(i, status);
            exit(status);
        }
        ids[i] = std::move(*child);
    }
    for (;;) {
        for (size_t i = 0; i < TaskCount; ++i) {
            if (finished[i]) continue;
            const auto observed = supervisor.observe(ids[i]);
            service::require(observed.status);
            if (observed.value == 0) continue;
            const auto status = supervisor.wait(ids[i]);
            if (i < 2 || status != ExpectedStatus) {
                (void)printer.print<"[file-session] task {} failed status={}\n">(i, status);
                exit(STATUS_INTERNAL);
            }
            finished[i] = true;
            if (--remaining != 0) continue;
            for (size_t j = 0; j < 2; ++j) {
                const auto live = supervisor.observe(ids[j]);
                service::require(live.status);
                if (live.value != 0) exit(STATUS_INTERNAL);
                (void)supervisor.stop(ids[j]);
            }
#ifdef TEST_FILE_FAILURE
            port.write("[file-failure] ok: read error, two faulted mappings, closed sessions, services live\n");
#else
            port.write("[file-session] ok: two sessions, byte reads, 64 outstanding, EOF, handles, close\n");
#endif
            exit();
        }
        yield();
    }
}
