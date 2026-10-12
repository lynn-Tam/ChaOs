#include <util.hpp>
#include <proto.hpp>
#include <servers/pci.hpp>
#include <utility>
#include <ch.hpp>
#include <cap.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>

pr_pkg program;
#ifdef TEST_FILE_FAILURE
constexpr size_t TaskCount = 4;
constexpr status_t ExpectedStatus = STATUS_PEER_FAULT;
#else
constexpr size_t TaskCount = 3;
constexpr status_t ExpectedStatus = STATUS_OK;
#endif
static pr_tasks supervisor;

int main() {

    const auto& info = rt_startup();
    auto pci = pci_bus::open(info, 0x3100'0000);
    if (!pci)
        sys::exit(pci.error());
    if (const auto rc = pci->configure(0x1042'1af4); rc != STATUS_OK)
        sys::exit(rc);
    auto selected = pci->find(0x1042'1af4, 0);
    if (!selected)
        sys::exit(selected.error());
    auto vm_area =
        vm_area::map(info.vspace, cap_owner{pci->firmware.console(ObjMem)},
                     0x30010000, 4096, VM_READ | VM_WRITE);
    if (!vm_area)
        sys::exit(vm_area.error());
    uart_port port{vm_area->address};
    port.reset();
    open(program, info);
    supervisor.open(info.pool, fw_root(info).domain, info.vspace, info.cpus);
    cap_owner endpoints[4];
    for (size_t i = 0; i < 2; ++i) {
        const auto pair =
            sys::channel_create(info.pool, i == 0 ? 1 : 8, CHANNEL_MAX_WORDS, 4, 2);
        if (pair.status != STATUS_OK) sys::exit(pair.status);
        endpoints[i * 2] = cap_owner{pair.value};
        endpoints[i * 2 + 1] = cap_owner{pair.value2};
    }
    constexpr const char* names[] = {"block", "files", "file-client", "file-client"};
    const pr_cap block_caps[]{host(selected->cap.selector(), selected->rid),
                              input(blk_service, endpoints[1].selector(), RIGHT_RECEIVE, 1)};
    const pr_cap files_caps[]{input(blk_service, endpoints[0].selector(), RIGHT_SEND, 1),
                              input(fs_service, endpoints[3].selector(), RIGHT_RECEIVE, 1)};
    pr_id ids[TaskCount]{};
    bool finished[TaskCount]{};
    size_t remaining = TaskCount - 2;
    for (size_t i = 0; i < TaskCount; ++i) {
        const pr_cap client_caps[]{input(fs_service, endpoints[2].selector(), RIGHT_SEND, 1)};
        ids[i] = start(supervisor, program, names[i],
                       {.caps = i == 0   ? std::span<const pr_cap>{block_caps}
                                : i == 1 ? std::span<const pr_cap>{files_caps}
                                         : std::span<const pr_cap>{client_caps},
                        .resources = i == 1 ? files_quota : io_quota});
    }
    for (;;) {
        for (size_t i = 0; i < TaskCount; ++i) {
            if (finished[i])
                continue;
            const auto observed = supervisor.observe(ids[i]);
            if (observed.status != STATUS_OK) sys::exit(observed.status);
            if (observed.value == 0)
                continue;
            const auto status = wait(supervisor, ids[i]);
            if (i < 2 || status != ExpectedStatus) {
                (void) libk::fmt::format_to<"[file-session] task {} failed status={}\n">(port, i, status);
                sys::exit(STATUS_INTERNAL);
            }
            finished[i] = true;
            if (--remaining != 0)
                continue;
            for (size_t j = 0; j < 2; ++j) {
                const auto live = supervisor.observe(ids[j]);
                if (live.status != STATUS_OK) sys::exit(live.status);
                if (live.value != 0)
                    sys::exit(STATUS_INTERNAL);
                (void) stop(supervisor, ids[j]);
            }
#ifdef TEST_FILE_FAILURE
            port.write("[file-failure] ok: read error, two faulted mappings, closed sessions, "
                       "services live\n");
#else
            port.write("[file-session] ok: two sessions, byte reads, 64 outstanding, EOF, handles, "
                       "close\n");
#endif
            sys::exit();
        }
        sys::yield();
    }
}
