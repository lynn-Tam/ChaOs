#include <lib/mgr/mgr.hpp>
#include <lib/fs/fs.hpp>
#include <lib/stream/stream.hpp>
#include <servers/devices.hpp>
#include <servers/fw.hpp>
#include <servers/uart.hpp>
#include <libk/pci.hpp>
#include <charconv>

static void dv_fence() noexcept { asm volatile("fence iorw, iorw" ::: "memory"); }
using dv_pci = pci::cfg<dv_fence>;

// The firmware describes bus addresses; only Phys creates actual authority.
struct dv_bus {
    struct Window { uint64_t child, physical, size, next; uint32_t kind; };
    const libk::fdt& tree;
    uint32_t node;
    cap_owner ecam;
    vm_area config;
    std::vector<Window> windows;
    uint32_t first{}, last{}, mapped{UINT32_MAX};
    static auto open(const rt_env& env, const libk::fdt& tree, uint32_t node) -> std::expected<dv_bus, status_t> {
        const auto reg = tree.reg(node);
        const auto up = tree.parent(node);
        const auto ranges = tree.property(node, "ranges"), buses = tree.property(node, "bus-range");
        if (!reg || !up || !ranges || !buses || buses->size() != 8 || reg->size % (1 << 20)) return std::unexpected(STATUS_BAD_ARGS);
        dv_bus bus{tree, node, {}, {}, {}};
        libk::byte_reader ids{buses->data(), buses->size()};
        if (!ids.read_be(bus.first) || !ids.read_be(bus.last) || bus.first > bus.last || bus.last > 255 ||
            reg->size / (1 << 20) < bus.last - bus.first + 1) return std::unexpected(STATUS_BAD_ARGS);
        libk::byte_reader r{ranges->data(), ranges->size()};
        while (r.remaining()) {
            uint32_t flags;
            uint64_t child, physical, bytes;
            if (!r.read_be(flags) || !libk::fdt::read_cells(r, 2, child) ||
                !libk::fdt::read_cells(r, tree.cell_count(*up, "#address-cells", 2), physical) ||
                !libk::fdt::read_cells(r, 2, bytes)) return std::unexpected(STATUS_BAD_ARGS);
            const auto kind = (flags >> 24) & 3;
            if (kind != 2 && kind != 3) continue;
            const auto span = tree.translate(*up, {physical, bytes});
            if (!span || child > UINT64_MAX - bytes) return std::unexpected(STATUS_BAD_ARGS);
            bus.windows.push_back({child, span->address, bytes, child, kind});
        }
        const auto memory = sys::memory_create_physical(env.alloc(), env.selector(dv_physical), reg->address, reg->size, VM_READ | VM_WRITE);
        if (memory.status != STATUS_OK) return std::unexpected(memory.status);
        bus.ecam = cap_owner{memory.value};
        const auto status = bus.config.open(rt_vm(), 0, 1 << 20);
        if (status != STATUS_OK) return std::unexpected(status);
        return bus;
    }
    auto function(uint16_t rid) -> std::expected<dv_pci, status_t> {
        const auto bus = uint32_t{rid} >> 8;
        if (bus < first || bus > last) return std::unexpected(STATUS_BAD_ARGS);
        if (mapped != bus) {
            auto status = config.detach();
            if (status == STATUS_OK) status = config.attach(ecam.selector(), (bus - first) * 256, 1 << 20, VM_READ | VM_WRITE);
            if (status != STATUS_OK) return std::unexpected(status);
            mapped = bus;
        }
        return dv_pci{config.address + (rid & 255) * 4096};
    }
    auto physical(uint64_t address, uint64_t bytes) const -> std::optional<uint64_t> {
        for (const auto& w : windows)
            if (address >= w.child && address - w.child <= w.size && bytes <= w.size - (address - w.child))
                return w.physical + address - w.child;
        return {};
    }
    auto requester(uint16_t rid) const -> std::optional<uint32_t> {
        const auto map = tree.property(node, "iommu-map");
        if (!map || map->size() % 16) return {};
        const auto masked = rid & tree.number(node, "iommu-map-mask").value_or(UINT32_MAX);
        libk::byte_reader r{map->data(), map->size()};
        while (r.remaining()) {
            uint32_t first, ref, target, count;
            if (!r.read_be(first) || !r.read_be(ref) || !r.read_be(target) || !r.read_be(count)) return {};
            if (masked < first || masked - first >= count) continue;
            const auto iommu = tree.phandle(ref);
            if (!iommu || !tree.matches(*iommu, "riscv,iommu") || target > 0xffffff - (masked - first)) return {};
            return target + masked - first;
        }
        return {};
    }
    auto interrupt(uint16_t rid, uint8_t pin) const -> std::optional<uint32_t> {
        const auto map = tree.property(node, "interrupt-map"), mask = tree.property(node, "interrupt-map-mask");
        if (!map || !mask || mask->size() != 16) return {};
        std::array<uint32_t, 4> bits;
        libk::byte_reader mr{mask->data(), mask->size()}, r{map->data(), map->size()};
        for (auto& bit : bits) if (!mr.read_be(bit)) return {};
        const uint32_t key[]{uint32_t{rid} << 8, 0, 0, pin};
        while (r.remaining()) {
            bool match = true;
            for (size_t n = 0; n < 4; ++n) { uint32_t value; if (!r.read_be(value)) return {}; match &= (value & bits[n]) == (key[n] & bits[n]); }
            uint32_t ref, source;
            if (!r.read_be(ref)) return {};
            const auto intc = tree.phandle(ref);
            if (!intc || !tree.matches(*intc, "sifive,plic-1.0.0") ||
                tree.number(*intc, "#address-cells") != 0 || tree.number(*intc, "#interrupt-cells") != 1 || !r.read_be(source)) return {};
            if (match) return source;
        }
        return {};
    }
};

struct dv_device {
    uint16_t rid;
    uint32_t did, source;
    std::string name;
    cap_owner account, scope, space, irq;
    auto alloc() const noexcept -> CreateAuth { return {account.selector(), scope.selector()}; }
    std::array<vm_area, 6> bars;
    std::array<uint64_t, 6> addresses{};
    dv_device(uint16_t rid, uint32_t did, uint32_t source, std::string name)
        : rid(rid), did(did), source(source), name(std::move(name)) {}
    auto reset(dv_pci cfg) -> status_t {
        cfg.write<uint16_t>(4, 2 | (1 << 10)); // disable bus mastering and INTx
        std::array<std::pair<uintptr_t, size_t>, 6> views;
        for (size_t n = 0; n < 6; ++n) views[n] = {bars[n].address, bars[n].size};
        const auto transport = cfg.virtio(views);
        if (!transport) return STATUS_INVALID_OP;
        const dv_pci common{transport->regs[0].first};
        (void)common.read<uint8_t>(20); // drain prior posted MMIO writes before reset
        common.write<uint8_t>(20, 0);
        const auto deadline = rt_after_ms(1000);
        if (!deadline) return STATUS_INTERNAL;
        while (common.read<uint8_t>(20)) {
            if (rt_admit(*deadline) != STATUS_OK) return STATUS_TIMED_OUT;
            sys::yield();
        }
        // For this transport, acknowledged virtio reset cancels the old queues.
        // IOMMU fencing alone would not establish this device-side fact.
        return STATUS_OK;
    }
    auto prepare(const rt_env& env, dv_bus& bus, cap_t interrupts, cap_t iommu) -> status_t {
        const auto made = sys::account_create(env.alloc(), 12 * 1024 * 1024, 512,
            proc_base_kinds | OBJ_BIT(ObjChannel) | OBJ_BIT(ObjIoSpace) | OBJ_BIT(ObjIrq));
        if (made.status != STATUS_OK) return made.status;
        account = cap_owner{made.value};
        const auto owner = sys::scope_create(env.alloc());
        if (owner.status != STATUS_OK) return owner.status;
        scope = cap_owner{owner.value};
        auto fn = bus.function(rid);
        if (!fn) return fn.error();
        fn->write<uint16_t>(4, 1 << 10);
        const auto probed = fn->bars();
        if (!probed) return STATUS_INVALID_OP;
        for (size_t n = 0; n < 6; ++n) {
            const auto& bar = (*probed)[n];
            if (!bar.size) continue;
            const auto bytes = std::max<uint64_t>(4096, bar.size);
            uint64_t address = bar.pa;
            if (!address) {
                for (auto& w : bus.windows) {
                    if (!bar.wide && w.kind != 2) continue;
                    if (w.next > UINT64_MAX - bytes + 1) continue;
                    const auto aligned = (w.next + bytes - 1) & ~(bytes - 1);
                    if (aligned < w.child || aligned - w.child > w.size || bytes > w.size - (aligned - w.child) ||
                        (!bar.wide && aligned > UINT32_MAX)) continue;
                    address = aligned;
                    w.next = aligned + bytes;
                    break;
                }
            }
            const auto physical = bus.physical(address, bytes);
            if (!address || address % bytes || !physical || *physical % 4096) return STATUS_BAD_ARGS;
            const auto flags = fn->read<uint32_t>(0x10 + n * 4) & 15;
            fn->write<uint32_t>(0x10 + n * 4, static_cast<uint32_t>(address) | flags);
            if (bar.wide) fn->write<uint32_t>(0x14 + n * 4, address >> 32);
            addresses[n] = *physical;
            const auto mem = sys::memory_create_physical(alloc(), env.selector(dv_physical), *physical, bytes, VM_READ | VM_WRITE);
            if (mem.status != STATUS_OK) return mem.status;
            auto mapping = vm_area::map(rt_vm(), cap_owner{mem.value}, 0, bytes, VM_READ | VM_WRITE);
            if (!mapping) return mapping.error();
            bars[n] = std::move(*mapping);
        }
        const auto status = reset(*fn);
        if (status != STATUS_OK) return status;
        const auto interrupt = sys::irq_create(alloc(), interrupts, source);
        if (interrupt.status != STATUS_OK) return interrupt.status;
        irq = cap_owner{interrupt.value};
        const auto domain = sys::io_space_create(alloc(), iommu, did);
        if (domain.status != STATUS_OK) return domain.status;
        space = cap_owner{domain.value};
        for (;;) {
            const auto state = sys::io_space_state(space.selector());
            if (state.status != STATUS_OK) return state.status;
            if (state.value == IO_SPACE_READY) return STATUS_OK;
            if (state.value != IO_SPACE_PENDING) return STATUS_BACKING_FAILED;
            sys::yield();
        }
    }
    auto deploy(const rt_env& env, dv_bus& bus, mgr& owner, mgr_child& driver) -> status_t {
        driver.grants.clear(); driver.owned.clear();
        driver.auth = proc_auth{alloc(), static_cast<uint32_t>(env.cpus), env.domain};
        driver.scope = scope.selector();
        for (size_t n = 0; n < bars.size(); ++n) if (bars[n].memory)
            driver.grants.push_back({.key=dv_regs[n], .bytes=bars[n].size,
                .cap=bars[n].memory.selector(), .limit=word_t{RIGHT_MAP | RIGHT_DUPLICATE}});
        auto config = vm_area::create(env.alloc(), rt_vm(), 0, 4096);
        if (!config) return config.error();
        const auto fn = bus.function(rid);
        if (!fn) return fn.error();
        // Immutable transport metadata, never ECAM authority.
        for (size_t offset = 0; offset < 256; offset += 4)
            *reinterpret_cast<uint32_t*>(config->address + offset) = fn->read<uint32_t>(offset);
        auto status = config->detach();
        if (status == STATUS_OK) status = sys::memory_seal(config->memory.selector()).status;
        if (status != STATUS_OK) return status;
        driver.owned.push_back({dv_regs[6].name, std::move(config->memory), RIGHT_MAP | RIGHT_DUPLICATE, ObjMem, 4096});
        driver.grants.push_back({.key=dv_space, .cap=space.selector(),
            .limit=word_t{RIGHT_MAP | RIGHT_UNMAP | RIGHT_INSPECT | RIGHT_OBSERVE | RIGHT_CLOSE | RIGHT_DUPLICATE}});
        driver.grants.push_back({.key=dv_irq, .cap=irq.selector(),
            .limit=word_t{RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK | RIGHT_DUPLICATE}});
        return owner.start(driver);
    }
    auto retire(dv_pci cfg) -> status_t {
        auto status = io_close(space.selector());
        if (status != STATUS_OK) return status;
        status = reset(cfg);
        if (status != STATUS_OK) return status;
        for (auto& bar : bars) if ((status = bar.close()) != STATUS_OK) return status;
        space = {}; irq = {};
        status = sys::scope_close(scope.selector()).status;
        if (status != STATUS_OK) return status;
        status = sys::wait_one(scope.selector(), STATE_DRAINED).status;
        if (status != STATUS_OK) return status;
        if ((status = scope.close()) != STATUS_OK) return status;
        status = sys::account_close(account.selector()).status;
        if (status != STATUS_OK) return status;
        status = sys::wait_one(account.selector(), STATE_DRAINED).status;
        if (status != STATUS_OK) return status;
        status = sys::object_destroy(account.selector()).status;
        if (status != STATUS_OK) return status;
        return account.close();
    }
};

int main() {
    const auto& env = rt_startup();
    const auto* input = env.find(dv_config);
    if (!input || !input->bytes || input->bytes > 4096) sys::exit(STATUS_BAD_ARGS);
    const auto copied = sys::cap_duplicate(input->cap, env.cspace, RIGHT_MAP);
    if (copied.status != STATUS_OK) sys::exit(copied.status);
    auto boot = vm_area::map(rt_vm(), cap_owner{copied.value}, 0, 4096, VM_READ);
    if (!boot) sys::exit(boot.error());
    const ubus_value config{{reinterpret_cast<const std::byte*>(boot->address), input->bytes}};
    if (!config.valid()) sys::exit(STATUS_BAD_ARGS);
    const auto source = config["firmware"];
    const auto size = source["size"].as<uint64_t>(), hart = source["hart"].as<uint64_t>();
    const auto offset = source["offset"].as<uint32_t>(), format = source["format"].as<uint32_t>();
    if (!size || !offset || !format || !hart) sys::exit(STATUS_BAD_ARGS);
    auto firmware = fw_view::open(env, env.selector(dv_firmware), *size, *offset, *format);
    if (!firmware) sys::exit(firmware.error());
    const auto& tree = firmware->tree;
    auto irq = firmware->irq(env, env.selector(dv_physical), *hart);
    if (!irq && irq.error() != STATUS_NOT_FOUND) sys::exit(irq.error());
    auto console = irq ? fw_console::open(env, tree, irq->selector(), env.selector(dv_physical))
        : std::expected<fw_console, status_t>{std::unexpected(STATUS_NOT_FOUND)};
    if (!console && console.error() != STATUS_NOT_FOUND) sys::exit(console.error());
    if (!console) console = fw_console{};
    cap_owner iommu;
    if (const auto node = tree.find("riscv,iommu"); node && irq) {
        const auto reg = tree.reg(*node);
        const auto interrupts = tree.property(*node, "interrupts");
        const auto plic = tree.find("sifive,plic-1.0.0");
        uint32_t source{};
        libk::byte_reader sources{interrupts ? interrupts->data() : nullptr, interrupts ? interrupts->size() : 0};
        if (!reg || !plic || !sources.read_be(source) ||
            tree.number(*node, "interrupt-parent") != tree.number(*plic, "phandle")) sys::exit(STATUS_BAD_ARGS);
        auto config = vm_area::create(env.alloc(), rt_vm(), 0, 4096);
        if (!config) sys::exit(config.error());
        std::construct_at(reinterpret_cast<IommuConfig*>(config->address), IOMMU_RISCV, 0U,
                         reg->address, reg->size, irq->selector(), source, 24U);
        const auto ctl = sys::iommu_create(env.alloc(), env.selector(dv_physical), config->memory.selector());
        if (ctl.status != STATUS_OK) sys::exit(ctl.status);
        iommu = cap_owner{ctl.value};
        for (;;) {
            const auto state = sys::iommu_state(iommu.selector());
            if (state.status != STATUS_OK || state.value == IO_SPACE_FAILED) sys::exit(STATUS_BACKING_FAILED);
            if (state.value == IO_SPACE_READY) break;
            sys::yield();
        }
    }

    const auto* bundle = env.find(fs_archive);
    if (!bundle) sys::exit(STATUS_INVALID_CAP);
    proc_archive archive;
    auto status = archive.open(bundle->cap, bundle->bytes);
    if (status != STATUS_OK) sys::exit(status);
    mgr owner{[&](auto name) { return archive.image(name); }};
    if ((status = owner.open()) != STATUS_OK) sys::exit(status);
    if ((status = owner.directory(env.selector(ubus_publisher), env.selector(ubus_directory))) != STATUS_OK) sys::exit(status);
    ubus_conn publisher;
    if ((status = publisher.bind(env.selector(ubus_publisher))) != STATUS_OK) sys::exit(status);
    if (console->memory) {
        auto& uart = owner.add("uart", "uart", {.bytes=1024 * 1024, .caps=128,
            .kinds=proc_base_kinds | OBJ_BIT(ObjChannel)});
        uart.await_ready = false;
        uart.grants = {
            {.key=uart_mem, .cap=console->memory.selector(), .limit=word_t{RIGHT_MAP}},
            {.key=uart_irq, .cap=console->irq.selector(), .limit=word_t{RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK}},
            {.key=rt_stdin, .cap=env.selector(rt_stdin), .limit=word_t{RIGHT_RECEIVE}},
            {.key=rt_stdout, .cap=env.selector(rt_stdout), .limit=word_t{RIGHT_SEND}}
        };
        char offset[24]; const auto encoded = std::to_chars(offset, offset + sizeof(offset), console->offset);
        uart.args.emplace_back(offset, encoded.ptr);
        if ((status = owner.start(uart)) != STATUS_OK) sys::exit(status);
    }
    const auto pci = tree.find("pci-host-ecam-generic");
    std::optional<dv_bus> bus;
    std::optional<dv_device> device;
    if (pci && iommu.selector()) {
        auto opened = dv_bus::open(env, tree, *pci);
        if (!opened) sys::exit(opened.error());
        bus.emplace(std::move(*opened));
        // bus-range bounds config access; only the root and configured bridges
        // establish reachable buses. Do not map every possible ECAM bus.
        std::vector<uint32_t> buses{bus->first};
        for (size_t at = 0; at < buses.size(); ++at) {
            const auto id = buses[at];
            for (uint32_t slot = 0; slot < 32; ++slot) {
                uint32_t functions = 1;
                for (uint32_t n = 0; n < functions; ++n) {
                    const auto rid = static_cast<uint16_t>((id << 8) | (slot << 3) | n);
                    const auto fn = bus->function(rid);
                    if (!fn) sys::exit(fn.error());
                    const auto identity = fn->read<uint32_t>(0);
                    if (identity == UINT32_MAX || !(identity & 0xffff)) continue;
                    if (!n && (fn->read<uint8_t>(14) & 0x80)) functions = 8;
                    fn->write<uint16_t>(4, (fn->read<uint16_t>(4) & ~uint16_t{4}) | (1 << 10));
                    if ((fn->read<uint32_t>(8) >> 16) == 0x0604) {
                        const auto secondary = fn->read<uint8_t>(0x19), subordinate = fn->read<uint8_t>(0x1a);
                        if (secondary && secondary >= bus->first && secondary <= subordinate && subordinate <= bus->last &&
                            std::ranges::find(buses, secondary) == buses.end()) buses.push_back(secondary);
                    }
                    if (identity != 0x1042'1af4 || device) continue;
                    const auto requester = bus->requester(rid), irq = bus->interrupt(rid, fn->read<uint8_t>(0x3d));
                    if (!requester || !irq) continue;
                    char name[24] = "block-";
                    const auto text = std::to_chars(name + 6, name + sizeof(name), rid, 16);
                    device.emplace(rid, *requester, *irq, std::string{name, text.ptr});
                }
            }
        }
    }

    mgr_child* driver{};
    if (device) {
        driver = &owner.add(device->name, "block", {.bytes=8 * 1024 * 1024, .caps=256,
            .kinds=proc_base_kinds | OBJ_BIT(ObjChannel), .slots=256, .pages=36});
        // Hardware capabilities remain with this owner through driver drain.
        status = device->prepare(env, *bus, irq->selector(), iommu.selector());
        if (status == STATUS_OK) status = device->deploy(env, *bus, owner, *driver);
        if (status != STATUS_OK) sys::exit(status);
    }
    for (const auto& [name, rights] : std::array<std::pair<std::string_view, word_t>, 2>{{
        {"control", mgr_observe | mgr_control}, {"view", mgr_observe}}})
        if (const auto published = owner.object().publish(publisher, name, rights); !published) sys::exit(published.error());
    bool ready{}, quiesced{};
    uint64_t published_generation{};
    for (;;) {
        if (driver && driver->task && !quiesced) {
            const auto ended = driver->task->observe();
            if (ended.status != STATUS_OK) return ended.status;
            if (ended.value || driver->state == "draining") {
                if ((status = driver->task->stop()) != STATUS_OK) sys::exit(status);
                if ((status = io_close(device->space.selector())) != STATUS_OK) sys::exit(status);
                const auto fn = bus->function(device->rid);
                if (!fn) sys::exit(fn.error());
                if ((status = device->reset(*fn)) != STATUS_OK) sys::exit(status);
                quiesced = true;
            }
        }
        if ((status = owner.poll()) != STATUS_OK) sys::exit(status);
        if (driver && !driver->task) {
            const auto fn = bus->function(device->rid);
            if (!fn) sys::exit(fn.error());
            driver->grants.clear(); driver->owned.clear();
            if (device->account && (status = device->retire(*fn)) != STATUS_OK) sys::exit(status);
            if (driver->desired) {
                if ((status = device->prepare(env, *bus, irq->selector(), iommu.selector())) != STATUS_OK) sys::exit(status);
                if ((status = device->deploy(env, *bus, owner, *driver)) != STATUS_OK) sys::exit(status);
                quiesced = false; continue;
            }
        }
        if (driver && driver->state == "running" && published_generation != driver->gen) {
            const auto fn = bus->function(device->rid);
            if (!fn) sys::exit(fn.error());
            fn->write<uint16_t>(4, 6); (void)fn->read<uint16_t>(4);
            if ((status = mgr_role(publisher, "block", device->name + "/block")) != STATUS_OK) sys::exit(status);
            published_generation = driver->gen;
        }
        if (!ready && (!driver || driver->state == "running")) {
            if ((status = sys::notification_signal(env.selector(ubus_ready)).status) != STATUS_OK) sys::exit(status);
            ready = true;
        }
        if ((status = ev_wait(env.events, owner.deadline()).status) != STATUS_OK && status != STATUS_TIMED_OUT) sys::exit(status);
    }
}
