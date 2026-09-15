#include <user/lib/mapped_memory.hpp>
#include <user/lib/supervisor.hpp>
#include <user/lib/uart.hpp>

namespace { myos::deploy::Program program;
#ifdef MYOS_TEST_FILE_FAILURE
constexpr size_t TaskCount = 4;
constexpr myos_status_t ExpectedStatus = MYOS_STATUS_PEER_FAULT;
#else
constexpr size_t TaskCount = 3;
constexpr myos_status_t ExpectedStatus = MYOS_STATUS_OK;
#endif
myos::deploy::Supervisor<TaskCount> supervisor;
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    auto mapping = MappedMemory::map(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        cap::OwnedCap{{service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY), 0}},
        0x30010000, 4096, MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_DEVICE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping.value().address};
    port.reset();
    uart::Printer printer{uart::Writer{port}};
    service::require(supervisor.load(program, info));
    supervisor.open(info);
    service::require(supervisor.add_boot_sources(info));
    service::require(supervisor.add("block.device", service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE),
        MYOS_OBJECT_KIND_DEVICE, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONNECT));
    constexpr const char* sources[][2] = {
        {"block.client", "block.server"}, {"files.client", "files.server"}};
    cap::OwnedCap endpoints[4];
    for (size_t i = 0; i < 2; ++i) {
        const auto pair = channel_create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
            i == 0 ? 1 : 8, MYOS_CHANNEL_MAX_WORDS, 4, 2);
        service::require(pair.status);
        endpoints[i * 2] = cap::OwnedCap{{pair.value, 0}};
        endpoints[i * 2 + 1] = cap::OwnedCap{{pair.value2, 0}};
        constexpr auto rights = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_DUPLICATE;
        service::require(supervisor.add(sources[i][0], pair.value, MYOS_OBJECT_KIND_CHANNEL, rights, 0));
        service::require(supervisor.add(sources[i][1], pair.value2, MYOS_OBJECT_KIND_CHANNEL, rights, 1));
    }
    constexpr const char* names[] = {"block", "files", "file-client", "file-client"};
    deploy::Supervisor<TaskCount>::Handle ids[TaskCount]{};
    bool finished[TaskCount]{};
    size_t remaining = TaskCount - 2;
    for (size_t i = 0; i < TaskCount; ++i) {
        myos_status_t status{};
        auto child = supervisor.launch(program, names[i], status);
        if (!child) {
            (void)printer.print<"[file-session] launch {} failed status={}\n">(i, status);
            exit(status);
        }
        ids[i] = libk::move(*child);
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
                exit(MYOS_STATUS_INTERNAL);
            }
            finished[i] = true;
            if (--remaining != 0) continue;
            for (size_t j = 0; j < 2; ++j) {
                const auto live = supervisor.observe(ids[j]);
                service::require(live.status);
                if (live.value != 0) exit(MYOS_STATUS_INTERNAL);
                (void)supervisor.stop(ids[j]);
            }
#ifdef MYOS_TEST_FILE_FAILURE
            port.write("[file-failure] ok: read error, two faulted mappings, closed sessions, services live\n");
#else
            port.write("[file-session] ok: two sessions, byte reads, 64 outstanding, EOF, handles, close\n");
#endif
            exit();
        }
        yield();
    }
}
