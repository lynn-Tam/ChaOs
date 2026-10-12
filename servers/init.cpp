#include <sys/protocol.hpp>
#include <sys/pci.hpp>
#include <sys/task.hpp>
#include <sys/clock.hpp>
#include <sys/channel.hpp>
#include <servers/uart.hpp>

using sys::cap_owner;
using sys::require;

// These are root's system policy, not a loader manifest or kernel object graph.
// The package selects which services are present; capabilities are wired here.
using Link = sys::Channel;
struct Unit {
    std::string_view name;
    proc::quota quota;
    bool ready{};
    bool wanted{true};
    std::optional<proc::task_id> task{};
    Link links[2]{};
    cap_owner ready_event{};
};
static constexpr auto bit(size_t i) -> uint32_t { return uint32_t{1} << i; }
static std::array<Unit, 8> units{{
    {.name = "uart", .quota = {.caps = 128, .kinds = proc::base_kinds | OBJ_BIT(ObjChannel)}},
    {.name = "block", .quota = {.bytes = 4 * 1024 * 1024, .caps = 128, .kinds = proc::base_kinds | OBJ_BIT(ObjIoSpace)}},
    {.name = "files", .quota = {.bytes = 16 * 1024 * 1024, .caps = 1024,
        .kinds = proc::base_kinds | OBJ_BIT(ObjPager) | OBJ_BIT(ObjChannel), .slots = 1024, .pages = 132}},
    {.name = "block_data", .quota = {.bytes = 4 * 1024 * 1024, .caps = 128, .kinds = proc::base_kinds | OBJ_BIT(ObjIoSpace)}},
    {.name = "store", .quota = {.bytes = 8 * 1024 * 1024, .caps = 256,
        .kinds = proc::base_kinds | OBJ_BIT(ObjChannel), .slots = 256, .pages = 36}, .ready = true},
    {.name = "vfs", .quota = {.bytes = 4 * 1024 * 1024, .caps = 256,
        .kinds = proc::base_kinds | OBJ_BIT(ObjChannel), .slots = 256, .pages = 36}, .ready = true},
    {.name = "proc", .quota = {.bytes = 32 * 1024 * 1024, .caps = 1024,
        .kinds = proc::base_kinds | OBJ_BIT(ObjChannel), .slots = 512, .pages = 68,
        .pool_perms = RIGHT_CREATE | RIGHT_SPLIT}},
    {.name = "shell", .quota = {.bytes = 2 * 1024 * 1024, .caps = 128,
        .kinds = proc::base_kinds | OBJ_BIT(ObjChannel)}}
}};
// A connection is also a dependency; restart ordering derives from these
// same edges instead of maintaining a second graph beside capability wiring.
struct Edge {
    size_t from, to;
    boot::Import contract;
    word_t badge{1}, rights{RIGHT_SEND};
    size_t link{}, side{};
};
static constexpr Edge edges[]{
    {1, 2, boot::Block}, {3, 4, boot::Block},
    {2, 5, boot::Files}, {4, 5, boot::Store, 2},
    {2, 6, boot::Files, 3, RIGHT_SEND | RIGHT_DUPLICATE},
    {5, 6, boot::Vfs, 2, RIGHT_SEND | RIGHT_DUPLICATE},
    {5, 6, boot::VfsRead, 1, RIGHT_SEND | RIGHT_DUPLICATE},
    {4, 6, boot::StoreAdmin, 3, RIGHT_SEND | RIGHT_DUPLICATE},
            {6, 7, boot::Process, 1, RIGHT_SEND | RIGHT_RECEIVE},
    {0, 7, boot::ConsoleOutput, 2, RIGHT_SEND | RIGHT_DUPLICATE},
    {0, 7, boot::ConsoleInput, 1, RIGHT_RECEIVE | RIGHT_DUPLICATE, 1, 1},
};
static uint32_t available{};
static sys::Clock clock;
static auto deps(size_t i) -> uint32_t {
    uint32_t mask{};
    for (const auto& e : edges) if (e.to == i) mask |= bit(e.from);
    return mask & available;
}
static proc::package package;
static proc::tasks<units.size()> tasks;
static std::array<sys::pci::Binding, 2> devices;

static void report(const boot::BootView& info, const sys::Firmware& fw, status_t status = STATUS_OK,
                   std::string_view name = {}) {
    constexpr auto address = 0x30010000;
    const auto region = sys::vm_slice(info.selector(BOOT_VSPACE), address, 4096,
        VM_READ | VM_WRITE, RIGHT_MAP | RIGHT_UNMAP | RIGHT_DESTROY);
    require(region.status);
    require(sys::vm_map(region.value, fw.console(ObjMem), address, 4096, 0, VM_READ | VM_WRITE).status);
    sys::uart::Port port{address};
    if (status == STATUS_OK) { port.reset(); port.write("init: native services\n"); }
    else {
        (void)libk::fmt::format_to<"init: service startup failed: {} task=">(port, status);
        port.write(name.data(), name.size()); port.write("\n");
    }
    require(sys::vm_unmap(region.value, address, 4096).status);
    require(sys::vm_clear(region.value).status);
    require(sys::cap_close(region.value).status);
}
static auto cap(boot::Import contract, cap_t source, word_t rights, word_t side = 0,
                word_t badge = 1) -> proc::cap_src {
    return {.contract = contract, .cap = source, .limit = CapView{
        .version = CAP_ATTENUATION_VERSION_CURRENT, .kind = contract.kind,
        .size = CAP_ATTENUATION_SIZE, .rights = rights, .words = {side}}, .badge = badge};
}
static auto fixed(uint32_t role, cap_t source, word_t rights,
                  word_t first = 0, word_t count = 0, word_t perms = 0) -> proc::cap_src {
    return {.role = role, .cap = source, .limit = CapView{
        .version = CAP_ATTENUATION_VERSION_CURRENT, .kind = boot_kind(role),
        .size = CAP_ATTENUATION_SIZE, .rights = rights, .words = {first, count, perms}}};
}
// One source spelling and direct capability input; no internal object keys.
static auto start(size_t i, const boot::BootView& info, const sys::Firmware& fw,
                  cap_t events, cap_t control) -> status_t {
    auto& u = units[i];
    const auto pool = info.selector(BOOT_POOL);
    auto s = i == 7 ? STATUS_OK : u.links[0].open(pool,
        i == 0 || i == 6 ? 16 : 4, i == 0 ? 0 : i == 6 ? 3 : 4, i == 6 ? 3 : 2);
    if (s != STATUS_OK) return s;
    if (i == 0) { s = u.links[1].open(pool, 16, 0); if (s != STATUS_OK) return s; }
    if (u.ready) {
        const auto created = sys::notification_create(pool, 1);
        if (created.status != STATUS_OK) return created.status;
        u.ready_event = cap_owner{created.value};
    }
    std::array<proc::cap_src, 12> sources{};
    size_t n = 0;
    const auto put = [&](proc::cap_src value) { libk_assert(n < sources.size()); sources[n++] = value; };
    for (const auto& e : edges) if (e.to == i && (available & bit(e.from)))
        put(cap(e.contract, units[e.from].links[e.link].ends[e.side].selector(),
                e.rights, e.side, e.badge));
    boot::args args;
    switch (i) {
    case 0: {
        auto mem = cap(boot::UartMem, fw.console(ObjMem), RIGHT_MAP, 0, 0);
        std::get<CapView>(mem.limit).words[1] = 1; std::get<CapView>(mem.limit).words[2] = VM_READ | VM_WRITE; put(mem);
        put(cap(boot::UartIrq, fw.console(ObjIrq), RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK, 0, 0));
        put(cap(boot::ConsoleOutput, u.links[0].ends[1].selector(), RIGHT_RECEIVE, 1));
        put(cap(boot::ConsoleInput, u.links[1].ends[0].selector(), RIGHT_SEND));
        break;
    }
    case 1: case 3:
        put(fixed(BOOT_IO_HOST, devices[i == 1 ? 0 : 1].cap.selector(), RIGHT_CONNECT,
            devices[i == 1 ? 0 : 1].rid, 1));
        put(cap(boot::Block, u.links[0].ends[1].selector(), RIGHT_RECEIVE, 1));
        break;
    case 2:
        put(cap(boot::Files, u.links[0].ends[1].selector(), RIGHT_RECEIVE, 1));
        break;
    case 4:
        put(cap(boot::Store, u.links[0].ends[1].selector(), RIGHT_RECEIVE, 1));
        break;
    case 5:
        put(cap(boot::Vfs, u.links[0].ends[1].selector(), RIGHT_RECEIVE, 1));
        break;
    case 6:
        put(fixed(BOOT_DOMAIN, info.selector(BOOT_DOMAIN), RIGHT_DUPLICATE | RIGHT_CONTROL));
        put(cap(boot::Process, u.links[0].ends[1].selector(), RIGHT_SEND | RIGHT_RECEIVE, 1));
        break;
    case 7:
        put(cap(boot::ServiceControl, control, RIGHT_SEND | RIGHT_RECEIVE, 1));
        if (units[4].wanted && !args.append("storage", 7)) return STATUS_BAD_ARGS;
        break;
    }
    if (u.ready) {
        put(fixed(BOOT_READY, u.ready_event.selector(), RIGHT_SIGNAL));
        put(cap(boot::ServiceWake, events, RIGHT_SIGNAL, 0, 0));
    }
    const auto spawned = tasks.spawn(package, u.name, {.args = &args, .caps = {sources.data(), n},
        .resources = u.quota, .sched = {.budget = 600'000}, .events = events, .close_badge = word_t{1} << (i + 1)});
    if (spawned.id.valid()) u.task = spawned.id;
    if (spawned.status != STATUS_OK || !u.ready) return spawned.status;
    const auto deadline = clock.after_ms(10'000);
    if (!deadline) return STATUS_BAD_ARGS;
    for (;;) {
        const auto ended = tasks.observe(*u.task);
        if (ended.status != STATUS_OK) return ended.status;
        if (ended.value) return ended.value2 ? static_cast<status_t>(ended.value2) : STATUS_PEER_FAULT;
        const auto ready = sys::notification_take(u.ready_event.selector());
        if (ready.status == STATUS_OK) return STATUS_OK;
        if (ready.status != STATUS_RETRY) return ready.status;
        const auto wake = sys::notification_wait(events, *deadline);
        if (wake.status != STATUS_OK) return wake.status;
        for (auto& unit : units) if (unit.task) tasks.notify(*unit.task, wake.value);
    }
}
static auto affected(uint32_t mask) -> uint32_t {
    for (;;) {
        const auto old = mask;
        // The interactive process session is owned by shell's lifetime.
        if (mask & bit(7)) mask |= bit(6);
        for (size_t i = 0; i < units.size(); ++i) if (deps(i) & mask) mask |= bit(i);
        if (old == mask) return mask;
    }
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    const auto info = sys::bootstrap(address, size);
    require(clock.open());
    auto pci = sys::pci::Bus::open(info, 0x31000000);
    if (!pci) sys::exit(pci.error());
    require(pci->configure(0x10421af4));
    report(info, pci->firmware);
    tasks.open(info);
    require(package.open(info.selector(BOOT_VSPACE), info.selector(BOOT_BUNDLE),
        {0x10000000, proc::window::round_size(info.bundle_size())}, info.bundle_size()));
    for (size_t i = 0; i < units.size(); ++i) {
        units[i].wanted = bool(package.find(units[i].name));
        if (units[i].wanted) available |= bit(i);
    }
    for (size_t i = 0; i < devices.size() && i < pci->count(0x10421af4); ++i) {
        auto dev = pci->find(0x10421af4, i);
        if (!dev) sys::exit(dev.error());
        devices[i] = std::move(*dev);
    }
    const auto notification = sys::notification_create(info.selector(BOOT_POOL), 1);
    require(notification.status);
    cap_owner events{notification.value};
    Link control;
    require(control.open(info.selector(BOOT_POOL), 4, 1, 1));
    const auto receiver = sys::channel_mint(control.ends[0].selector(), info.selector(BOOT_CSPACE),
        1, RIGHT_RECEIVE | RIGHT_SEND);
    require(receiver.status);
    cap_owner control_rx{receiver.value};
    sys::Port channel{control_rx.selector(), events.selector()};
    uint32_t recovering = available;
    uint64_t retry_at = 0;
    for (;;) {
        for (size_t i = 0; i < units.size(); ++i) if (units[i].task && !(recovering & bit(i))) {
            const auto ended = tasks.observe(*units[i].task);
            require(ended.status);
            if (ended.value) recovering |= affected(bit(i));
        }
        // Start every affected drain before waiting: capability copies in a
        // sibling can still carry quota charged to the stopped process.
        // Provider channels reset only after the whole group has drained.
        bool pending = false, poll = false;
        for (size_t i = units.size(); i-- > 0;) if ((recovering & bit(i)) && units[i].task) {
            require(tasks.request_stop(*units[i].task));
            const auto collected = tasks.collect(*units[i].task);
            if (collected.status == STATUS_OK) units[i].task.reset();
            else if (proc::retryable(collected.status) || collected.status == STATUS_WOULD_BLOCK) {
                pending = true; poll |= tasks.closing_needs_poll(*units[i].task);
            } else sys::exit(collected.status);
        }
        if (recovering && !pending) {
            const auto now = sys::clock_now(); require(now.status);
            if (!retry_at || now.value >= retry_at) {
                for (size_t i = 0; i < units.size(); ++i) if (recovering & bit(i)) {
                    auto& u = units[i];
                    if (!u.wanted) { recovering &= ~bit(i); continue; }
                    const auto started = start(i, info, pci->firmware, events.selector(), control.ends[1].selector());
                    if (started != STATUS_OK) {
                        report(info, pci->firmware, started, u.name);
                        recovering |= affected(bit(i));
                        const auto deadline = clock.after_ms(100);
                        if (!deadline) sys::exit(STATUS_INTERNAL);
                        retry_at = *deadline;
                        break;
                    }
                    recovering &= ~bit(i);
                }
                if (!recovering) retry_at = 0;
            }
        }
        for (;;) {
            sys::Message request;
            cap_owner reply;
            const auto received = channel.try_receive(request, reply);
            if (received.status == STATUS_WOULD_BLOCK) break;
            if (received.status == STATUS_BAD_ARGS || received.status == STATUS_TRANSFER_FAILED) continue;
            require(received.status);
            status_t status = STATUS_NOT_FOUND;
            for (size_t i = 0; i < units.size(); ++i) if (units[i].name == std::string_view{request.data, request.size}) {
                const auto group = affected(bit(i));
                // 0=restart (existing shell), 1=stop, 2=start.
                if (request.operation > 2) { status = STATUS_BAD_ARGS; break; }
                if (request.operation == 1) {
                    for (size_t j = 0; j < units.size(); ++j) if (group & bit(j)) units[j].wanted = false;
                } else {
                    units[i].wanted = true;
                    for (size_t j = units.size(); j-- > 0;)
                        if (units[j].wanted) for (size_t k = 0; k < j; ++k)
                            if (deps(j) & bit(k)) units[k].wanted = true;
                    for (size_t j = 0; j < units.size(); ++j)
                        if (units[j].wanted && !units[j].task) recovering |= bit(j);
                }
                recovering |= group;
                status = STATUS_OK; break;
            }
            (void)sys::send(reply.selector(), {.status = status}, false);
        }
        require(channel.arm().status);
        if (pending && poll) { sys::yield(); continue; }
        if (recovering && !pending && !retry_at) continue;
        const auto wake = sys::notification_wait(events.selector(), retry_at);
        if (wake.status != STATUS_OK && wake.status != STATUS_TIMED_OUT) sys::exit(wake.status);
        if (wake.status == STATUS_OK) for (auto& u : units) if (u.task) tasks.notify(*u.task, wake.value);
    }
}
