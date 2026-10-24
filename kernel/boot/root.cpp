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
#include <sched/sched.hpp>
#include <sched/dispatcher.hpp>
#include <task/env.hpp>
#include <task/thread.hpp>
#include <uapi/start.h>
#include <array>
#include <algorithm>

constexpr usize root_stack_pages = 8;
constexpr mm::va_t root_ipc_address{0x40000000};
constexpr usize root_stack_size = root_stack_pages * mm::page_size;
constexpr usize root_initial_bytes = 384;
constexpr mm::va_t root_info_address{mm::UserEnd - 2 * mm::page_size};
constexpr mm::va_t root_stack_address{root_info_address.raw() - mm::page_size - root_stack_size};

[[nodiscard]] static constexpr auto charge_pages(usize pages, u64 caps = 0) noexcept
    -> resource::Capacity {
    return resource::Capacity{
        .memory = static_cast<u64>(pages) * mm::page_size,
        .caps = caps,
    };
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
                                      resource::Reservation&& usage, object::ref<> object,
                                      cap::Rights rights, cap::Limits limit = {}) noexcept
    -> std::expected<cap::Handle, BootErr> {
    auto grant =
        grants.create_root(std::move(usage), std::move(object), cap::View{rights, limit});
    if (!grant) {
        return std::unexpected(BootErr::CapabilityFailed);
    }
    auto cap = cspace.insert(std::move(grant).value(), cap::View{rights, limit});
    return cap ? std::expected<cap::Handle, BootErr>{(cap.value())}
               : std::expected<cap::Handle, BootErr>{std::unexpected(BootErr::CapabilityFailed)};
}

// First user thread only. Failure aborts boot; this is not a process service
// with an unload/retry protocol. Successful publication transfers ownership to
// capabilities and mappings; the caller enables execution after CPU admission.
auto boot_root(Boot& boot, Cpu& runtime, const BootInfo& handoff,
               mm::BootPages&& reservation, mm::BootPages&& firmware) noexcept -> std::expected<object::ref<Thread>, BootErr> {
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
        auto reservation = pool->reserve(pool, charge_pages(1));
        if (!reservation) {
            return std::unexpected(BootErr::OutOfMemory);
        }
        auto scope = object::Scope::reserve(boot.scope);
        if (!scope) return std::unexpected(BootErr::InvalidState);
        auto image = boot.objects.get<mm::Mem>().create(std::move(*scope), std::move(reservation).value(), pmm, size,
                                                   mm::PhysCfg{{&extent, 1}, std::move(pages)});
        if (!image) {
            return std::unexpected(BootErr::OutOfMemory);
        }

        bundle_pending.emplace(std::move(*image));
    }

    object::ref<mm::Mem> stack, info, ipc_mem;
    auto reserve =
        [&](resource::Capacity usage) noexcept -> std::expected<resource::Reservation, BootErr> {
        auto reserved = pool->reserve(pool, usage);
        return reserved
                   ? std::expected<resource::Reservation, BootErr>{(std::move(reserved).value())}
                   : std::expected<resource::Reservation, BootErr>{
                         std::unexpected(BootErr::OutOfMemory)};
    };

    // The group owns retirement, independently of storage references held by
    // maps or capability slots. Private boot pages need the same protocol.
    auto own = [&](auto& pending, cap::View view,
                   cap::CSpace* space = nullptr) noexcept -> std::expected<cap::Handle, BootErr> {
        auto fee = reserve(cap::Graph::node_usage());
        if (!fee) return std::unexpected(fee.error());
        auto root = boot.grants.create_root(std::move(*fee), pending.reference(), view);
        if (!root) return std::unexpected(BootErr::CapabilityFailed);
        cap::Handle handle;
        if (space) {
            auto pin = root->acquire();
            if (!pin) return std::unexpected(BootErr::CapabilityFailed);
            auto slots = space->publish(std::array{view}, [&](cap::View child)
                -> std::expected<cap::GrantRef, cap::CSpace::PublishError> {
                auto fee = reserve(cap::Graph::node_usage());
                if (!fee) return std::unexpected(resource::errc::exhausted);
                auto target = pin->clone_target();
                if (!target) return std::unexpected(cap::GrantError::InvalidState);
                auto grant = boot.grants.derive(std::move(*fee), *pin, std::move(*target), child);
                if (!grant) return std::unexpected(grant.error());
                return std::move(*grant);
            });
            if (!slots) return std::unexpected(BootErr::CapabilityFailed);
            handle = (*slots)[0];
        }
        pending.publish().reset();
        return handle;
    };

    // Keep construction/publication scratch in bounded cold steps.
    auto create = [&]<class T> [[gnu::noinline]] (resource::Capacity budget, auto&&... args) noexcept
        -> std::expected<object::pending<T>, BootErr> {
        auto usage = reserve(budget);
        if (!usage) return std::unexpected(usage.error());
        auto scope = object::Scope::reserve(boot.scope);
        if (!scope) return std::unexpected(BootErr::InvalidState);
        auto result = boot.objects.get<T>().create(std::move(*scope), std::move(*usage),
            std::forward<decltype(args)>(args)...);
        if (!result) return std::unexpected(BootErr::OutOfMemory);
        return std::move(*result);
    };
    auto create_mem = [&] [[gnu::noinline]] (object::ref<mm::Mem>& dst, resource::Capacity budget,
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
        charge_pages(17, 10), pmm);
    if (!vspace || !cspace) return std::unexpected(BootErr::OutOfMemory);
    auto& space = cspace->get();
    {
        const mm::VRange stack_range{root_stack_address, root_stack_size};
        const mm::VRange info_range{root_info_address, mm::page_size};
        for (size_t i = 0; i < init.segment_count(); ++i) {
            const auto seg = *init.segment(i);
            if (seg.type != 1 || !seg.size) continue;
            const auto skip = seg.va % mm::page_size;
            const mm::va_t va{seg.va - skip};
            const auto perms = mm::Perms::from_raw(static_cast<u8>(
                ((seg.flags & 4) ? 1 : 0) | ((seg.flags & 2) ? 2 : 0) | ((seg.flags & 1) ? 4 : 0)));
            const auto size = libk::checked_align_up(seg.size + skip, mm::page_size);
            if (va.raw() < mm::UserBegin || va.raw() >= mm::UserEnd || !size ||
                *size > mm::UserEnd - va.raw() || !mm::valid_perms(perms))
                return std::unexpected(BootErr::InvalidBundle);
            const mm::VRange range{va, *size};
            if (range.intersects(stack_range) || range.intersects(info_range) ||
                range.intersects(mm::VRange{root_ipc_address, mm::page_size}))
                return std::unexpected(BootErr::InvalidBundle);
            object::ref<mm::Mem> hold;
            if (!create_mem(hold, charge_pages(1 + *size / mm::page_size), pmm, *size,
                        mm::AnonCfg{.perms = perms, .eager = true}))
                return std::unexpected(BootErr::OutOfMemory);
            const std::span bytes{
                reinterpret_cast<const uint8_t*>(file->data.data()) + seg.offset, seg.file_size};
            if (!write_memory(boot.pmm, hold.get(), bytes, skip))
                return std::unexpected(BootErr::OutOfMemory);
            if (perms.contains(mm::Perm::Execute) && !hold->seal())
                return std::unexpected(BootErr::InvalidState);
            if (!map_memory(vspace->get(), cpu, hold, va, perms))
                return std::unexpected(BootErr::MappingFailed);
            // The mapping now owns the segment; no boot-side lifetime mirror.
        }
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

    auto create_thread = [&] [[gnu::noinline]] () noexcept
        -> std::expected<object::pending<Thread>, BootErr> {
        auto thread_charge = reserve(object::pool<Thread>::slot_usage());
        if (!thread_charge) return std::unexpected(BootErr::OutOfMemory);
        auto ipc_buffer = vspace->get().bind_view({.memory = ipc_mem, .object = mm::ObjectRange{0, 1},
            .virtual_range = mm::VRange{root_ipc_address, mm::page_size}});
        if (!ipc_buffer)
            return std::unexpected(BootErr::MappingFailed);
        auto execution =
            Env::user(vspace->reference(), cspace->reference(),
                      std::move(ipc_buffer).value());
        if (!execution)
            return std::unexpected(BootErr::InvalidState);
        auto scope = object::Scope::reserve(boot.scope);
        if (!scope) return std::unexpected(BootErr::InvalidState);
        auto pending_thread = boot.objects.get<Thread>().create(std::move(*scope),
            std::move(thread_charge).value(), std::move(execution).value(),
            Thread::UserStart{
                .entry = init.entry(),
                .stack = root_stack_address.raw() + root_stack_size - root_initial_bytes,
            });
        if (!pending_thread)
            return std::unexpected(BootErr::OutOfMemory);
        return std::move(*pending_thread);
    };
    auto thread = create_thread();
    if (!thread) return std::unexpected(thread.error());

    auto sc = create.template operator()<sched::Sc>(charge_pages(1),
                sched::Sc::Config{.weight = 2048},
                boot.clock.rd_ticks());
    if (!sc)
        return std::unexpected(BootErr::OutOfMemory);
    auto& root_sc = sc->get();
    if (!thread->get().claim_home(runtime.dispatcher()) || !boot.domain->admit(root_sc, cpu)
        || !root_sc.bind(sc->reference(), thread->get()))
        return std::unexpected(BootErr::SchedulingFailed);
    auto root_thread = thread->reference();
    auto publish_root = [&] [[gnu::noinline]] () noexcept -> std::expected<void, BootErr> {
        // Write directly into its backing page, with no second boot-stack copy.
        auto info_lease = info->materialize(0);
        if (!info_lease)
            return std::unexpected(BootErr::OutOfMemory);
        auto& info_page = *libk::construct_at(
            reinterpret_cast<RootInfo*>(boot.pmm.bytes(info_lease.value().page())));
        info_page.version = ROOT_VERSION;
        info_page.bytes = sizeof(RootInfo);
        info_page.bundle_bytes = module.size;
        info_page.boot_hart = runtime.hw.raw;
        auto install = [&] [[gnu::noinline]] (auto& object, cap::View view) noexcept -> std::expected<cap::Handle, BootErr> {
            if constexpr (requires { object.publish(); }) {
                return own(object, view, &space);
            } else {
                auto fee = reserve(cap::Graph::node_usage());
                if (!fee) return std::unexpected(fee.error());
                return install_cap(boot.grants, space, std::move(*fee), object, view.rights, view.data);
            }
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
                cap::Right::Inspect, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke, extra...);
        };
        if (!add_cap(info_page.physical, boot.physical,
                     rights(cap::Right::Create, cap::Right::Control),
                     cap::PhysLimit{{mm::Page{0}, arch::Pte::PhysicalPages}}))
            return std::unexpected(BootErr::CapabilityFailed);
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
                            cap::Right::Control, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
        const auto pool_rights = cap::Rights::of(
            cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Create,
            cap::Right::Split, cap::Right::Close, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
        const auto cspace_rights =
            cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                            cap::Right::Manage, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
        const auto vspace_rights = cap::Rights::of(
            cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Reserve, cap::Right::Map,
            cap::Right::Unmap, cap::Right::Destroy, cap::Right::Protect, cap::Right::Inspect,
            cap::Right::Manage, cap::Right::Control, cap::Right::Observe, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
        const cap::VmLimit vm_limit{
            .range = mm::VRange{mm::va_t{mm::UserBegin}, mm::UserEnd - mm::UserBegin},
            .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write, mm::Perm::Execute),
        };
        const cap::MemLimit bundle_limit{
            .range = mm::ObjectRange{0, bundle_pending->get().page_count()},
            .perms = mm::Perms::of(mm::Perm::Read),
        };
        constexpr u64 resource_kinds = OBJ_KINDS;
        const cap::QuotaLimit pool_limit{
            .budget = pool->limit(),
            .object_kinds = resource_kinds,
        };

        cap_t vm_cap{}, cs_cap{}, pool_cap{}, scope_cap{}, event_cap{}, stack_cap{}, ipc_cap{};
        auto events = create.template operator()<ipc::Port>(object::pool<ipc::Port>::slot_usage(), pmm, ipc::Port::Config{128, 16});
        if (!events) return std::unexpected(events.error());
        status_t ready;
        do { ready = events->get().open(); } while (ready == STATUS_PENDING);
        if (ready != STATUS_OK) return std::unexpected(BootErr::OutOfMemory);
        const auto event_rights = rights(cap::Right::Manage, cap::Right::Receive, cap::Right::Inspect);
        const auto installed_event = install(*events, {event_rights});
        if (!installed_event) return std::unexpected(installed_event.error());
        event_cap = installed_event->raw();
        if (!add_cap(stack_cap, stack, rights(cap::Right::Map, cap::Right::Destroy), cap::MemLimit{{0, root_stack_pages}, rw}) ||
            !add_cap(ipc_cap, ipc_mem, rights(cap::Right::Map, cap::Right::Destroy), cap::MemLimit{{0, 1}, rw}) ||
            !add_cap(vm_cap, *vspace, vspace_rights, vm_limit) ||
            !add_cap(cs_cap, *cspace, cspace_rights) ||
            !add_cap(info_page.bundle, *bundle_pending,
                     cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Map,
                                     cap::Right::Inspect, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke), bundle_limit) ||
            !add_cap(info_page.thread, *thread, cap::Rights::from_raw(basic_rights.raw() | RIGHT_OBSERVE)) ||
            !add_cap(pool_cap, pool, pool_rights, pool_limit) ||
            !add_cap(scope_cap, boot.scope, cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate,
                cap::Right::Inspect, cap::Right::Create, cap::Right::Manage, cap::Right::Close,
                cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke), cap::ScopeLimit{}) ||
            !add_cap(info_page.sc, *sc, cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Control, cap::Right::Attach, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke)) ||
            !add_cap(info_page.domain, boot.domain, cap::Rights::from_raw(basic_rights.raw() | RIGHT_CREATE | RIGHT_CLOSE)))
            return std::unexpected(BootErr::CapabilityFailed);
        const auto sp = root_stack_address.raw() + root_stack_size - root_initial_bytes;
        auto stack_page = stack->materialize(root_stack_pages - 1);
        if (!stack_page) return std::unexpected(BootErr::OutOfMemory);
        auto* initial = boot.pmm.bytes(stack_page->page()) + mm::page_size - root_initial_bytes;
        ::memset(initial, 0, root_initial_bytes);
        auto* words = reinterpret_cast<word_t*>(initial);
        words[0] = 1;
        words[1] = sp + root_initial_bytes - 16;
        auto aux = [&](size_t i, word_t type, word_t value) {
            std::construct_at(reinterpret_cast<AuxEntry*>(initial + 4 * sizeof(word_t)) + i, type, value);
        };
        aux(0, AT_PAGESZ, mm::page_size); aux(1, AT_ENTRY, init.entry());
        aux(2, AT_SYS_QUOTA, pool_cap); aux(3, AT_SYS_VSPACE, vm_cap);
        aux(4, AT_SYS_CSPACE, cs_cap); aux(5, AT_SYS_EVENTS, event_cap);
        aux(6, AT_SYS_CPUS, boot.cpus.count()); aux(7, AT_SYS_IPC, root_ipc_address.raw());
        aux(8, AT_SYS_BOOT, root_info_address.raw()); aux(9, AT_SYS_BOOT_SIZE, info_page.bytes);
        aux(10, AT_SYS_SCOPE, scope_cap); aux(11, AT_SYS_DOMAIN, info_page.domain); aux(12, AT_SYS_STACK, root_stack_address.raw()); aux(13, AT_SYS_STACK_SIZE, root_stack_size);
        aux(14, AT_SYS_THREAD, info_page.thread); aux(15, AT_SYS_SC, info_page.sc);
        aux(16, AT_SYS_STACK_MEM, stack_cap); aux(17, AT_SYS_IPC_MEM, ipc_cap); aux(18, AT_NULL, 0);
        ::memcpy(initial + root_initial_bytes - 16, "init", 5);
        return {};
    };
    const auto published = publish_root();
    if (!published) return std::unexpected(published.error());

    return root_thread;
}
