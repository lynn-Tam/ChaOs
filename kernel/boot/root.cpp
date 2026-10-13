#include <expected>
#include <optional>
#include <object/pool.hpp>
#include <boot/info.hpp>
#include <libk/cpio.hpp>
#include <libk/elf.hpp>
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
#include <array>
#include <algorithm>

constexpr usize root_stack_pages = 8;
constexpr mm::va_t root_ipc_address{0x40000000};
constexpr usize root_stack_size = root_stack_pages * mm::page_size;
constexpr usize root_initial_bytes = 224;
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

[[nodiscard]] static auto write_memory(mm::Pmm& pmm, mm::Mem& memory, std::span<const uint8_t> bytes, usize offset = 0) noexcept
    -> bool {
    if (offset > memory.size() || bytes.size() > memory.size() - offset) return false;
    for (usize index = 0; index < memory.page_count(); ++index) {
        auto page = memory.materialize(index);
        if (!page) return false;
        auto* destination = pmm.bytes(page.value().page());
        memset(destination, 0, mm::page_size);
        const auto start = index * mm::page_size;
        const auto first = std::max(start, offset);
        const auto end = std::min(start + mm::page_size, offset + bytes.size());
        if (first < end) memcpy(destination + first - start, bytes.data() + first - offset, end - first);
    }
    return true;
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
    const auto archive = libk::cpio_view::parse(std::as_bytes(std::span{source.value(), module.size}));
    const auto file = archive.find("init");
    const auto init = file ? libk::elf_view::parse(file->data) : libk::elf_view{};
    if (!init || init.machine() != 243) return std::unexpected(BootErr::InvalidBundle);
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
        charge_pages(17, (mm::page_size - sizeof(RootInfo)) / sizeof(RootResource) + 9), pmm);
    if (!vspace || !cspace) return std::unexpected(BootErr::OutOfMemory);
    auto& space = cspace->get();
    {
        const mm::VRange stack_range{root_stack_address, root_stack_size};
        const mm::VRange info_range{root_info_address, mm::page_size};
        usize end{};
        bool entry_ok{};
        for (size_t i = 0; i < init.segment_count(); ++i) {
            const auto raw = *init.segment(i);
            if (raw.type != 1 || !raw.size) continue;
            const auto skip = raw.va % mm::page_size;
            struct { usize va, size; std::span<const uint8_t> file; mm::Perms perms; } segment{
                raw.va - skip, raw.size + skip,
                {reinterpret_cast<const uint8_t*>(file->data.data()) + raw.offset, raw.file_size},
                mm::Perms::from_raw(static_cast<u8>(((raw.flags & 4) ? 1 : 0) | ((raw.flags & 2) ? 2 : 0) | ((raw.flags & 1) ? 4 : 0)))};
            auto* seg = &segment;
            if (seg->va < mm::UserBegin || seg->va >= mm::UserEnd || !mm::valid_perms(seg->perms))
                return std::unexpected(BootErr::InvalidBundle);
            const auto size = page_round(seg->size);
            if (!size || seg->va < end || *size > mm::UserEnd - seg->va)
                return std::unexpected(BootErr::InvalidBundle);
            end = seg->va + *size;
            entry_ok |= init.entry() >= seg->va && init.entry() - seg->va < seg->size &&
                        seg->perms.contains(mm::Perm::Execute);
            const mm::VRange range{mm::va_t{seg->va}, *size};
            if (range.intersects(stack_range) || range.intersects(info_range) ||
                range.intersects(mm::VRange{root_ipc_address, mm::page_size}))
                return std::unexpected(BootErr::InvalidBundle);
            object::ref<mm::Mem> hold;
            if (!create_mem(hold, charge_pages(1 + *size / mm::page_size), pmm, *size,
                        mm::AnonCfg{.perms = seg->perms, .eager = true}))
                return std::unexpected(BootErr::OutOfMemory);
            if (!write_memory(boot.pmm, hold.get(), seg->file, skip)) {
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
                .entry = init.entry(),
                .stack = root_stack_address.raw() + root_stack_size - root_initial_bytes,
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
            reinterpret_cast<RootInfo*>(boot.pmm.bytes(info_lease.value().page())));
        info_page.version = ROOT_VERSION;
        info_page.bytes = sizeof(RootInfo);
        info_page.bundle_bytes = module.size;
        info_page.resources_offset = sizeof(RootInfo);
        auto* entries = reinterpret_cast<RootResource*>(&info_page + 1);
        auto install = [&](auto& object, cap::View view) noexcept -> std::expected<cap::Handle, BootErr> {
            if constexpr (requires { object.publish(); }) {
                return own(object, view, &space);
            } else {
                auto fee = reserve(cap::Graph::node_charge());
                if (!fee) return std::unexpected(fee.error());
                return install_cap(boot.grants, space, std::move(*fee), object, view.rights, view.data);
            }
        };
        auto publish = [&](RootResource entry, auto& object, cap::View view) noexcept -> bool {
            if (info_page.bytes + sizeof(entry) > mm::page_size) return false;
            entry.kind = object.kind();
            const auto installed = install(object, view);
            if (!installed) return false;
            entry.handle = installed->raw();
            libk::construct_at(entries + info_page.resources_count++, entry);
            info_page.bytes += sizeof(entry);
            return true;
        };
        auto add_cap = [&](cap_t& destination, auto& object, cap::Rights rights,
                           cap::Limits limit = {}) noexcept -> bool {
            const auto installed = install(object, {rights, limit});
            if (!installed) return false;
            destination = installed->raw();
            return true;
        };
        const auto rights = [](auto... extra) noexcept {
            return cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate,
                cap::Right::Inspect, cap::Right::Revoke, extra...);
        };
        auto emit_hw = [&](u32 node, u32 index, HwRes resource) noexcept {
            RootResource entry{};
            entry.node = node; entry.index = index;
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
        info_page.firmware_format = handoff.fw_format;
        info_page.firmware_offset = handoff.firmware.pa % mm::page_size;
        info_page.firmware_bytes = handoff.firmware.size;
        if (!fw) return std::unexpected(BootErr::OutOfMemory);
        auto fw_ref = std::move(*fw).publish();
        if (!add_cap(info_page.firmware, fw_ref, rights(cap::Right::Map),
                     cap::MemLimit{extent.object, extent.perms}))
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

        cap_t vm_cap{}, cs_cap{}, pool_cap{}, event_cap{};
        auto events = create.template operator()<ipc::Notification>(charge_pages(1));
        if (!events) return std::unexpected(events.error());
        const auto event_rights = rights(cap::Right::Signal, cap::Right::Receive);
        const auto installed_event = install(*events, {event_rights, cap::Badge{1}});
        if (!installed_event) return std::unexpected(installed_event.error());
        event_cap = installed_event->raw();
        if (!add_cap(vm_cap, *vspace, vspace_rights, vm_limit) ||
            !add_cap(cs_cap, *cspace, cspace_rights) ||
            !add_cap(info_page.bundle, *bundle_pending,
                     cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Map,
                                     cap::Right::Inspect, cap::Right::Revoke), bundle_limit) ||
            !add_cap(info_page.thread, *thread, basic_rights) ||
            !add_cap(pool_cap, pool, pool_rights, pool_limit) ||
            !add_cap(info_page.sc, *sc, basic_rights) ||
            !add_cap(info_page.domain, boot.domain, basic_rights))
            return std::unexpected(BootErr::CapabilityFailed);
        const auto sp = root_stack_address.raw() + root_stack_size - root_initial_bytes;
        alignas(16) std::array<uint8_t, root_initial_bytes> initial{};
        auto* words = reinterpret_cast<word_t*>(initial.data());
        words[0] = 1;
        words[1] = sp + root_initial_bytes - 16;
        const std::array<AuxEntry, 11> aux{{
            {AT_PAGESZ, mm::page_size}, {AT_ENTRY, init.entry()}, {AT_SYS_POOL, pool_cap},
            {AT_SYS_VSPACE, vm_cap}, {AT_SYS_CSPACE, cs_cap}, {AT_SYS_EVENTS, event_cap},
            {AT_SYS_CPUS, boot.cpus.count()}, {AT_SYS_IPC, root_ipc_address.raw()},
            {AT_SYS_BOOT, root_info_address.raw()}, {AT_SYS_BOOT_SIZE, info_page.bytes}, {AT_NULL, 0}}};
        ::memcpy(initial.data() + 4 * sizeof(word_t), aux.data(), sizeof(aux));
        ::memcpy(initial.data() + root_initial_bytes - 16, "init", 5);
        auto stack_page = stack->materialize(root_stack_pages - 1);
        if (!stack_page) return std::unexpected(BootErr::OutOfMemory);
        ::memcpy(boot.pmm.bytes(stack_page->page()) + mm::page_size - root_initial_bytes,
                     initial.data(), initial.size());

    }

    if (!runtime.dispatcher().make_ready(root_sc))
        return std::unexpected(BootErr::SchedulingFailed);
    return {};
}
