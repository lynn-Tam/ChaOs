#include <mgr/mgr.hpp>
#include "fw.hpp"
#include <libk/pci.hpp>
#include <charconv>

static cap_t dv_phys;
struct dv_record {
    std::string id, name, type, location;
    std::shared_ptr<mgr_child> driver;
};

static auto grant(ubus_msg& message, std::string_view name, cap_t source, word_t rights, uint64_t bytes = 0) -> status_t {
    const auto duplicated = sys::cap_duplicate(source, rt_startup().cspace, rights);
    if (duplicated.status != STATUS_OK) return duplicated.status;
    const auto index = message.offer(cap_owner{duplicated.value}, rights); if (!index) return index.error();
    if (bytes) message.data.put(name, ubus_buffer{*index, 0, bytes}); else message.data.cap(name, *index);
    return STATUS_OK;
}
static auto grant(mgr_child& child, std::string_view name, cap_t source, word_t rights, uint64_t bytes = 0) -> status_t {
    auto* message = child.inbox.empty() ? &child.startup : &child.inbox.back();
    if (message->count == CHANNEL_MAX_CAPS - 1) {
        message->data.put("more", true); child.inbox.emplace_back(); message = &child.inbox.back();
    }
    return grant(*message, name, source, rights, bytes);
}

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
        const auto memory = sys::memory_create_physical(env.alloc(), dv_phys, reg->address, reg->size, VM_READ | VM_WRITE);
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
    cap_owner quota, scope, space, irq;
    std::shared_ptr<mgr_child> driver;
    uint64_t published_run{}, publication{};
    bool quiesced{}, reset_ready{};
    auto alloc() const noexcept -> CreateAuth { return {quota.selector(), scope.selector()}; }
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
    auto prepare(const rt_env& env, dv_bus& bus, cap_t interrupts, cap_t iommu, mgr_child& driver) -> status_t {
        const auto boundary = sys::scope_create(env.alloc());
        if (boundary.status != STATUS_OK) return boundary.status;
        driver.scope = cap_owner{boundary.value};
        const CreateAuth deployment{env.quota, driver.scope.selector()};
        const auto made = sys::quota_create(deployment, 12 * 1024 * 1024, 512,
            proc_base_kinds | OBJ_BIT(ObjIoSpace) | OBJ_BIT(ObjIrq));
        if (made.status != STATUS_OK) return made.status;
        quota = cap_owner{made.value};
        const auto owner = sys::scope_create(deployment);
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
            const auto mem = sys::memory_create_physical(alloc(), dv_phys, *physical, bytes, VM_READ | VM_WRITE);
            if (mem.status != STATUS_OK) return mem.status;
            auto mapping = vm_area::map(rt_vm(), cap_owner{mem.value}, 0, bytes, VM_READ | VM_WRITE);
            if (!mapping) return mapping.error();
            bars[n] = std::move(*mapping);
        }
        const auto status = reset(*fn);
        if (status != STATUS_OK) return status;
        reset_ready = true;
        const auto interrupt = sys::irq_create(alloc(), interrupts, source);
        if (interrupt.status != STATUS_OK) return interrupt.status;
        irq = cap_owner{interrupt.value};
        const auto domain = sys::io_space_create(alloc(), iommu, did);
        if (domain.status != STATUS_OK) return domain.status;
        space = cap_owner{domain.value};
        const auto deadline = rt_after_ms(5000);
        if (!deadline) return STATUS_BAD_ARGS;
        const auto ready = sys::wait_one(space.selector(), (word_t{1} << IO_SPACE_READY) |
            (word_t{1} << IO_SPACE_FAILED) | (word_t{1} << IO_SPACE_CLOSED), 0, *deadline);
        if (ready.status != STATUS_OK) return ready.status;
        const auto state = sys::io_space_state(space.selector());
        return state.status != STATUS_OK ? state.status : state.value == IO_SPACE_READY ? STATUS_OK : STATUS_BACKING_FAILED;
    }
    auto handoff(const rt_env& env, dv_bus& bus, mgr_child& driver) -> status_t {
        driver.startup = {}; driver.inbox.clear();
        driver.auth = proc_auth{alloc(), static_cast<uint32_t>(env.cpus), env.domain};
        for (size_t n = 0; n < bars.size(); ++n) if (bars[n].memory) {
            const auto status = grant(driver, "bar" + std::to_string(n), bars[n].memory.selector(), RIGHT_MAP | RIGHT_DUPLICATE, bars[n].size);
            if (status != STATUS_OK) return status;
        }
        auto config = vm_area::create(env.alloc(), rt_vm(), 0, 4096);
        if (!config) return config.error();
        const auto fn = bus.function(rid); if (!fn) return fn.error();
        for (size_t offset = 0; offset < 256; offset += 4)
            *reinterpret_cast<uint32_t*>(config->address + offset) = fn->read<uint32_t>(offset);
        auto status = config->detach();
        if (status == STATUS_OK) status = sys::memory_seal(config->memory.selector()).status;
        if (status == STATUS_OK) status = grant(driver, "pci.config", config->memory.selector(), RIGHT_MAP | RIGHT_DUPLICATE, 4096);
        if (status == STATUS_OK) status = grant(driver, "dma", space.selector(), RIGHT_MAP | RIGHT_UNMAP | RIGHT_INSPECT | RIGHT_OBSERVE | RIGHT_CLOSE | RIGHT_DUPLICATE);
        if (status == STATUS_OK) status = grant(driver, "irq", irq.selector(), RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK | RIGHT_DUPLICATE);
        return status;
    }
    auto retire(dv_pci cfg) -> status_t {
        auto status = space ? io_close(space.selector()) : STATUS_OK;
        if (status == STATUS_OK && reset_ready) status = reset(cfg);
        if (status != STATUS_OK) return status;
        // Before the first acknowledged reset, enumeration/prepare has kept
        // bus mastering disabled. Partial admission has never submitted DMA.
        for (auto& bar : bars) if ((status = bar.close()) != STATUS_OK) return status;
        space = {}; irq = {}; reset_ready = false;
        if (scope) {
            status = sys::scope_close(scope.selector()).status;
            if (status == STATUS_OK) status = sys::wait_one(scope.selector(), STATE_DRAINED).status;
            if (status != STATUS_OK) return status;
            if ((status = scope.close()) != STATUS_OK) return status;
        }
        if (quota) {
            status = sys::quota_close(quota.selector()).status;
            if (status == STATUS_OK) status = sys::wait_one(quota.selector(), STATE_DRAINED).status;
            if (status != STATUS_OK) return status;
            status = sys::object_destroy(quota.selector()).status;
            if (status != STATUS_OK) return status;
            return quota.close();
        }
        return STATUS_OK;
    }
};

int main() {
    const auto& env = rt_startup();
    ubus_conn parent;
    auto status = parent.bind(env.environment); if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = parent.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    const auto message = setup.data["message"];
    const auto buffer = message["firmware"].as<ubus_buffer>();
    const auto hart = message["hart"].as<uint64_t>();
    const auto format = message["format"].as<uint32_t>();
    dv_phys = setup.capability(message["physical"]);
    if (!buffer || !hart || !format || !dv_phys) return STATUS_BAD_ARGS;
    auto firmware = fw_view::open(env, setup.capability(message["firmware"]), buffer->size, buffer->offset, *format);
    if (!firmware) return firmware.error();
    const auto& tree = firmware->tree;
    auto irq = firmware->irq(env, dv_phys, *hart);
    if (!irq && irq.error() != STATUS_NOT_FOUND) sys::exit(irq.error());
    auto console = irq ? fw_console::open(env, tree, irq->selector(), dv_phys)
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
        const auto ctl = sys::iommu_create(env.alloc(), dv_phys, config->memory.selector());
        if (ctl.status != STATUS_OK) sys::exit(ctl.status);
        iommu = cap_owner{ctl.value};
        const auto deadline = rt_after_ms(5000);
        if (!deadline) return STATUS_BAD_ARGS;
        const auto done = sys::wait_one(iommu.selector(), OP_DONE, 0, *deadline, 0, SOURCE_OP);
        if (done.status != STATUS_OK) return done.status;
        const auto state = sys::iommu_state(iommu.selector());
        if (state.status != STATUS_OK || state.value != IO_SPACE_READY) return STATUS_BACKING_FAILED;
    }

    const auto bundle = message["archive"].as<ubus_buffer>(); if (!bundle || bundle->offset) return STATUS_BAD_ARGS;
    proc_archive archive;
    status = archive.open(setup.capability(message["archive"]), bundle->size); if (status != STATUS_OK) return status;
    std::optional<dv_bus> bus;
    std::vector<std::unique_ptr<dv_device>> devices;
    std::vector<dv_record> records;
    mgr owner{[&](mgr_child& child) -> std::expected<proc_image, status_t> {
        for (auto& device : devices) if (&child == device->driver.get() && !device->quota) {
            auto status = device->prepare(env, *bus, irq->selector(), iommu.selector(), child);
            if (status == STATUS_OK) status = device->handoff(env, *bus, child);
            if (status != STATUS_OK) return std::unexpected(status);
            device->quiesced = false;
        }
        return archive.image(child.program());
    }};
    status = owner.open(); if (status != STATUS_OK) return status;
    if (console->memory) {
        auto recipe = mgr_recipe("uart", "/boot/uart.elf", {.bytes=1024 * 1024, .caps=128});
        recipe.data.put("restart", true);
        ubus_msg payload;
        status = grant(payload, "registers", console->memory.selector(), RIGHT_MAP | RIGHT_DUPLICATE, 4096);
        if (status == STATUS_OK) status = grant(payload, "irq", console->irq.selector(), RIGHT_ROUTE | RIGHT_OBSERVE | RIGHT_ACK | RIGHT_DUPLICATE);
        if (status == STATUS_OK) status = grant(payload, "stdin", setup.capability(message["stdin"]), RIGHT_RECEIVE | RIGHT_DUPLICATE);
        if (status == STATUS_OK) status = grant(payload, "stdout", setup.capability(message["stdout"]), RIGHT_SEND | RIGHT_DUPLICATE);
        payload.data.put("offset", console->offset);
        recipe.data.put("setup", payload.data); payload.data = std::move(recipe.data);
        auto uart = owner.configure(std::move(payload)); if (!uart) return uart.error();
        records.push_back({"uart", "Serial port", "Serial", "Firmware", *uart});
        if (status == STATUS_OK) status = owner.start(**uart);
        if (status != STATUS_OK) return status;
    }
    const auto pci = tree.find("pci-host-ecam-generic");
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
                    char key[24] = "pci-";
                    const auto end = std::to_chars(key + 4, key + sizeof(key), rid, 16);
                    const auto bridge = (fn->read<uint32_t>(8) >> 24) == 0x06;
                    records.push_back({std::string{key, end.ptr}, identity == 0x1042'1af4 ? "Virtio block" : bridge ? "PCI bridge" : "PCI device",
                        identity == 0x1042'1af4 ? "Storage" : bridge ? "Bridge" : "Other",
                        "PCI " + std::to_string(id) + ":" + std::to_string(slot) + "." + std::to_string(n), {}});
                    if (identity != 0x1042'1af4) continue;
                    const auto requester = bus->requester(rid), irq = bus->interrupt(rid, fn->read<uint8_t>(0x3d));
                    if (!requester || !irq) continue;
                    char name[24] = "block-";
                    const auto text = std::to_chars(name + 6, name + sizeof(name), rid, 16);
                    devices.push_back(std::make_unique<dv_device>(rid, *requester, *irq, std::string{name, text.ptr}));
                }
            }
        }
    }

    // Reserve only the discovered hardware's deployment capacity. The parent's
    // capability ceiling authorizes growth; an unused second slot costs nothing.
    const auto quota = cap_info(env.quota); if (!quota) return quota.error();
    ubus_msg capacity, resized;
    capacity.data.put("action", "resize")
        .put("bytes", std::min<uint64_t>((8 + 12 * devices.size()) * 1024 * 1024, quota->view.words[0]))
        .put("caps", std::min<uint64_t>(256 + 512 * devices.size(), quota->view.words[1]));
    status = parent.invoke("Resource", std::move(capacity), resized);
    if (status != STATUS_OK && status != STATUS_NO_MEMORY) return status;
    for (auto& device : devices) {
        auto& driver = device->driver;
        auto recipe = mgr_recipe(device->name, "/boot/block.elf", {.bytes=8 * 1024 * 1024, .caps=256, .slots=256, .pages=36});
        ubus_data exports{ubus_type::Array}; exports.push("block"); recipe.data.put("exports", exports).put("restart", true);
        auto configured = owner.configure(std::move(recipe)); if (!configured) return configured.error();
        driver = *configured;
        for (auto& record : records) {
            char key[24] = "pci-"; const auto end = std::to_chars(key + 4, key + sizeof(key), device->rid, 16);
            if (record.id == std::string_view{key, end.ptr}) record.driver = driver;
        }
        // Hardware capabilities remain with this owner through driver drain.
        // Native admission reports this driver's failure; other devices remain usable.
        (void)owner.start(*driver);
    }
    status = owner.attach(parent); if (status != STATUS_OK) return status;
    ubus_object inventory{"io.devices"};
    status = inventory.add("List", {}, {{"items", ubus_type::Array}}, [&](auto&, ubus_msg& response) -> status_t {
        ubus_data items{ubus_type::Array};
        for (const auto& record : records) {
            ubus_data row;
            std::string_view state = "Detected";
            if (const auto& child = record.driver; child && owner.find(child->child_id)) {
                const auto device = std::ranges::find_if(devices, [&](const auto& item) { return item->driver == child; });
                state = child->state == mgr_state::Failed ? "Failed" : !child->task ? "Stopped"
                    : child->state == mgr_state::Draining ? "Stopping" : device != devices.end()
                    ? (*device)->published_run == child->run_id && !(*device)->quiesced ? "Ready" : "Starting" : "In use";
                const auto program = child->program().substr(child->program().find_last_of('/') + 1);
                row.put("driver", program.ends_with(".elf") ? program.substr(0, program.size() - 4) : program);
                auto problem = child->state == mgr_state::Failed ? child->result : STATUS_OK;
                if (device != devices.end() && child->task && (*device)->space) {
                    const auto io = sys::io_space_state((*device)->space.selector());
                    if (io.status != STATUS_OK || io.value == IO_SPACE_FAILED) { state = "Failed"; problem = STATUS_BACKING_FAILED; }
                    else if (io.value != IO_SPACE_READY) state = io.value == IO_SPACE_PENDING ? "Starting" : "Stopping";
                }
                if (problem) row.put("error", int64_t(problem));
            } else row.put("driver", "--");
            row.put("id", record.id).put("name", record.name).put("type", record.type).put("location", record.location).put("state", state);
            items.push(row);
        }
        response.data.put("items", items); return STATUS_OK;
    });
    if (status == STATUS_OK) status = inventory.open();
    if (status != STATUS_OK) return status;
    auto listing = inventory.connector(); if (!listing) return listing.error();
    ubus_msg announce, accepted;
    const auto peer_idx = announce.attach(listing->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
    if (!peer_idx) return peer_idx.error();
    announce.data.put("name", "inventory").put("interface", "io.devices").put("version", uint64_t{1})
        .cap("connector", *peer_idx);
    status = parent.invoke("Publish", std::move(announce), accepted); if (status != STATUS_OK) return status;
    for (;;) {
        for (auto& device : devices) {
            const auto& driver = device->driver;
            if (!driver->task || device->quiesced || !driver->task->pending()) continue;
            const auto advanced = driver->task->step();
            if (!advanced) return advanced.error();
            const auto ended = driver->task->result();
            if (ended || driver->state == mgr_state::Draining) {
                if ((status = driver->task->retire(ended ? ended->code : STATUS_CANCELED)) != STATUS_OK) sys::exit(status);
                if ((status = io_close(device->space.selector())) != STATUS_OK) sys::exit(status);
                const auto fn = bus->function(device->rid);
                if (!fn) sys::exit(fn.error());
                if ((status = device->reset(*fn)) != STATUS_OK) sys::exit(status);
                device->quiesced = true;
            }
        }
        if ((status = owner.poll()) != STATUS_OK) sys::exit(status);
        for (size_t count = 0; count < 8; ++count) {
            status = inventory.poll(); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) return status;
        }
        for (auto& device : devices) {
            const auto& driver = device->driver;
            if (!driver->task && (device->quota || driver->scope)) {
                const auto fn = bus->function(device->rid);
                if (!fn) sys::exit(fn.error());
                driver->startup = {}; driver->inbox.clear();
                if ((status = device->retire(*fn)) != STATUS_OK) sys::exit(status);
                if (driver->scope) {
                    status = sys::scope_close(driver->scope.selector()).status;
                    if (status == STATUS_OK) status = sys::wait_one(driver->scope.selector(), STATE_DRAINED).status;
                    if (status != STATUS_OK) return status;
                    driver->scope = {};
                }
                if (driver->desired) {
                    (void)owner.start(*driver);
                    device->quiesced = false; continue;
                }
            }
            if (driver->task && device->published_run != driver->run_id) {
                auto connector = owner.lookup(device->name + "/block");
                if (!connector && connector.error() != STATUS_NOT_FOUND) return connector.error();
                if (connector) {
                    const auto fn = bus->function(device->rid); if (!fn) return fn.error();
                    fn->write<uint16_t>(4, 6); (void)fn->read<uint16_t>(4);
                    device->published_run = driver->run_id;
                    // The first discovered disk is the default deployment policy.
                    // Every driver remains addressable through its recursive name.
                    if (device != devices.front()) continue;
                    ubus_msg request, response;
                    if (device->publication) {
                        request.data.put("instance", device->publication);
                        status = parent.invoke("Withdraw", std::move(request), response);
                        if (status != STATUS_OK && status != STATUS_NOT_FOUND) return status;
                    }
                    request = {};
                    const auto peer_idx = request.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
                    if (!peer_idx) return peer_idx.error();
                    request.data.put("name", "block").put("interface", "io.block").put("version", uint64_t{1})
                    .cap("connector", *peer_idx);
                    status = parent.invoke("Publish", std::move(request), response); if (status != STATUS_OK) return status;
                    const auto published = response.data["instance"].as<uint64_t>();
                    if (!published) return STATUS_PEER_FAULT;
                    device->publication = *published;
                }
            }
        }
        if ((status = ev_wait(rt_events(), owner.deadline()).status) != STATUS_OK && status != STATUS_TIMED_OUT) sys::exit(status);
    }
}
