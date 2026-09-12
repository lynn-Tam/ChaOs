#include <user/lib/mapped_memory.hpp>
#include <user/lib/supervisor.hpp>
#include <user/lib/uart.hpp>

namespace { myos::deploy::Supervisor<3> supervisor; }

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
    service::require(supervisor.open(info));
    service::require(supervisor.add_boot_sources(info));
    service::require(supervisor.add("block.device", service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE),
        MYOS_OBJECT_KIND_DEVICE, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONNECT));
    constexpr const char* sources[][2] = {
        {"block.client", "block.server"}, {"files.first.client", "files.first.server"},
        {"files.second.client", "files.second.server"}};
    cap::OwnedCap endpoints[6];
    for (size_t i = 0; i < 3; ++i) {
        const auto pair = channel_create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
            1, MYOS_CHANNEL_MAX_WORDS, 4, 2);
        service::require(pair.status);
        endpoints[i * 2] = cap::OwnedCap{{pair.value, 0}};
        endpoints[i * 2 + 1] = cap::OwnedCap{{pair.value2, 0}};
        constexpr auto rights = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_DUPLICATE;
        service::require(supervisor.add(sources[i][0], pair.value, MYOS_OBJECT_KIND_CHANNEL, rights, 0));
        service::require(supervisor.add(sources[i][1], pair.value2, MYOS_OBJECT_KIND_CHANNEL, rights, 1));
    }
    constexpr const char* names[] = {"block", "files", "file-client"};
    deploy::Supervisor<3>::Handle ids[3]{};
    for (size_t i = 0; i < 3; ++i) {
        myos_status_t status{};
        auto child = supervisor.launch(names[i], status);
        if (!child) {
            (void)printer.print<"[file-session] launch {} failed status={}\n">(i, status);
            exit(status);
        }
        ids[i] = libk::move(*child);
    }
    for (;;) {
        for (size_t i = 0; i < 3; ++i) {
            const auto observed = supervisor.observe(ids[i]);
            service::require(observed.status);
            if (observed.value == 0) continue;
            const auto status = supervisor.wait(ids[i]);
            for (size_t j = 0; j < 3; ++j) if (j != i) {
                (void)supervisor.stop(ids[j]);
            }
            if (i != 2 || status != MYOS_STATUS_OK) {
                (void)printer.print<"[file-session] task {} failed status={}\n">(i, status);
                exit(MYOS_STATUS_INTERNAL);
            }
            port.write("[file-session] ok: two sessions, byte reads, 64 outstanding, EOF, handles, close\n");
            exit();
        }
        yield();
    }
}
