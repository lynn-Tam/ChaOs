#include <expected>
#include <optional>
#include <platform/riscv-virt/board.hpp>
#include <object/pool.hpp>
#include <boot/info.hpp>
#include <boot/bundle.hpp>
#include <cap/cap.hpp>
#include <cap/cspace.hpp>
#include <cpu.hpp>
#include <state.hpp>
#include <console.hpp>
#include <cpu/runtime.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/mem.h>
#include <libk/sync/atomic.hpp>
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

constexpr usize root_stack_pages = 8;
constexpr mm::Virt root_ipc_address{BOOT_ROOT_IPC_ADDRESS};
constexpr usize root_stack_size = root_stack_pages * mm::page_size;
constexpr mm::Virt root_info_address{mm::UserEnd - 2 * mm::page_size};
constexpr mm::Virt root_stack_address{root_info_address.raw() - mm::page_size - root_stack_size};

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

[[nodiscard]] static auto write_memory(mm::Pmm& pmm, mm::Mem& memory, libk::ByteSpan bytes) noexcept
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
                                     mm::Virt address, mm::Perms access) noexcept -> bool {
    auto reference = memory.erase();
    if (!reference) {
        return false;
    }
    const mm::ObjectRange object{0, memory->page_count()};
    const auto mapped =
        vspace.map(mm::VmCtx{.local = cpu},
                   mm::MapReq{
                       .virtual_range = mm::VRange{address, memory->size()},
                       .object = object,
                       .perms = access,
                   },
                   std::move(reference).value(), memory.get(), cap::MemLimit{object, access});
    return mapped && mapped.value().status == mm::VmStatus::Complete;
}

[[nodiscard]] static auto install_cap(KernelState& kernel, cap::CSpace& cspace,
                                      resource::Reservation&& charge, object::ref<>&& object,
                                      cap::Rights rights, cap::Limits limit = {}) noexcept
    -> std::expected<cap::Handle, BootErr> {
    auto grant =
        kernel.grants().create_root(std::move(charge), std::move(object), cap::View{rights, limit});
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
auto boot_root(KernelState& kernel, CpuRuntime& runtime, BootModule module,
               mm::BootPages&& reservation) noexcept -> std::expected<void, BootErr> {
    auto parent = kernel.clone_root_pool();
    if (!parent)
        return std::unexpected(BootErr::InvalidState);
    auto pool = std::move(parent).value();
    auto& pmm = kernel.pmm();

    if (!pool || !module || !module.physical.is_aligned(mm::page_size) ||
        module.pages.base().base() != module.physical || !reservation ||
        reservation.range().base() != module.pages.base() ||
        reservation.range().page_count() != module.pages.page_count()) {
        return std::unexpected(BootErr::InvalidModule);
    }
    const auto source = pmm.ptr<const byte>(module.physical, module.size);
    if (!source) {
        return std::unexpected(BootErr::InvalidModule);
    }
    const auto parsed = parse_bundle(libk::ByteSpan{source.value(), module.size});
    if (!parsed) {
        return std::unexpected(BootErr::InvalidBundle);
    }

    const BootBundle& package = *parsed;
    object::ref<mm::Mem> bundle_mem;
    {
        auto adopted = pmm.adopt(std::move(reservation));
        if (!adopted) {
            return std::unexpected(BootErr::Ownership);
        }
        mm::PageGroup pages = std::move(adopted).value();
        const usize size = pages.page_count() * mm::page_size;
        const mm::Extent extent{
            .object = mm::ObjectRange{0, pages.page_count()},
            .physical = module.pages,
            .perms = mm::Perms::of(mm::Perm::Read),
        };
        auto pool_ref = pool.erase();
        if (!pool_ref) {
            return std::unexpected(BootErr::InvalidState);
        }
        auto sponsorship = pool->reserve(std::move(pool_ref).value(), charge_pages(1));
        if (!sponsorship) {
            return std::unexpected(BootErr::OutOfMemory);
        }
        auto image = kernel.pool<mm::Mem>().create(std::move(sponsorship).value(), pmm, size,
                                                   mm::PhysCfg{{&extent, 1}, std::move(pages)});
        if (!image) {
            return std::unexpected(BootErr::OutOfMemory);
        }

        bundle_mem = std::move(image).value().publish();
    }

    object::ref<mm::Mem> stack, info, ipc_mem;
    object::ref<mm::VSpace> vspace;
    object::ref<cap::CSpace> cspace;
    object::ref<Thread> thread;
    object::ref<sched::Sc> sc;
    auto reserve =
        [&](resource::budget charge) noexcept -> std::expected<resource::Reservation, BootErr> {
        auto pool_ref = pool.erase();
        if (!pool_ref) {
            return std::unexpected(BootErr::InvalidState);
        }
        auto reserved = pool->reserve(std::move(pool_ref).value(), charge);
        return reserved
                   ? std::expected<resource::Reservation, BootErr>{(std::move(reserved).value())}
                   : std::expected<resource::Reservation, BootErr>{
                         std::unexpected(BootErr::OutOfMemory)};
    };

    // Every boot object follows the same reserve/create/publish transaction.
    auto create = [&]<class T>(object::ref<T>& dst, resource::budget budget,
                               auto&&... args) noexcept -> bool {
        auto charge = reserve(budget);
        if (!charge)
            return false;
        auto pending = kernel.pool<T>().create(std::move(charge).value(),
                                               std::forward<decltype(args)>(args)...);
        if (!pending)
            return false;
        dst = std::move(pending).value().publish();
        return true;
    };
    const CpuId cpu = runtime.local.descriptor->logical_id();

    if (!create(vspace, charge_pages(64), pmm, kernel.kernel_vspace(), kernel.space_work()) ||
        !create(cspace, charge_pages(17, (mm::page_size - sizeof(BootHdr)) / sizeof(BootCap)), pmm))
        return std::unexpected(BootErr::OutOfMemory);
    {
        const mm::VRange stack_range{root_stack_address, root_stack_size};
        const mm::VRange info_range{root_info_address, mm::page_size};
        for (usize index = 0; index < package.segment_count(); ++index) {
            auto decoded = package.segment(index);
            if (!decoded) {
                return std::unexpected(BootErr::InvalidBundle);
            }
            const BundleSegment segment = decoded.value();
            const auto size = page_round(segment.memory_size);
            if (!size) {
                return std::unexpected(BootErr::InvalidBundle);
            }
            const mm::VRange range{mm::Virt{segment.virtual_address}, *size};
            if (range.intersects(stack_range) || range.intersects(info_range) ||
                range.intersects(mm::VRange{root_ipc_address, mm::page_size})) {
                return std::unexpected(BootErr::InvalidBundle);
            }
            object::ref<mm::Mem> hold;
            if (!create(hold, charge_pages(1 + *size / mm::page_size), pmm, *size,
                        mm::AnonCfg{.perms = segment.perms, .eager = true}))
                return std::unexpected(BootErr::OutOfMemory);
            if (!write_memory(kernel.pmm(), hold.get(), segment.file)) {
                libk_assert(hold.retire());
                hold.reset();
                return std::unexpected(BootErr::OutOfMemory);
            }
            if (segment.perms.contains(mm::Perm::Execute) && !hold->seal()) {
                libk_assert(hold.retire());
                hold.reset();
                return std::unexpected(BootErr::InvalidState);
            }
            if (!map_memory(vspace.get(), cpu, hold, mm::Virt{segment.virtual_address},
                            segment.perms)) {
                libk_assert(hold.retire());
                hold.reset();
                return std::unexpected(BootErr::MappingFailed);
            }
            // The mapping now owns the segment; no boot-side lifetime mirror.
        }
    }

    const auto rw = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);
    const mm::AnonCfg anon{.perms = rw, .eager = true};
    if (!create(stack, charge_pages(1 + root_stack_pages), pmm, root_stack_size, anon) ||
        !create(info, charge_pages(2), pmm, mm::page_size, anon) ||
        !create(ipc_mem, charge_pages(2), pmm, mm::page_size, anon))
        return std::unexpected(BootErr::OutOfMemory);
    if (!map_memory(vspace.get(), cpu, stack, root_stack_address,
                    mm::Perms::of(mm::Perm::Read, mm::Perm::Write)) ||
        !map_memory(vspace.get(), cpu, info, root_info_address, mm::Perms::of(mm::Perm::Read)) ||
        !map_memory(vspace.get(), cpu, ipc_mem, root_ipc_address,
                    mm::Perms::of(mm::Perm::Read, mm::Perm::Write))) {
        return std::unexpected(BootErr::MappingFailed);
    }

    {
        auto thread_charge = reserve(object::pool<Thread>::slot_charge());
        auto kernel_stack_charge = reserve(resource::budget{.memory = mm::Stack::StackBytes});
        if (!thread_charge || !kernel_stack_charge)
            return std::unexpected(BootErr::OutOfMemory);
        auto stack_capacity = std::move(kernel_stack_charge).value();
        auto home = mm::Stack::create(kernel.kernel_vspace());
        auto execution_vspace = vspace.erase();
        auto execution_cspace = cspace.erase();
        if (!home || !execution_vspace || !execution_cspace)
            return std::unexpected(BootErr::OutOfMemory);
        auto ipc_reference = ipc_mem.erase();
        if (!ipc_reference)
            return std::unexpected(BootErr::InvalidState);
        auto ipc_buffer = ipc::Buffer::bind(
            kernel.pmm(), vspace.get(), std::move(ipc_reference).value(), ipc_mem.get(),
            mm::ObjectRange{0, 1}, mm::VRange{root_ipc_address, mm::page_size});
        if (!ipc_buffer)
            return std::unexpected(BootErr::MappingFailed);
        auto execution =
            Env::user(std::move(execution_vspace).value(), std::move(execution_cspace).value(),
                      std::move(ipc_buffer).value());
        if (!execution)
            return std::unexpected(BootErr::InvalidState);
        auto pending_thread = kernel.pool<Thread>().create(
            std::move(thread_charge).value(), std::move(stack_capacity).commit(),
            std::move(home).value(), std::move(execution).value(),
            Thread::UserStart{
                .entry = mm::Virt{package.entry()},
                .stack = mm::Virt{root_stack_address.raw() + root_stack_size},
                .arguments = {root_info_address.raw(), mm::page_size},
            });
        if (!pending_thread)
            return std::unexpected(BootErr::OutOfMemory);
        thread = std::move(pending_thread).value().publish();
    }

    {
        // Write directly into its backing page, with no second boot-stack copy.
        auto info_lease = info->materialize(0);
        if (!info_lease)
            return std::unexpected(BootErr::OutOfMemory);
        auto& info_page = *libk::construct_at(
            reinterpret_cast<BootHdr*>(kernel.pmm().bytes(info_lease.value().page())));
        info_page.magic = BOOT_MAGIC;
        info_page.major = BOOT_MAJOR;
        info_page.minor = BOOT_MINOR;
        info_page.size = sizeof(BootHdr);
        info_page.cpu_count = kernel.cpus().count();
        info_page.stack_base = root_stack_address.raw();
        info_page.stack_size = root_stack_size;
        info_page.boot_bundle_size = module.size;
        auto* entries = reinterpret_cast<BootCap*>(&info_page + 1);
        auto publish = [&](BootCap entry, object::ref<>&& ref, cap::View view) noexcept -> bool {
            if (!ref || sizeof(info_page) + (info_page.count + 1) * sizeof(BootCap) > mm::page_size)
                return false;
            auto charge = reserve(kernel.grants().node_charge());
            if (!charge)
                return false;
            auto installed = install_cap(kernel, cspace.get(), std::move(charge).value(),
                                         std::move(ref), view.rights, view.data);
            if (!installed)
                return false;
            entry.handle = installed->raw();
            libk::construct_at(entries + info_page.count++, entry);
            info_page.size += sizeof(BootCap);
            return true;
        };
        auto add_cap = [&](u32 role, auto&& ref, cap::Rights rights,
                           cap::Limits limit = {}) -> bool {
            if (!ref)
                return false;
            BootCap entry{};
            entry.role = role;
            entry.kind = static_cast<u16>(ref->kind());
            return publish(entry, std::move(ref).value(), {rights, limit});
        };

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
            .range = mm::VRange{mm::Virt{mm::UserBegin}, mm::UserEnd - mm::UserBegin},
            .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write, mm::Perm::Execute),
        };
        const cap::MemLimit bundle_limit{
            .range = mm::ObjectRange{0, bundle_mem->page_count()},
            .perms = mm::Perms::of(mm::Perm::Read),
        };
        constexpr u64 resource_kinds = OBJECT_KINDS;
        const cap::Quota pool_limit{
            .budget = pool->limit(),
            .object_kinds = resource_kinds,
        };

        if (!virt_caps(BootCaps::bind(publish)))
            return std::unexpected(BootErr::CapabilityFailed);

        if (!add_cap(BOOT_VSPACE, vspace.erase(), vspace_rights, vm_limit) ||
            !add_cap(BOOT_CSPACE, cspace.erase(), cspace_rights) ||
            !add_cap(BOOT_BUNDLE, bundle_mem.erase(),
                     cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Map,
                                     cap::Right::Inspect, cap::Right::Revoke),
                     bundle_limit) ||
            !add_cap(BOOT_THREAD, thread.erase(), basic_rights) ||
            !add_cap(BOOT_POOL, pool.erase(), pool_rights, pool_limit)) {
            return std::unexpected(BootErr::CapabilityFailed);
        }

        const auto budget = kernel.clock().duration_from_nanoseconds(2'000'000);
        const auto period = kernel.clock().duration_from_nanoseconds(10'000'000);
        const auto urgency = sched::Urgency::make(20);
        if (!budget || !period || !urgency) {
            return std::unexpected(BootErr::SchedulingFailed);
        }
        if (!create(sc, charge_pages(1),
                    sched::Sc::Config{.budget = *budget, .period = *period, .urgency = *urgency},
                    kernel.clock().now()))
            return std::unexpected(BootErr::OutOfMemory);
        if (!add_cap(BOOT_SC, sc.erase(), basic_rights) ||
            !add_cap(BOOT_DOMAIN, kernel.kernel_domain_ref(), basic_rights)) {
            return std::unexpected(BootErr::CapabilityFailed);
        }
    }

    auto target = thread.clone();
    const auto admitted = kernel.kernel_domain().admit(sc.get(), cpu);
    if (!admitted || !target || !sc->bind(std::move(target).value())) {
        return std::unexpected(BootErr::SchedulingFailed);
    }
    if (!runtime.dispatcher().make_ready(sc.get())) {
        return std::unexpected(BootErr::SchedulingFailed);
    }
    return {};
}
