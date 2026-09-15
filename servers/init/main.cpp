#include <user/lib/supervisor.hpp>
#include <user/lib/uart.hpp>

namespace {
myos::deploy::Program program;
using Supervisor = myos::deploy::Supervisor<5, 24>;
Supervisor supervisor;
myos::cap::OwnedCap channels[10];
libk::optional<Supervisor::Handle> tasks[5];

void channel_pair(myos_cap_t pool, size_t index,
                  const char* first, const char* second, bool io = false, size_t depth = 1,
                  size_t relations = 2) {
    const auto pair = myos::channel_create(pool, io ? depth : 16, MYOS_CHANNEL_MAX_WORDS, io ? 4 : 0, relations);
    myos::service::require(pair.status);
    channels[index] = myos::cap::OwnedCap{{pair.value, 0}};
    channels[index + 1] = myos::cap::OwnedCap{{pair.value2, 0}};
    constexpr auto rights = MYOS_RIGHT_SEND | MYOS_RIGHT_RECEIVE | MYOS_RIGHT_DUPLICATE;
    myos::service::require(supervisor.add(first, pair.value,
        MYOS_OBJECT_KIND_CHANNEL, rights, 0));
    myos::service::require(supervisor.add(second, pair.value2,
        MYOS_OBJECT_KIND_CHANNEL, rights, 1));
}

void configure_uart(const myos::bootstrap::BootstrapView& info) {
    const auto vspace = myos::service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    const auto memory = myos::service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY);
    constexpr auto address = 0x30010000;
    const auto region = myos::vm_create_region(vspace, address, 4096,
        MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_DEVICE,
        MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
    myos::service::require(region.status);
    myos::service::require(myos::vm_map(region.value, memory, address, 4096, 0,
                                      MYOS_VM_READ | MYOS_VM_WRITE).status);
    myos::uart::Port port{address};
    port.reset();
    port.write("init: native services\n");
    myos::service::require(myos::vm_complete(vspace, myos::vm_unmap(region.value, address, 4096)).status);
    myos::service::require(myos::vm_complete(vspace, myos::vm_destroy_region(region.value)).status);
    myos::service::require(myos::cap_close(region.value).status);
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = myos::service::bootstrap(address, size);
    configure_uart(info);
    myos::service::require(supervisor.load(program, info));
    supervisor.open(info);
    myos::service::require(supervisor.add_boot_sources(info));
    myos::service::require(supervisor.add("uart.memory",
        myos::service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY),
        MYOS_OBJECT_KIND_MEMORY, MYOS_RIGHT_MAP, 0, 1,
        MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_DEVICE));
    myos::service::require(supervisor.add("uart.irq",
        myos::service::capability(info, MYOS_BOOTSTRAP_CAP_IRQ), MYOS_OBJECT_KIND_IRQ,
        MYOS_RIGHT_ROUTE | MYOS_RIGHT_OBSERVE | MYOS_RIGHT_ACK));
    const auto pool = myos::service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    channel_pair(pool, 0, "console.sender", "console.receiver");
    channel_pair(pool, 2, "input.sender", "input.receiver");
    channel_pair(pool, 4, "process.client", "process.server", false, 1, 3);
    channel_pair(pool, 6, "block.client", "block.server", true);
    channel_pair(pool, 8, "files.client", "files.server", true, 8);
    myos::service::require(supervisor.add("block.device",
        myos::service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE), MYOS_OBJECT_KIND_DEVICE,
        MYOS_RIGHT_CONNECT | MYOS_RIGHT_DUPLICATE));
    const char* roles[] = {"uart", "block", "files", "process_server", "shell"};
    myos_status_t status = MYOS_STATUS_OK;
    for (size_t i = 0; i < 5; ++i) {
        tasks[i] = supervisor.launch(program, roles[i], status);
        if (!tasks[i]) break;
    }
    if (status == MYOS_STATUS_OK) {
        for (;;) {
            bool terminal = false;
            for (auto& task : tasks) {
                const auto observed = supervisor.observe(*task);
                myos::service::require(observed.status);
                if (observed.value != 0) {
                    status = supervisor.wait(*task);
                    task.reset();
                    terminal = true;
                    break;
                }
            }
            if (terminal) break;
            myos::yield();
        }
    }
    for (size_t i = 5; i != 0; --i)
        if (tasks[i - 1]) static_cast<void>(supervisor.stop(*tasks[i - 1]));
    myos::exit(status);
}
