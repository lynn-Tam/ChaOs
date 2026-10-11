#include <sys/protocol.hpp>
#include <sys/pci.hpp>
#include <utility>
#include <sys/channel.hpp>
#include <sys/handle.hpp>
#include <test/user/task.hpp>
#include <servers/uart/port.hpp>

namespace { proc::package program;
#ifdef TEST_FILE_FAILURE
constexpr size_t TaskCount = 4;
constexpr status_t ExpectedStatus = STATUS_PEER_FAULT;
#else
constexpr size_t TaskCount = 3;
constexpr status_t ExpectedStatus = STATUS_OK;
#endif
proc::tasks<TaskCount> supervisor;
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    auto pci = sys::pci::Bus::open(info, 0x3100'0000);
    if (!pci) exit(pci.error());
    sys::require(pci->configure(0x1042'1af4));
    auto selected = pci->find(0x1042'1af4, 0);
    if (!selected) sys::exit(selected.error());
    auto mapping = mapping::map(sys::capability(info, BOOT_VSPACE),
        cap_owner{pci->firmware.console(ObjMem)},
        0x30010000, 4096, VM_READ | VM_WRITE);
    if (!mapping) exit(mapping.error());
    uart::Port port{mapping->address};
    port.reset();
    uart::Printer printer{uart::Writer{port}};
    open(program, info);
    supervisor.open(info);
    cap_owner endpoints[4];
    for (size_t i = 0; i < 2; ++i) {
        const auto pair = channel_create(sys::capability(info, BOOT_POOL),
            i == 0 ? 1 : 8, CHANNEL_MAX_WORDS, 4, 2);
        sys::require(pair.status);
        endpoints[i * 2] = cap_owner{pair.value};
        endpoints[i * 2 + 1] = cap_owner{pair.value2};
    }
    constexpr const char* names[] = {"block", "files", "file-client", "file-client"};
    const proc::cap_src block_caps[]{host(selected->cap.selector(), selected->rid),
        input(boot::Block, endpoints[1].selector(), RIGHT_RECEIVE, 1)};
    const proc::cap_src files_caps[]{input(boot::Block, endpoints[0].selector(), RIGHT_SEND, 1),
        input(boot::Files, endpoints[3].selector(), RIGHT_RECEIVE, 1)};
    proc::task_id ids[TaskCount]{};
    bool finished[TaskCount]{};
    size_t remaining = TaskCount - 2;
    for (size_t i = 0; i < TaskCount; ++i) {
        const proc::cap_src client_caps[]{input(boot::Files, endpoints[2].selector(), RIGHT_SEND, 1)};
        ids[i] = start(supervisor, program, names[i], {
            .caps = i == 0 ? std::span<const proc::cap_src>{block_caps} : i == 1 ? std::span<const proc::cap_src>{files_caps} : std::span<const proc::cap_src>{client_caps},
            .resources = i == 1 ? files_quota : io_quota});
    }
    for (;;) {
        for (size_t i = 0; i < TaskCount; ++i) {
            if (finished[i]) continue;
            const auto observed = supervisor.observe(ids[i]);
            sys::require(observed.status);
            if (observed.value == 0) continue;
            const auto status = wait(supervisor, ids[i]);
            if (i < 2 || status != ExpectedStatus) {
                (void)printer.print<"[file-session] task {} failed status={}\n">(i, status);
                exit(STATUS_INTERNAL);
            }
            finished[i] = true;
            if (--remaining != 0) continue;
            for (size_t j = 0; j < 2; ++j) {
                const auto live = supervisor.observe(ids[j]);
                sys::require(live.status);
                if (live.value != 0) exit(STATUS_INTERNAL);
                (void)stop(supervisor, ids[j]);
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
