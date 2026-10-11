#include <expected>
#include <optional>
#include <object/pool.hpp>
#include <boot/info.hpp>
#include <libk/pkg.hpp>
#include <libk/byte_reader.hpp>
#include <cap/cap.hpp>
#include <cap/cspace.hpp>
#include <cpu.hpp>
#include <boot/start.hpp>
#include <console.hpp>
#include <cpu/cpu.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/mem.h>
#include <atomic>
#include <utility>
#include <mm/kspace.hpp>
#include <mm/mem.hpp>
#include <mm/types.hpp>
#include <mm/table.hpp>
#include <wait.hpp>
#include <mm/vspace.hpp>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <task/env.hpp>
#include <task/thread.hpp>
#include <uapi/start.h>

// The kernel loads one execution. Application names and deployment belong to root.
struct Init {
    usize entry{}, payload{};
    std::span<const uint8_t> loads{};
};

static auto read_init(std::span<const uint8_t> bytes) noexcept -> std::optional<Init> {
    if (bytes.size() < BOOT_LOAD_TRAILER) return {};
    const auto end = bytes.size() - BOOT_LOAD_TRAILER;
    libk::byte_reader r{bytes.data() + end, BOOT_LOAD_TRAILER};
    u64 magic{}, payload{}, entry{}, off{}, count{};
    if (!r.read_le(magic) || !r.read_le(payload) || !r.read_le(entry) ||
        !r.read_le(off) || !r.read_le(count) || magic != BOOT_LOAD_MAGIC ||
        !payload || payload != end || !count || off > payload ||
        count > (payload - off) / PKG_SEGMENT_SIZE) return {};
    return Init{entry, payload, bytes.subspan(off, count * PKG_SEGMENT_SIZE)};
}

struct Load {
    usize va{}, size{};
    std::span<const uint8_t> file{};
    mm::Perms perms{};
};

static auto read_load(libk::byte_reader& r, std::span<const uint8_t> payload) noexcept
    -> std::optional<Load> {
    const auto s = libk::load_seg::read(r);
    if (!s || !s->size || s->file > s->size || s->off > payload.size() ||
        s->file > payload.size() - s->off || s->va % mm::page_size ||
        s->va < mm::UserBegin || s->va >= mm::UserEnd ||
        s->size > mm::UserEnd - s->va || s->perms > 7) return {};
    const auto perms = mm::Perms::from_raw(static_cast<u8>(s->perms));
    if (!mm::valid_perms(perms) ||
        (perms.contains(mm::Perm::Write) && perms.contains(mm::Perm::Execute))) return {};
    return Load{s->va, s->size, payload.subspan(s->off, s->file), perms};
}

constexpr usize root_stack_pages = 8;
constexpr mm::va_t root_ipc_address{BOOT_ROOT_IPC_ADDRESS};
constexpr usize root_stack_size = root_stack_pages * mm::page_size;
constexpr mm::va_t root_info_address{mm::UserEnd - 2 * mm::page_size};
constexpr mm::va_t root_stack_address{root_info_address.raw() - mm::page_size - root_stack_size};

[[nodiscard]] static constexpr auto charge_pages(usize pages, u64 caps = 0) noexcept
    -> resource::budget {
    return resource::budget{
        .memory = static_cast<u64>(pages) * mm::page_size,
        .caps = caps,
    };
}

[[nodiscard]] static constexpr auto page_round(usize size) noexcept -> std::optional<usize> {
    const auto adjusted = libk::checked_add(size, mm::page_size - 1);
    return adjusted ? std::optional<usize>{*adjusted & ~(mm::page_size - 1)} : std::nullopt;
}

[[nodiscard]] static auto write_memory(mm::Pmm& pmm, mm::Mem& memory, std::span<const uint8_t> bytes) noexcept
    -> bool {
    if (bytes.size() > memory.size()) {
        return false;
    }
    usize copied{};
    for (usize index = 0; index < memory.page_count(); ++index) {
        auto page = memory.materialize(index);
        if (!page) {
            return false;
        }
        byte* const destination = pmm.bytes(page.value().page());
        const usize remaining = bytes.size() - copied;
        const usize amount = remaining < mm::page_size ? remaining : mm::page_size;
        if (amount != 0) {
            memcpy(destination, bytes.data() + copied, amount);
            copied += amount;
        }
        if (amount != mm::page_size) {
            memset(destination + amount, 0, mm::page_size - amount);
        }
    }
    return copied == bytes.size();
}

[[nodiscard]] static auto map_memory(mm::VSpace& vspace, CpuId cpu, object::ref<mm::Mem>& memory,
                                     mm::va_t address, mm::Perms access) noexcept -> bool {
    const mm::ObjectRange object{0, memory->page_count()};
    const auto mapped =
        vspace.map(mm::VmCtx{.local = cpu},
                   mm::MapReq{
                       .virtual_range = mm::VRange{address, memory->size()},
                       .object = object,
                       .perms = access,
                   },
                   memory, memory.get(), cap::MemLimit{object, access});
    return mapped && mapped.value().status == mm::VmStatus::Complete;
}

[[nodiscard]] static auto install_cap(cap::Graph& grants, cap::CSpace& cspace,
                                      resource::Reservation&& charge, object::ref<> object,
                                      cap::Rights rights, cap::Limits limit = {}) noexcept
    -> std::expected<cap::Handle, BootErr> {
    auto grant =
        grants.create_root(std::move(charge), std::move(object), cap::View{rights, limit});
    if (!grant) {
        return std::unexpected(BootErr::CapabilityFailed);
    }
    auto cap = cspace.insert(std::move(grant).value(), cap::View{rights, limit});
    return cap ? std::expected<cap::Handle, BootErr>{(cap.value())}
               : std::expected<cap::Handle, BootErr>{std::unexpected(BootErr::CapabilityFailed)};
}

// First user thread only. Failure aborts boot; this is not a process service
// with an unload/retry protocol. Successful publication transfers ownership to
// capabilities, mappings and the scheduler before these local refs are dropped.
auto boot_root(Boot& boot, Cpu& runtime, const BootInfo& handoff,
               mm::BootPages&& reservation, mm::BootPages&& firmware, HwStart start) noexcept -> std::expected<void, BootErr> {
    const auto module = *handoff.module;
    auto pool = boot.root;
    auto& pmm = boot.pmm;

    if (!pool || !module || module.pa % mm::page_size != 0 || !reservation ||
        reservation.range().base() != module.pages().base() ||
        reservation.range().page_count() != module.pages().page_count()) {
        return std::unexpected(BootErr::InvalidModule);
    }
    const auto source = pmm.ptr<const byte>(mm::pa_t{module.pa}, module.size);
    if (!source) {
        return std::unexpected(BootErr::InvalidModule);
    }
    const auto parsed = read_init(std::span<const uint8_t>{source.value(), module.size});
    if (!parsed) {
        return std::unexpected(BootErr::InvalidBundle);
    }

    const Init& init = *parsed;
    const std::span<const uint8_t> payload{source.value(), init.payload};
    std::optional<object::pending<mm::Mem>> bundle_pending;
    {
        auto adopted = pmm.adopt(std::move(reservation));
        if (!adopted) {
            return std::unexpected(BootErr::Ownership);
        }
        mm::PageGroup pages = std::move(adopted).value();
        const usize size = pages.page_count() * mm::page_size;
        const mm::Extent extent{
            .object = mm::ObjectRange{0, pages.page_count()},
            .physical = module.pages(),
            .perms = mm::Perms::of(mm::Perm::Read),
        };
        auto sponsorship = pool->reserve(pool, charge_pages(1));
        if (!sponsorship) {
            return std::unexpected(BootErr::OutOfMemory);
        }
        auto image = boot.objects.get<mm::Mem>().create(std::move(sponsorship).value(), pmm, size,
                                                   mm::PhysCfg{{&extent, 1}, std::move(pages)});
        if (!image) {
            return std::unexpected(BootErr::OutOfMemory);
        }

        bundle_pending.emplace(std::move(*image));
    }

    object::ref<mm::Mem> stack, info, ipc_mem;
    auto reserve =
        [&](resource::budget charge) noexcept -> std::expected<resource::Reservation, BootErr> {
        auto reserved = pool->reserve(pool, charge);
        return reserved
                   ? std::expected<resource::Reservation, BootErr>{(std::move(reserved).value())}
                   : std::expected<resource::Reservation, BootErr>{
                         std::unexpected(BootErr::OutOfMemory)};
    };

    // The group owns retirement, independently of storage references held by
    // maps or capability slots. Private boot pages need the same protocol.
    auto own = [&](auto& pending, cap::View view,
                   cap::CSpace* space = nullptr) noexcept -> std::expected<cap::Handle, BootErr> {
        auto fee = reserve(cap::Graph::node_charge());
        if (!fee) return std::unexpected(fee.error());
        auto root = pending.root(boot.grants, std::move(*fee), view);
        if (!root) return std::unexpected(BootErr::CapabilityFailed);
        cap::Handle handle;
        if (space) {
            auto lease = root->acquire();
            if (!lease) return std::unexpected(BootErr::CapabilityFailed);
            auto slots = space->publish(std::array{view}, [&](cap::View child)
                -> std::expected<cap::GrantRef, cap::CSpace::PublishError> {
                auto fee = reserve(cap::Graph::node_charge());
                if (!fee) return std::unexpected(resource::errc::exhausted);
                auto target = lease->clone_target();
                if (!target) return std::unexpected(cap::GrantError::InvalidState);
                auto grant = boot.grants.derive(std::move(*fee), *lease, std::move(*target), child);
                if (!grant) return std::unexpected(grant.error());
                return std::move(*grant);
            });
            if (!slots) return std::unexpected(BootErr::CapabilityFailed);
            handle = (*slots)[0];
        }
        pending.publish().reset();
        return handle;
    };

    auto create = [&]<class T>(resource::budget budget, auto&&... args) noexcept
        -> std::expected<object::pending<T>, BootErr> {
        auto charge = reserve(budget);
        if (!charge) return std::unexpected(charge.error());
        auto result = boot.objects.get<T>().create(std::move(*charge),
            std::forward<decltype(args)>(args)...);
        if (!result) return std::unexpected(BootErr::OutOfMemory);
        return std::move(*result);
    };
    auto create_mem = [&](object::ref<mm::Mem>& dst, resource::budget budget,
                          auto&&... args) noexcept {
        auto result = create.template operator()<mm::Mem>(budget,
            std::forward<decltype(args)>(args)...);
        if (!result) return false;
        dst = result->reference();
        return own(*result, {{}, cap::MemLimit{{0, dst->page_count()},
                    mm::Perms::of(mm::Perm::Read)}}).has_value();
    };
    const CpuId cpu = runtime.id;
    auto vspace = create.template operator()<mm::VSpace>(charge_pages(64), pmm, boot.vm, boot.work);
    auto cspace = create.template operator()<cap::CSpace>(
        charge_pages(17, (mm::page_size - sizeof(BootHdr)) / sizeof(BootCap)), pmm);
    if (!vspace || !cspace) return std::unexpected(BootErr::OutOfMemory);
    auto& space = cspace->get();
    {
        const mm::VRange stack_range{root_stack_address, root_stack_size};
        const mm::VRange info_range{root_info_address, mm::page_size};
        libk::byte_reader loads{init.loads.data(), init.loads.size()};
        usize end{};
        bool entry_ok{};
        while (loads.remaining()) {
            const auto seg = read_load(loads, payload);
            if (!seg) return std::unexpected(BootErr::InvalidBundle);
            const auto size = page_round(seg->size);
            if (!size || seg->va < end || *size > mm::UserEnd - seg->va)
                return std::unexpected(BootErr::InvalidBundle);
            end = seg->va + *size;
            entry_ok |= init.entry >= seg->va && init.entry - seg->va < seg->size &&
                        seg->perms.contains(mm::Perm::Execute);
            const mm::VRange range{mm::va_t{seg->va}, *size};
            if (range.intersects(stack_range) || range.intersects(info_range) ||
                range.intersects(mm::VRange{root_ipc_address, mm::page_size}))
                return std::unexpected(BootErr::InvalidBundle);
            object::ref<mm::Mem> hold;
            if (!create_mem(hold, charge_pages(1 + *size / mm::page_size), pmm, *size,
                        mm::AnonCfg{.perms = seg->perms, .eager = true}))
                return std::unexpected(BootErr::OutOfMemory);
            if (!write_memory(boot.pmm, hold.get(), seg->file)) {
                return std::unexpected(BootErr::OutOfMemory);
            }
            if (seg->perms.contains(mm::Perm::Execute) && !hold->seal()) {
                return std::unexpected(BootErr::InvalidState);
            }
            if (!map_memory(vspace->get(), cpu, hold, mm::va_t{seg->va},
                            seg->perms)) {
                return std::unexpected(BootErr::MappingFailed);
            }
            // The mapping now owns the segment; no boot-side lifetime mirror.
        }
        if (!entry_ok) return std::unexpected(BootErr::InvalidBundle);
    }

    const auto rw = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);
    const mm::AnonCfg anon{.perms = rw, .eager = true};
    if (!create_mem(stack, charge_pages(1 + root_stack_pages), pmm, root_stack_size, anon) ||
        !create_mem(info, charge_pages(2), pmm, mm::page_size, anon) ||
        !create_mem(ipc_mem, charge_pages(2), pmm, mm::page_size, anon))
        return std::unexpected(BootErr::OutOfMemory);
    if (!map_memory(vspace->get(), cpu, stack, root_stack_address,
                    mm::Perms::of(mm::Perm::Read, mm::Perm::Write)) ||
        !map_memory(vspace->get(), cpu, info, root_info_address, mm::Perms::of(mm::Perm::Read)) ||
        !map_memory(vspace->get(), cpu, ipc_mem, root_ipc_address,
                    mm::Perms::of(mm::Perm::Read, mm::Perm::Write))) {
        return std::unexpected(BootErr::MappingFailed);
    }

    std::optional<object::pending<Thread>> thread;
    {
        auto thread_charge = reserve(object::pool<Thread>::slot_charge());
        auto kernel_stack_charge = reserve(resource::budget{.memory = mm::Stack::StackBytes});
        if (!thread_charge || !kernel_stack_charge)
            return std::unexpected(BootErr::OutOfMemory);
        auto stack_capacity = std::move(kernel_stack_charge).value();
        auto home = mm::Stack::create(boot.vm);
        if (!home)
            return std::unexpected(BootErr::OutOfMemory);
        auto ipc_buffer = ipc::Buffer::bind(
            boot.pmm, vspace->get(), ipc_mem, ipc_mem.get(),
            mm::ObjectRange{0, 1}, mm::VRange{root_ipc_address, mm::page_size});
        if (!ipc_buffer)
            return std::unexpected(BootErr::MappingFailed);
        auto execution =
            Env::user(vspace->reference(), cspace->reference(),
                      std::move(ipc_buffer).value());
        if (!execution)
            return std::unexpected(BootErr::InvalidState);
        auto pending_thread = boot.objects.get<Thread>().create(
            std::move(thread_charge).value(), std::move(stack_capacity).commit(),
            std::move(home).value(), std::move(execution).value(),
            Thread::UserStart{
                .entry = init.entry,
                .stack = root_stack_address.raw() + root_stack_size,
                .arguments = {root_info_address.raw(), mm::page_size},
            });
        if (!pending_thread)
            return std::unexpected(BootErr::OutOfMemory);
        thread.emplace(std::move(*pending_thread));
    }

    const auto budget = boot.clock.ticks<2, time::ms>();
    const auto period = boot.clock.ticks<10, time::ms>();
    const auto urgency = sched::Urgency::make(20);
    if (!budget || !period || !urgency) {
        return std::unexpected(BootErr::SchedulingFailed);
    }
    auto sc = create.template operator()<sched::Sc>(charge_pages(1),
                sched::Sc::Config{.budget = *budget, .period = *period, .urgency = *urgency},
                boot.clock.rd_ticks());
    if (!sc)
        return std::unexpected(BootErr::OutOfMemory);
    auto& root_sc = sc->get();
    if (!boot.domain->admit(root_sc, cpu) || !root_sc.bind(thread->reference()))
        return std::unexpected(BootErr::SchedulingFailed);
    {
        // Write directly into its backing page, with no second boot-stack copy.
        auto info_lease = info->materialize(0);
        if (!info_lease)
            return std::unexpected(BootErr::OutOfMemory);
        auto& info_page = *libk::construct_at(
            reinterpret_cast<BootHdr*>(boot.pmm.bytes(info_lease.value().page())));
        info_page.magic = BOOT_MAGIC;
        info_page.major = BOOT_MAJOR;
        info_page.minor = BOOT_MINOR;
        info_page.size = sizeof(BootHdr);
        info_page.cpu_count = boot.cpus.count();
        info_page.stack_base = root_stack_address.raw();
        info_page.stack_size = root_stack_size;
        info_page.boot_bundle_size = init.payload;
        auto* entries = reinterpret_cast<BootCap*>(&info_page + 1);
        auto publish = [&](BootCap entry, auto& object, cap::View view) noexcept -> bool {
            if (sizeof(info_page) + (info_page.count + 1) * sizeof(BootCap) > mm::page_size)
                return false;
            auto installed = [&]() -> std::expected<cap::Handle, BootErr> {
                if constexpr (requires { object.publish(); }) {
                    entry.kind = object::kind<std::remove_reference_t<decltype(object.get())>>;
                    return own(object, view, &space);
                } else {
                    entry.kind = object.kind();
                    auto fee = reserve(cap::Graph::node_charge());
                    if (!fee) return std::unexpected(fee.error());
                    return install_cap(boot.grants, space, std::move(*fee), object,
                                       view.rights, view.data);
                }
            }();
            if (!installed) return false;
            entry.handle = installed->raw();
            libk::construct_at(entries + info_page.count++, entry);
            info_page.size += sizeof(BootCap);
            return true;
        };
        auto add_cap = [&](u32 role, auto& object, cap::Rights rights,
                           cap::Limits limit = {}) noexcept {
            BootCap entry{};
            entry.role = role;
            return publish(entry, object, {rights, limit});
        };
        const auto rights = [](auto... extra) noexcept {
            return cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate,
                cap::Right::Inspect, cap::Right::Revoke, extra...);
        };
        auto emit_hw = [&](u32 node, u32 index, HwRes resource) noexcept {
            BootCap entry{};
            entry.role = BOOT_RESOURCE;
            entry.resource = {node, index};
            return std::visit([&](auto value) noexcept {
                using T = decltype(value);
                auto emit = [&](auto pending, cap::View view) noexcept {
                    if (!pending) return false;
                    auto ref = std::move(*pending).publish();
                    return publish(entry, ref, view);
                };
                // Hardware belongs to the machine. Its initial grant remains
                // independent of the root task's reclaimable object group.
                if constexpr (std::same_as<T, io::Reg>) {
                    entry.phys = value.pa;
                    entry.bytes = value.size;
                    auto pages = mm::Pages::from_aligned_bytes(mm::pa_t{value.pa}, value.size);
                    if (!pages) return false;
                    const mm::Extent extent{{0, pages->page_count()}, *pages, value.perms};
                    return emit(boot.objects.get<mm::Mem>().create(pmm, value.size,
                        mm::PhysCfg{{&extent, 1}, {}}),
                        {rights(cap::Right::Map), cap::MemLimit{extent.object, extent.perms}});
                } else if constexpr (std::same_as<T, irq::Line>) {
                    return emit(boot.objects.get<irq::Irq>().create(value),
                        {rights(cap::Right::Route, cap::Right::Observe, cap::Right::Ack,
                                cap::Right::Close), cap::IrqRoute{value.id, value.level}});
                } else {
                    return emit(boot.objects.get<io::Host>().create(*value),
                        {rights(cap::Right::Connect), cap::HostLimit{0, value->count}});
                }
            }, resource);
        };
        runtime.ext_irq = start(pmm, boot.clock, HwEmit::bind(emit_hw));
        if (!runtime.ext_irq) return std::unexpected(BootErr::CapabilityFailed);

        auto pages = pmm.adopt(std::move(firmware));
        if (!pages) return std::unexpected(BootErr::Ownership);
        const mm::Extent extent{{0, pages->page_count()}, handoff.firmware.pages(),
                                mm::Perms::of(mm::Perm::Read)};
        auto fw = boot.objects.get<mm::Mem>().create(pmm, pages->page_count() * mm::page_size,
            mm::PhysCfg{{&extent, 1}, std::move(*pages)});
        BootCap fw_entry{};
        fw_entry.role = BOOT_FIRMWARE;
        fw_entry.firmware.format = handoff.fw_format;
        fw_entry.phys = handoff.firmware.pa;
        fw_entry.bytes = handoff.firmware.size;
        if (!fw) return std::unexpected(BootErr::OutOfMemory);
        auto fw_ref = std::move(*fw).publish();
        if (!publish(fw_entry, fw_ref,
            {rights(cap::Right::Map), cap::MemLimit{extent.object, extent.perms}}))
            return std::unexpected(BootErr::CapabilityFailed);

        const auto basic_rights =
            cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                            cap::Right::Control, cap::Right::Destroy, cap::Right::Revoke);
        const auto pool_rights = cap::Rights::of(
            cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Create,
            cap::Right::Split, cap::Right::Close, cap::Right::Revoke);
        const auto cspace_rights =
            cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                            cap::Right::Manage, cap::Right::Destroy, cap::Right::Revoke);
        const auto vspace_rights = cap::Rights::of(
            cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Reserve, cap::Right::Map,
            cap::Right::Unmap, cap::Right::Destroy, cap::Right::Protect, cap::Right::Inspect,
            cap::Right::Manage, cap::Right::Revoke);
        const cap::VmLimit vm_limit{
            .range = mm::VRange{mm::va_t{mm::UserBegin}, mm::UserEnd - mm::UserBegin},
            .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write, mm::Perm::Execute),
        };
        const cap::MemLimit bundle_limit{
            .range = mm::ObjectRange{0, bundle_pending->get().page_count()},
            .perms = mm::Perms::of(mm::Perm::Read),
        };
        constexpr u64 resource_kinds = OBJ_KINDS;
        const cap::Quota pool_limit{
            .budget = pool->limit(),
            .object_kinds = resource_kinds,
        };

        if (!add_cap(BOOT_VSPACE, *vspace, vspace_rights, vm_limit) ||
            !add_cap(BOOT_CSPACE, *cspace, cspace_rights) ||
            !add_cap(BOOT_BUNDLE, *bundle_pending,
                     cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Map,
                                     cap::Right::Inspect, cap::Right::Revoke),
                     bundle_limit) ||
            !add_cap(BOOT_THREAD, *thread, basic_rights) ||
            !add_cap(BOOT_POOL, pool, pool_rights, pool_limit)) {
            return std::unexpected(BootErr::CapabilityFailed);
        }

        if (!add_cap(BOOT_SC, *sc, basic_rights) ||
            !add_cap(BOOT_DOMAIN, boot.domain, basic_rights)) {
            return std::unexpected(BootErr::CapabilityFailed);
        }
    }

    if (!runtime.dispatcher().make_ready(root_sc))
        return std::unexpected(BootErr::SchedulingFailed);
    return {};
}
