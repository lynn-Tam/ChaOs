#include <expected>
#include <optional>
#include <platform/riscv-virt/board.hpp>
#include <platform/riscv-virt/config.hpp>
#include <object/pool.hpp>
#include <boot/root.hpp>
#include <cap/cap.hpp>
#include <cap/cspace.hpp>
#include <arch/uart.hpp>
#include <arch/time.hpp>
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
#include <uapi/bootstrap.h>

namespace {

constexpr usize root_stack_pages = 8;
constexpr mm::Virt root_ipc_address{MYOS_BOOTSTRAP_ROOT_IPC_ADDRESS};
constexpr usize root_stack_size = root_stack_pages * mm::page_size;
constexpr mm::Virt root_info_address{mm::UserEnd - 2 * mm::page_size};
constexpr mm::Virt root_stack_address{root_info_address.raw() - mm::page_size - root_stack_size};

[[nodiscard]] constexpr auto charge_pages(usize pages, u64 caps = 0) noexcept -> resource::budget {
    return resource::budget{
        .memory = static_cast<u64>(pages) * mm::page_size,
        .caps = caps,
    };
}

[[nodiscard]] constexpr auto page_round(usize size) noexcept -> std::optional<usize> {
    const auto adjusted = libk::checked_add(size, mm::page_size - 1);
    return adjusted ? std::optional<usize>{*adjusted & ~(mm::page_size - 1)} : std::nullopt;
}

[[nodiscard]] auto write_memory(mm::Pmm& pmm, mm::Mem& memory, libk::ByteSpan bytes) noexcept -> bool {
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

[[nodiscard]] auto map_memory(mm::VSpace& vspace, CpuId cpu, object::ref<mm::Mem>& memory, mm::Virt address,
                              mm::Perms access) noexcept -> bool {
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

[[nodiscard]] auto install_cap(KernelState& kernel, cap::CSpace& cspace, resource::Reservation&& charge,
                               object::ref<>&& object, cap::Rights rights,
                               cap::Limits authority = {}) noexcept
    -> std::expected<cap::Handle, RootTaskError> {
    auto grant =
        kernel.grants().create_root(std::move(charge), std::move(object), cap::View{rights, authority});
    if (!grant) {
        return std::unexpected(RootTaskError::CapabilityFailed);
    }
    auto cap = cspace.insert(std::move(grant).value(), cap::View{rights, authority});
    return cap ? std::expected<cap::Handle, RootTaskError>{(cap.value())}
               : std::expected<cap::Handle, RootTaskError>{std::unexpected(RootTaskError::CapabilityFailed)};
}

} // namespace

auto RootTask::initialize_in(libk::ManualLifetime<RootTask>& storage, object::pool<mm::Mem>& memory,
                             mm::Pmm& pmm, object::ref<object::group>&& pool,
                             BootModule module, mm::BootPages&& reservation) noexcept
    -> std::expected<void, RootTaskError> {
    if (!pool || !module || !module.physical.is_aligned(mm::page_size) ||
        module.pages.base().base() != module.physical || !reservation ||
        reservation.range().base() != module.pages.base() ||
        reservation.range().page_count() != module.pages.page_count()) {
        return std::unexpected(RootTaskError::InvalidModule);
    }
    const auto source = pmm.ptr<const byte>(module.physical, module.size);
    if (!source) {
        return std::unexpected(RootTaskError::InvalidModule);
    }
    const auto parsed = parse_bundle(libk::ByteSpan{source.value(), module.size});
    if (!parsed) {
        return std::unexpected(RootTaskError::InvalidBundle);
    }

    auto adopted = pmm.adopt(std::move(reservation));
    if (!adopted) {
        return std::unexpected(RootTaskError::Ownership);
    }
    mm::PageGroup pages = std::move(adopted).value();
    const usize image_size = pages.page_count() * mm::page_size;
    const mm::Extent extent{
        .object = mm::ObjectRange{0, pages.page_count()},
        .physical = module.pages,
        .perms = mm::Perms::of(mm::Perm::Read),
    };
    auto pool_ref = pool.erase();
    if (!pool_ref) {
        return std::unexpected(RootTaskError::InvalidState);
    }
    auto sponsorship = pool->reserve(std::move(pool_ref).value(), charge_pages(1));
    if (!sponsorship) {
        return std::unexpected(RootTaskError::OutOfMemory);
    }
    auto image = memory.create(std::move(sponsorship).value(), pmm, image_size, mm::PhysCfg{{&extent, 1}, std::move(pages)});
    if (!image) {
        return std::unexpected(RootTaskError::OutOfMemory);
    }

    RootTask& bootstrap = storage.emplace(ConstructionKey{}, pmm, module);
    bootstrap.pool_ = std::move(pool);
    bootstrap.package_ = std::move(image).value().publish();
    return {};
}

auto RootTask::bundle() const noexcept -> std::expected<BootBundle, BundleError> {
    const auto source = pmm_->ptr<const byte>(module_.physical, module_.size);
    if (!source) {
        return std::unexpected(BundleError::Truncated);
    }
    return parse_bundle(libk::ByteSpan{source.value(), module_.size});
}

auto RootTask::reserve(resource::budget charge) noexcept
    -> std::expected<resource::Reservation, RootTaskError> {
    auto pool_ref = pool_.erase();
    if (!pool_ref) {
        return std::unexpected(RootTaskError::InvalidState);
    }
    auto reserved = pool_->reserve(std::move(pool_ref).value(), charge);
    return reserved ? std::expected<resource::Reservation, RootTaskError>{(std::move(reserved).value())}
                    : std::expected<resource::Reservation, RootTaskError>{
                          std::unexpected(RootTaskError::OutOfMemory)};
}

auto RootTask::prepare_bootstrap(KernelState& kernel) noexcept -> std::expected<void, RootTaskError> {
    // Construct the envelope in its final private page. Its import capacity
    // must not grow the boot stack or require a second full-page copy.
    auto info_lease = info_->materialize(0);
    if (!info_lease) return std::unexpected(RootTaskError::OutOfMemory);
    auto& info_page = *libk::construct_at(
        reinterpret_cast<myos_bootstrap_info*>(kernel.pmm().bytes(info_lease.value().page())));
    info_page.magic = MYOS_BOOTSTRAP_MAGIC;
    info_page.major = MYOS_BOOTSTRAP_MAJOR;
    info_page.minor = MYOS_BOOTSTRAP_MINOR;
    info_page.size = sizeof(myos_bootstrap_info);
    info_page.cpu_count = kernel.cpus().count();
    info_page.stack_base = root_stack_address.raw();
    info_page.stack_size = root_stack_size;
    info_page.boot_bundle_size = module_.size;
    info_page.cap_count = 0;
    info_page.import_count = 0;
    info_page.reserved = 0;
    auto add_cap = [&](u32 kind, auto&& reference, cap::Rights rights, cap::Limits authority = {}) -> bool {
        if (!reference || info_page.cap_count == MYOS_BOOTSTRAP_MAX_CAPS) {
            return false;
        }
        auto charge = reserve(kernel.grants().node_charge());
        if (!charge) {
            return false;
        }
        auto installed = install_cap(kernel, cspace_.get(), std::move(charge).value(),
                                     std::move(reference).value(), rights, authority);
        if (!installed) {
            return false;
        }
        info_page.caps[info_page.cap_count++] = myos_bootstrap_cap{
            .kind = kind,
            .flags = 0,
            .handle = installed.value().raw(),
        };
        return true;
    };

    const auto basic_rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Control,
                        cap::Right::Destroy, cap::Right::Revoke);
    const auto pool_rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Create,
                        cap::Right::Split, cap::Right::Close, cap::Right::Revoke);
    const auto cspace_rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Manage,
                        cap::Right::Destroy, cap::Right::Revoke);
    const auto vspace_rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Reserve, cap::Right::Map,
                        cap::Right::Unmap, cap::Right::Destroy, cap::Right::Protect, cap::Right::Inspect,
                        cap::Right::Manage, cap::Right::Revoke);
    const cap::VmLimit vspace_authority{
        .range = mm::VRange{mm::Virt{mm::UserBegin}, mm::UserEnd - mm::UserBegin},
        .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write, mm::Perm::Execute),
    };
    const cap::MemLimit bundle_authority{
        .range = mm::ObjectRange{0, package_->page_count()},
        .perms = mm::Perms::of(mm::Perm::Read),
    };
    constexpr u64 resource_kinds = MYOS_OBJECT_KINDS;
    const cap::Quota pool_authority{
        .budget = pool_->limit(),
        .object_kinds = resource_kinds,
    };

    // The platform owns the UART identity.  Root init receives these exact
    // resources as bootstrap capabilities; it must delegate them to the UART
    // service instead of manufacturing a source number or physical mapping.
    const auto uart_physical = mm::Pages::from_aligned_bytes(mm::Phys{VirtUartBase}, mm::page_size);
    libk_assert(uart_physical);
    const mm::Extent uart_extent{
        .object = mm::ObjectRange{0, 1},
        .physical = *uart_physical,
        .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write),
    };
    auto uart_memory_charge = reserve(object::pool<mm::Mem>::slot_charge());
    auto uart_memory = uart_memory_charge
                           ? kernel.pool<mm::Mem>().create(std::move(uart_memory_charge).value(), kernel.pmm(), mm::page_size, mm::PhysCfg{{&uart_extent, 1}, {}})
                           : std::expected<object::pool<mm::Mem>::pending, mm::MemErr>{
                                 std::unexpected(mm::MemErr::OutOfMemory)};
    if (!uart_memory) {
        return std::unexpected(RootTaskError::OutOfMemory);
    }
    uart_memory_ = std::move(uart_memory).value().publish();

    auto uart_irq_charge = reserve(object::pool<irq::Irq>::slot_charge());
    auto uart_irq = uart_irq_charge ? kernel.pool<irq::Irq>().create(std::move(uart_irq_charge).value(),
                                                                        virt_uart_irq())
                                    : std::expected<object::pool<irq::Irq>::pending, object::error>{
                                          std::unexpected(object::error::out_of_memory)};
    if (!uart_irq) {
        return std::unexpected(RootTaskError::OutOfMemory);
    }
    uart_irq_ = std::move(uart_irq).value().publish();

    for (usize i = 0; i < virt_device_count(); ++i) {
        if (info_page.import_count == MYOS_BOOTSTRAP_MAX_IMPORTS)
            return std::unexpected(RootTaskError::CapabilityFailed);
        auto reference = virt_device_ref(i);
        if (!reference) return std::unexpected(RootTaskError::InvalidState);
        auto charge = reserve(kernel.grants().node_charge());
        if (!charge) return std::unexpected(RootTaskError::OutOfMemory);
        auto installed =
            install_cap(kernel, cspace_.get(), std::move(charge).value(), std::move(reference).value(),
                        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                                        cap::Right::Connect, cap::Right::Revoke));
        if (!installed) return std::unexpected(RootTaskError::CapabilityFailed);
        auto& entry = info_page.imports[info_page.import_count++];
        entry = {};
        const u16 requester = virt_device(i).requester();
        constexpr char hex[] = "0123456789abcdef";
        entry.name[0] = 'p';
        entry.name[1] = 'c';
        entry.name[2] = 'i';
        entry.name[3] = '.';
        for (usize digit = 0; digit < 4; ++digit)
            entry.name[4 + digit] = hex[(requester >> (12 - digit * 4)) & 15];
        entry.protocol = MYOS_BOOTSTRAP_DEVICE_PROTOCOL;
        entry.major = 1;
        entry.object_kind = MYOS_OBJECT_KIND_DEVICE;
        entry.handle = installed.value().raw();
    }

    const cap::MemLimit uart_memory_authority{
        .range = mm::ObjectRange{0, 1},
        .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write),
    };
    const auto uart_memory_rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate,
                                                    cap::Right::Map, cap::Right::Inspect, cap::Right::Revoke);
    const auto uart_irq_rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Route, cap::Right::Observe,
                        cap::Right::Ack, cap::Right::Inspect, cap::Right::Close, cap::Right::Revoke);
    if (!add_cap(MYOS_BOOTSTRAP_CAP_VSPACE, vspace_.erase(), vspace_rights, vspace_authority) ||
        !add_cap(MYOS_BOOTSTRAP_CAP_CSPACE, cspace_.erase(), cspace_rights) ||
        !add_cap(MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE, package_.erase(),
                 cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Map,
                                 cap::Right::Inspect, cap::Right::Revoke),
                 bundle_authority) ||
        !add_cap(MYOS_BOOTSTRAP_CAP_THREAD, thread_.erase(), basic_rights) ||
        !add_cap(MYOS_BOOTSTRAP_CAP_RESOURCE_POOL, pool_.erase(), pool_rights, pool_authority) ||
        !add_cap(MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY, uart_memory_.erase(), uart_memory_rights,
                 uart_memory_authority) ||
        !add_cap(MYOS_BOOTSTRAP_CAP_IRQ, uart_irq_.erase(), uart_irq_rights,
                 cap::IrqRoute{uart_irq_->source().id, uart_irq_->source().level})) {
        return std::unexpected(RootTaskError::CapabilityFailed);
    }

    const auto budget = kernel.clock().duration_from_nanoseconds(2'000'000);
    const auto period = kernel.clock().duration_from_nanoseconds(10'000'000);
    const auto urgency = sched::Urgency::make(20);
    if (!budget || !period || !urgency) {
        return std::unexpected(RootTaskError::SchedulingFailed);
    }
    auto context_charge = reserve(charge_pages(1));
    if (!context_charge) {
        return std::unexpected(RootTaskError::OutOfMemory);
    }
    auto pending_context = kernel.pool<sched::Sc>().create(std::move(context_charge).value(),
                                                                    sched::Sc::Config{
                                                                        .budget = *budget,
                                                                        .period = *period,
                                                                        .urgency = *urgency,
                                                                    },
                                                                    kernel.clock().now());
    if (!pending_context) {
        return std::unexpected(RootTaskError::OutOfMemory);
    }
    context_ = std::move(pending_context).value().publish();
    if (!add_cap(MYOS_BOOTSTRAP_CAP_SCHED_CONTEXT, context_.erase(), basic_rights) ||
        !add_cap(MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN, kernel.kernel_domain_ref(), basic_rights)) {
        return std::unexpected(RootTaskError::CapabilityFailed);
    }
    return {};
}

auto RootTask::load_segments(KernelState& kernel, const BootBundle& package, CpuId cpu) noexcept
    -> std::expected<void, RootTaskError> {
    const mm::VRange stack_range{root_stack_address, root_stack_size};
    const mm::VRange info_range{root_info_address, mm::page_size};
    for (usize index = 0; index < package.segment_count(); ++index) {
        auto decoded = package.segment(index);
        if (!decoded) {
            return std::unexpected(RootTaskError::InvalidBundle);
        }
        const BundleSegment segment = decoded.value();
        const auto size = page_round(segment.memory_size);
        if (!size) {
            return std::unexpected(RootTaskError::InvalidBundle);
        }
        const mm::VRange range{mm::Virt{segment.virtual_address}, *size};
        if (range.intersects(stack_range) || range.intersects(info_range) ||
            range.intersects(mm::VRange{root_ipc_address, mm::page_size})) {
            return std::unexpected(RootTaskError::InvalidBundle);
        }
        auto memory_charge = reserve(charge_pages(1 + *size / mm::page_size));
        if (!memory_charge) {
            return std::unexpected(RootTaskError::OutOfMemory);
        }
        auto memory = kernel.pool<mm::Mem>().create(std::move(memory_charge).value(), kernel.pmm(), *size, mm::AnonCfg{
                    .perms = segment.perms,
                    .eager = true,
                });
        if (!memory) {
            return std::unexpected(RootTaskError::OutOfMemory);
        }
        auto hold = std::move(memory).value().publish();
        if (!write_memory(kernel.pmm(), hold.get(), segment.file)) {
            libk_assert(hold.retire());
            hold.reset();
            return std::unexpected(RootTaskError::OutOfMemory);
        }
        if (segment.perms.contains(mm::Perm::Execute) && !hold->seal()) {
            libk_assert(hold.retire());
            hold.reset();
            return std::unexpected(RootTaskError::InvalidState);
        }
        if (!map_memory(vspace_.get(), cpu, hold, mm::Virt{segment.virtual_address}, segment.perms)) {
            libk_assert(hold.retire());
            hold.reset();
            return std::unexpected(RootTaskError::MappingFailed);
        }
        libk_assert(segments_.try_push_back(std::move(hold)));
    }
    return {};
}

auto RootTask::create_thread(KernelState& kernel, usize entry) noexcept
    -> std::expected<void, RootTaskError> {
    auto thread_charge = reserve(object::pool<Thread>::slot_charge());
    auto kernel_stack_charge = reserve(resource::budget{.memory = mm::Stack::StackBytes});
    if (!thread_charge || !kernel_stack_charge) return std::unexpected(RootTaskError::OutOfMemory);
    auto stack_capacity = std::move(kernel_stack_charge).value();
    auto home = mm::Stack::create(kernel.kernel_vspace());
    auto execution_vspace = vspace_.erase();
    auto execution_cspace = cspace_.erase();
    if (!home || !execution_vspace || !execution_cspace) return std::unexpected(RootTaskError::OutOfMemory);
    auto ipc_reference = ipc_.erase();
    if (!ipc_reference) return std::unexpected(RootTaskError::InvalidState);
    auto ipc_buffer =
        ipc::Buffer::bind(kernel.pmm(), vspace_.get(), std::move(ipc_reference).value(), ipc_.get(),
                          mm::ObjectRange{0, 1}, mm::VRange{root_ipc_address, mm::page_size});
    if (!ipc_buffer) return std::unexpected(RootTaskError::MappingFailed);
    auto execution = Env::user(std::move(execution_vspace).value(), std::move(execution_cspace).value(),
                               std::move(ipc_buffer).value());
    if (!execution) return std::unexpected(RootTaskError::InvalidState);
    auto pending_thread = kernel.pool<Thread>().create(
        std::move(thread_charge).value(), std::move(stack_capacity).commit(), std::move(home).value(),
        std::move(execution).value(),
        Thread::UserStart{
            .entry = mm::Virt{entry},
            .stack = mm::Virt{root_stack_address.raw() + root_stack_size},
            .arguments = {root_info_address.raw(), sizeof(myos_bootstrap_info)},
        });
    if (!pending_thread) return std::unexpected(RootTaskError::OutOfMemory);
    thread_ = std::move(pending_thread).value().publish();
    return {};
}

auto RootTask::start(KernelState& kernel, CpuRuntime& runtime) noexcept
    -> std::expected<void, RootTaskError> {
    if (started_ || !package_ || segments_.size() != 0 || stack_ || info_ || vspace_ || cspace_ || thread_ ||
        context_ || arch::interrupts_enabled()) {
        return std::unexpected(RootTaskError::InvalidState);
    }
    auto parsed = bundle();
    if (!parsed) {
        return std::unexpected(RootTaskError::InvalidBundle);
    }
    const BootBundle package = parsed.value();
    const CpuId cpu = runtime.local.descriptor->logical_id();

    auto fail = [&](RootTaskError error) -> std::expected<void, RootTaskError> {
        rollback(kernel);
        return std::unexpected(error);
    };

    auto space_charge = reserve(charge_pages(64));
    auto cspace_charge = reserve(charge_pages(17, MYOS_BOOTSTRAP_MAX_CAPS));
    if (!space_charge || !cspace_charge) {
        return fail(RootTaskError::OutOfMemory);
    }
    auto space = kernel.pool<mm::VSpace>().create(
        std::move(space_charge).value(), kernel.pmm(),
        kernel.kernel_vspace(), kernel.space_work());
    auto cspace = kernel.pool<cap::CSpace>().create(std::move(cspace_charge).value(), kernel.pmm());
    if (!space || !cspace) {
        return fail(RootTaskError::OutOfMemory);
    }
    vspace_ = std::move(space).value().publish();
    cspace_ = std::move(cspace).value().publish();
    if (auto loaded = load_segments(kernel, package, cpu); !loaded) return fail(loaded.error());

    auto stack_charge = reserve(charge_pages(1 + root_stack_pages));
    auto info_charge = reserve(charge_pages(2));
    auto ipc_charge = reserve(charge_pages(2));
    if (!stack_charge || !info_charge || !ipc_charge) {
        return fail(RootTaskError::OutOfMemory);
    }
    auto stack = kernel.pool<mm::Mem>().create(std::move(stack_charge).value(), kernel.pmm(), root_stack_size, mm::AnonCfg{
                .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write),
                .eager = true,
            });
    auto info = kernel.pool<mm::Mem>().create(std::move(info_charge).value(), kernel.pmm(), mm::page_size, mm::AnonCfg{
                .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write),
                .eager = true,
            });
    auto ipc_memory = kernel.pool<mm::Mem>().create(std::move(ipc_charge).value(), kernel.pmm(), mm::page_size, mm::AnonCfg{
                .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write),
                .eager = true,
            });
    if (!stack || !info || !ipc_memory) {
        return fail(RootTaskError::OutOfMemory);
    }
    stack_ = std::move(stack).value().publish();
    info_ = std::move(info).value().publish();
    ipc_ = std::move(ipc_memory).value().publish();
    if (!map_memory(vspace_.get(), cpu, stack_, root_stack_address,
                    mm::Perms::of(mm::Perm::Read, mm::Perm::Write)) ||
        !map_memory(vspace_.get(), cpu, info_, root_info_address, mm::Perms::of(mm::Perm::Read)) ||
        !map_memory(vspace_.get(), cpu, ipc_, root_ipc_address,
                    mm::Perms::of(mm::Perm::Read, mm::Perm::Write))) {
        return fail(RootTaskError::MappingFailed);
    }

    if (auto created = create_thread(kernel, package.entry()); !created) return fail(created.error());

    const auto prepared = prepare_bootstrap(kernel);
    if (!prepared) {
        return fail(prepared.error());
    }

    auto target = thread_.clone();
    const auto admitted = kernel.kernel_domain().admit(context_.get(), cpu);
    if (!admitted || !target || !context_->bind(std::move(target).value())) {
        return fail(RootTaskError::SchedulingFailed);
    }
    if (!runtime.dispatcher().make_ready(context_.get())) {
        return fail(RootTaskError::SchedulingFailed);
    }
    started_ = true;
    return {};
}

void RootTask::rollback(KernelState& kernel) noexcept {
    if (context_) {
        if (context_->bound()) {
            libk_assert(context_->unbind());
        }
        if (context_->admitted()) {
            libk_assert(kernel.kernel_domain().unadmit(context_.get()));
        }
        libk_assert(context_.retire());
        context_.reset();
    }
    if (thread_) {
        // The user binding is attached before bootstrap capabilities are
        // installed.  A failed preparation path therefore has to release the
        // execution relation before the Thread object can retire.
        thread_->env().detach_user();
        libk_assert(thread_.retire());
        thread_.reset();
    }
    kernel.drain_reclaim();
    if (cspace_) {
        libk_assert(cspace_.retire());
        cspace_.reset();
    }
    uart_irq_.reset();
    uart_memory_.reset();
    if (vspace_) {
        libk_assert(vspace_.retire());
        vspace_.reset();
    }
    if (info_) {
        libk_assert(info_.retire());
        info_.reset();
    }
    if (ipc_) {
        libk_assert(ipc_.retire());
        ipc_.reset();
    }
    if (stack_) {
        libk_assert(stack_.retire());
        stack_.reset();
    }
    for (auto& segment : segments_) {
        libk_assert(segment.retire());
        segment.reset();
    }
    segments_.clear();
    kernel.drain_reclaim();
    started_ = false;
}
