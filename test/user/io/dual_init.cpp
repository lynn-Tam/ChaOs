#include <optional>
#include <servers/runtime/service.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/uart/port.hpp>

namespace {
deploy::program program;
deploy::tasks<2> supervisor;

auto binding(myos_cap_t device) noexcept -> deploy::source {
    myos_cap_attenuation ceiling{};
    ceiling.version = MYOS_CAP_ATTENUATION_VERSION_CURRENT;
    ceiling.kind = MYOS_OBJECT_KIND_DEVICE;
    ceiling.size = MYOS_CAP_ATTENUATION_SIZE;
    ceiling.rights = MYOS_RIGHT_CONNECT | MYOS_RIGHT_DUPLICATE;
    return {"block.device", {device, 0}, ceiling};
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    if (info.device_count() != 2) exit(MYOS_STATUS_NOT_FOUND);
    auto mapping = MappedMemory::map(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        cap::OwnedCap{{service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY), 0}},
        0x30010000, 4096, MYOS_VM_READ | MYOS_VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    service::require(supervisor.load(program, info));
    supervisor.open(info);
    service::require(supervisor.add_boot_sources(info));
    std::optional<deploy::tasks<2>::handle> tasks[2];
    for (size_t i = 0; i < 2; ++i) {
        const deploy::source source[] = {binding(info.device(i))};
        myos_status_t status{};
        tasks[i] = supervisor.launch(program, deploy::tasks<2>::name("io-test"), status,
            {.sources = source});
        if (status != MYOS_STATUS_OK || !tasks[i]) {
            uart::Printer printer{uart::Writer{port}};
            (void)printer.print<"[io-dual] launch={} status={}\n">(i, status);
            exit(status);
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        const auto status = supervisor.wait(*tasks[i]);
        if (status != MYOS_STATUS_OK) {
            uart::Printer printer{uart::Writer{port}};
            (void)printer.print<"[io-dual] worker={} status={}\n">(i, status);
            exit(status);
        }
    }
    port.write("[io-dual] two devices completed independent DMA and teardown\n");
    exit();
}
