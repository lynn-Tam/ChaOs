#include <optional>
#include <servers/runtime/service.hpp>
#include <sys/handle.hpp>
#include <servers/deploy/launch.hpp>
#include <servers/uart/port.hpp>

namespace {
deploy::program program;
deploy::tasks<2> supervisor;

auto binding(cap_t device) noexcept -> deploy::source {
    CapView ceiling{};
    ceiling.version = CAP_ATTENUATION_VERSION_CURRENT;
    ceiling.kind = OBJECT_KIND_DEVICE;
    ceiling.size = CAP_ATTENUATION_SIZE;
    ceiling.rights = RIGHT_CONNECT | RIGHT_DUPLICATE;
    return {"block.device", {device, 0}, ceiling};
}
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    if (info.device_count() != 2) exit(STATUS_NOT_FOUND);
    auto mapping = MappedMemory::map(service::capability(info, BOOT_VSPACE),
        cap::OwnedCap{{service::capability(info, boot::UartMem), 0}},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    service::require(supervisor.load(program, info));
    supervisor.open(info);
    service::require(supervisor.add_boot_sources(info));
    std::optional<deploy::tasks<2>::handle> tasks[2];
    for (size_t i = 0; i < 2; ++i) {
        const deploy::source source[] = {binding(info.device(i))};
        status_t status{};
        tasks[i] = supervisor.launch(program, deploy::tasks<2>::name("io-test"), status,
            {.sources = source});
        if (status != STATUS_OK || !tasks[i]) {
            uart::Printer printer{uart::Writer{port}};
            (void)printer.print<"[io-dual] launch={} status={}\n">(i, status);
            exit(status);
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        const auto status = supervisor.wait(*tasks[i]);
        if (status != STATUS_OK) {
            uart::Printer printer{uart::Writer{port}};
            (void)printer.print<"[io-dual] worker={} status={}\n">(i, status);
            exit(status);
        }
    }
    port.write("[io-dual] two devices completed independent DMA and teardown\n");
    exit();
}
