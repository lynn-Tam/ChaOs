#include <init/root_task.hpp>
#include <cap/authority.hpp>
#include <cap/cspace.hpp>
#include <cap/rights.hpp>
#include <core/kernel_state.hpp>
#include <cpu/cpu_runtime.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/mem.h>
#include <libk/utility.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/memory_object.hpp>
#include <mm/object_range.hpp>
#include <mm/virtual_layout.hpp>
#include <mm/vspace.hpp>
#include <resource/traits.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <execution/binding.hpp>
#include <thread/thread.hpp>
#include <uapi/bootstrap.h>

namespace {

using kernel::init::RootTaskError;

constexpr usize root_stack_pages = 8;
constexpr usize root_stack_size = root_stack_pages * kernel::mm::page_size;
constexpr kernel::mm::VirtAddr root_info_address{
    kernel::mm::layout::UserEnd - 2 * kernel::mm::page_size};
constexpr kernel::mm::VirtAddr root_stack_address{
    root_info_address.raw() - kernel::mm::page_size - root_stack_size};

[[nodiscard]] constexpr auto charge_pages(
    usize pages,
    u64 caps = 0) noexcept -> kernel::resource::Budget {
    return kernel::resource::Budget{
        .memory = static_cast<u64>(pages) * kernel::mm::page_size,
        .caps = caps,
    };
}

[[nodiscard]] constexpr auto page_round(usize size) noexcept
    -> libk::optional<usize> {
    const auto adjusted = libk::checked_add(
        size, kernel::mm::page_size - 1);
    return adjusted
        ? libk::optional<usize>{
              *adjusted & ~(kernel::mm::page_size - 1)}
        : libk::nullopt;
}

[[nodiscard]] auto write_memory(
    kernel::mm::Pmm& pmm,
    kernel::mm::MemoryObject& memory,
    libk::ByteSpan bytes) noexcept -> bool {
    if (bytes.size() > memory.size()) {
        return false;
    }
    usize copied{};
    for (usize index = 0; index < memory.page_count(); ++index) {
        auto page = memory.materialize(index);
        if (!page) {
            return false;
        }
        byte* const destination = pmm.bytes(page.value().page().page);
        const usize remaining = bytes.size() - copied;
        const usize amount = remaining < kernel::mm::page_size
            ? remaining
            : kernel::mm::page_size;
        if (amount != 0) {
            memcpy(destination, bytes.data() + copied, amount);
            copied += amount;
        }
        if (amount != kernel::mm::page_size) {
            memset(destination + amount, 0, kernel::mm::page_size - amount);
        }
    }
    return copied == bytes.size();
}

[[nodiscard]] auto map_memory(
    kernel::mm::VSpace& vspace,
    kernel::CpuId cpu,
    kernel::object::ObjectStore::MemoryHold& memory,
    kernel::mm::VirtAddr address,
    kernel::mm::AccessMask access) noexcept -> bool {
    auto reference = memory.ref();
    if (!reference) {
        return false;
    }
    const kernel::mm::MemoryTypes types = kernel::mm::MemoryTypes::of(
        kernel::mm::MemoryType::Normal);
    const kernel::mm::ObjectRange object{0, memory->page_count()};
    const auto mapped = vspace.map_kernel(
        kernel::mm::VmContext{.local = cpu},
        vspace.root_key(),
        kernel::mm::MapRequest{
            .virtual_range = kernel::mm::VirtRange{address, memory->size()},
            .object = object,
            .access = access,
        },
        libk::move(reference).value(),
        memory.get(),
        kernel::cap::MemoryAuthority{object, access, types});
    return mapped && mapped.value().status == kernel::mm::VmStatus::Complete;
}

[[nodiscard]] auto install_cap(
    kernel::KernelState& kernel,
    kernel::cap::CSpace& cspace,
    kernel::resource::Reservation&& charge,
    kernel::object::ObjectRef&& object,
    kernel::cap::Rights rights,
    kernel::cap::AuthorityData authority = {}) noexcept
    -> libk::Expected<kernel::cap::CapHandle, RootTaskError> {
    auto grant = kernel.grants().create_root(
        libk::move(charge),
        libk::move(object),
        kernel::cap::GrantCeiling{rights, authority});
    if (!grant) {
        return libk::unexpected(RootTaskError::CapabilityFailed);
    }
    auto cap = cspace.insert(
        libk::move(grant).value(), kernel::cap::CapView{rights, authority});
    return cap
        ? libk::Expected<kernel::cap::CapHandle, RootTaskError>{
              libk::expected(cap.value())}
        : libk::Expected<kernel::cap::CapHandle, RootTaskError>{
              libk::unexpected(RootTaskError::CapabilityFailed)};
}

} // namespace

namespace kernel::init {

auto RootTask::initialize_in(
    libk::ManualLifetime<RootTask>& storage,
    kernel::object::ObjectStore& objects,
    kernel::mm::Pmm& pmm,
    kernel::mm::DirectMap& direct_map,
    kernel::object::ObjectStore::ResourceHold&& pool,
    kernel::boot::BootModule module,
    kernel::mm::BootReservation&& reservation) noexcept
    -> libk::Expected<void, RootTaskError> {
    if (!pool || !module || module.kind != kernel::boot::BootModuleKind::Bundle
        || !module.physical.is_aligned(kernel::mm::page_size)
        || module.pages.first().base() != module.physical
        || !reservation || reservation.range().first() != module.pages.first()
        || reservation.range().page_count() != module.pages.page_count()) {
        return libk::unexpected(RootTaskError::InvalidModule);
    }
    const auto source = direct_map.ptr<const byte>(
        module.physical, module.size);
    if (!source) {
        return libk::unexpected(RootTaskError::InvalidModule);
    }
    const auto parsed = kernel::image::parse_bundle(
        libk::ByteSpan{source.value(), module.size});
    if (!parsed) {
        return libk::unexpected(RootTaskError::InvalidBundle);
    }

    auto adopted = pmm.adopt(libk::move(reservation));
    if (!adopted) {
        return libk::unexpected(RootTaskError::Ownership);
    }
    kernel::mm::OwnedPageGroup pages = libk::move(adopted).value();
    const usize image_size = pages.page_count() * kernel::mm::page_size;
    const kernel::mm::MemoryExtent extent{
        .object = kernel::mm::ObjectRange{0, pages.page_count()},
        .physical = module.pages,
        .access = kernel::mm::AccessMask::of(kernel::mm::Access::Read),
        .type = kernel::mm::MemoryType::Normal,
    };
    auto pool_ref = pool.ref();
    if (!pool_ref) {
        return libk::unexpected(RootTaskError::InvalidState);
    }
    auto sponsorship = pool->reserve(
        libk::move(pool_ref).value(), charge_pages(1));
    if (!sponsorship) {
        return libk::unexpected(RootTaskError::OutOfMemory);
    }
    auto image = objects.create_boot_image_sponsored(
        libk::move(sponsorship).value(),
        image_size,
        libk::Span<const kernel::mm::MemoryExtent>{&extent, 1},
        kernel::mm::BootOwnership::Owned,
        libk::move(pages));
    if (!image) {
        return libk::unexpected(RootTaskError::OutOfMemory);
    }

    RootTask& bootstrap = storage.emplace(
        ConstructionKey{}, direct_map, module);
    bootstrap.pool_ = libk::move(pool);
    bootstrap.image_ = libk::move(image).value().publish();
    return libk::expected();
}

auto RootTask::bundle() const noexcept
    -> libk::Expected<kernel::image::BootBundle, kernel::image::BundleError> {
    const auto source = direct_map_->ptr<const byte>(
        module_.physical, module_.size);
    if (!source) {
        return libk::unexpected(kernel::image::BundleError::Truncated);
    }
    return kernel::image::parse_bundle(
        libk::ByteSpan{source.value(), module_.size});
}

auto RootTask::reserve(kernel::resource::Budget charge) noexcept
    -> libk::Expected<kernel::resource::Reservation, RootTaskError> {
    auto pool_ref = pool_.ref();
    if (!pool_ref) {
        return libk::unexpected(RootTaskError::InvalidState);
    }
    auto reserved = pool_->reserve(libk::move(pool_ref).value(), charge);
    return reserved
        ? libk::Expected<kernel::resource::Reservation, RootTaskError>{
              libk::expected(libk::move(reserved).value())}
        : libk::Expected<kernel::resource::Reservation, RootTaskError>{
              libk::unexpected(RootTaskError::OutOfMemory)};
}

auto RootTask::prepare_bootstrap(kernel::KernelState& kernel) noexcept
    -> libk::Expected<void, RootTaskError> {
    myos_bootstrap_info info_page{};
    info_page.magic = MYOS_BOOTSTRAP_MAGIC;
    info_page.major = MYOS_BOOTSTRAP_MAJOR;
    info_page.minor = MYOS_BOOTSTRAP_MINOR;
    info_page.size = sizeof(myos_bootstrap_info);
    info_page.cpu_count = kernel.cpus().count();
    info_page.stack_base = root_stack_address.raw();
    info_page.stack_size = root_stack_size;
    info_page.boot_bundle_size = module_.size;
    auto add_cap = [&](u32 kind, auto&& reference, kernel::cap::Rights rights,
                       kernel::cap::AuthorityData authority = {}) -> bool {
        if (!reference || info_page.cap_count == MYOS_BOOTSTRAP_MAX_CAPS) {
            return false;
        }
        auto charge = reserve(kernel.grants().node_charge());
        if (!charge) {
            return false;
        }
        auto installed = install_cap(
            kernel,
            cspace_.get(),
            libk::move(charge).value(),
            libk::move(reference).value(),
            rights, authority);
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

    const auto basic_rights = kernel::cap::Rights::of(
        kernel::cap::Right::Duplicate,
        kernel::cap::Right::Delegate,
        kernel::cap::Right::Inspect,
        kernel::cap::Right::Control,
        kernel::cap::Right::Destroy,
        kernel::cap::Right::Revoke);
    const auto pool_rights = kernel::cap::Rights::of(
        kernel::cap::Right::Duplicate,
        kernel::cap::Right::Delegate,
        kernel::cap::Right::Inspect,
        kernel::cap::Right::Create,
        kernel::cap::Right::Split,
        kernel::cap::Right::Close,
        kernel::cap::Right::Revoke);
    const auto cspace_rights = kernel::cap::Rights::of(
        kernel::cap::Right::Duplicate,
        kernel::cap::Right::Delegate,
        kernel::cap::Right::Inspect,
        kernel::cap::Right::Manage,
        kernel::cap::Right::Destroy,
        kernel::cap::Right::Revoke);
    const auto vspace_rights = kernel::cap::Rights::of(
        kernel::cap::Right::Duplicate,
        kernel::cap::Right::Delegate,
        kernel::cap::Right::Reserve,
        kernel::cap::Right::CreateRegion,
        kernel::cap::Right::Map,
        kernel::cap::Right::Unmap,
        kernel::cap::Right::Protect,
        kernel::cap::Right::Inspect,
        kernel::cap::Right::Manage,
        kernel::cap::Right::Revoke);
    const kernel::mm::MemoryTypes normal = kernel::mm::MemoryTypes::of(
        kernel::mm::MemoryType::Normal);
    const kernel::cap::VSpaceAuthority vspace_authority{
        .region = vspace_->root_key(),
        .range = kernel::mm::VirtRange{
            kernel::mm::VirtAddr{kernel::mm::layout::LowGuardEnd},
            kernel::mm::layout::UserEnd - kernel::mm::layout::LowGuardEnd},
        .access = kernel::mm::AccessMask::of(
            kernel::mm::Access::Read,
            kernel::mm::Access::Write,
            kernel::mm::Access::Execute),
        .types = normal,
    };
    const kernel::cap::MemoryAuthority bundle_authority{
        .range = kernel::mm::ObjectRange{0, image_->page_count()},
        .access = kernel::mm::AccessMask::of(kernel::mm::Access::Read),
        .types = normal,
    };
    constexpr u64 resource_kinds =
        ((u64{1} << static_cast<u16>(
            kernel::object::ObjectKind::Count)) - 1)
        & ~u64{1};
    const kernel::cap::ResourcePoolAuthority pool_authority{
        .budget = pool_->limit(),
        .object_kinds = resource_kinds,
    };
    if (!add_cap(
            MYOS_BOOTSTRAP_CAP_VSPACE, vspace_.ref(), vspace_rights,
            vspace_authority)
        || !add_cap(
            MYOS_BOOTSTRAP_CAP_CSPACE, cspace_.ref(), cspace_rights)
        || !add_cap(
            MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE, image_.ref(),
            kernel::cap::Rights::of(
                kernel::cap::Right::Duplicate,
                kernel::cap::Right::Delegate,
                kernel::cap::Right::Map,
                kernel::cap::Right::Inspect,
                kernel::cap::Right::Revoke),
            bundle_authority)
        || !add_cap(
            MYOS_BOOTSTRAP_CAP_THREAD, thread_.ref(), basic_rights)
        || !add_cap(
            MYOS_BOOTSTRAP_CAP_RESOURCE_POOL,
            pool_.ref(),
            pool_rights,
            pool_authority)) {
        return libk::unexpected(RootTaskError::CapabilityFailed);
    }

    const auto budget = kernel.clock().duration_from_nanoseconds(2'000'000);
    const auto period = kernel.clock().duration_from_nanoseconds(10'000'000);
    const auto urgency = kernel::sched::Urgency::make(20);
    if (!budget || !period || !urgency) {
        return libk::unexpected(RootTaskError::SchedulingFailed);
    }
    auto context_charge = reserve(charge_pages(1));
    if (!context_charge) {
        return libk::unexpected(RootTaskError::OutOfMemory);
    }
    auto pending_context = kernel.objects().create_context_sponsored(
        libk::move(context_charge).value(),
        kernel::sched::SchedulingContext::Config{
            .budget = *budget,
            .period = *period,
            .urgency = *urgency,
        },
        kernel.clock().now());
    if (!pending_context) {
        return libk::unexpected(RootTaskError::OutOfMemory);
    }
    context_ = libk::move(pending_context).value().publish();
    if (!add_cap(
            MYOS_BOOTSTRAP_CAP_SCHED_CONTEXT, context_.ref(), basic_rights)
        || !add_cap(
            MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN,
            kernel.kernel_domain_ref(), basic_rights)) {
        return libk::unexpected(RootTaskError::CapabilityFailed);
    }
    if (!write_memory(
            kernel.pmm(), info_.get(),
            libk::ByteSpan{
                reinterpret_cast<const byte*>(&info_page),
                sizeof(info_page)})) {
        return libk::unexpected(RootTaskError::OutOfMemory);
    }
    return libk::expected();
}

auto RootTask::start(
    kernel::KernelState& kernel,
    kernel::CpuRuntime& runtime) noexcept
    -> libk::Expected<void, RootTaskError> {
    if (started_ || !image_ || segments_.size() != 0 || stack_ || info_
        || vspace_ || cspace_ || thread_ || context_
        || arch::interrupts_enabled()) {
        return libk::unexpected(RootTaskError::InvalidState);
    }
    auto parsed = bundle();
    if (!parsed) {
        return libk::unexpected(RootTaskError::InvalidBundle);
    }
    const kernel::image::BootBundle package = parsed.value();
    const kernel::CpuId cpu = runtime.local.descriptor->logical_id();
    const kernel::mm::VirtRange stack_range{root_stack_address, root_stack_size};
    const kernel::mm::VirtRange info_range{
        root_info_address, kernel::mm::page_size};

    auto fail = [&](RootTaskError error)
        -> libk::Expected<void, RootTaskError> {
        rollback(kernel);
        return libk::unexpected(error);
    };

    auto space_charge = reserve(charge_pages(64));
    auto cspace_charge = reserve(charge_pages(17, MYOS_BOOTSTRAP_MAX_CAPS));
    if (!space_charge || !cspace_charge) {
        return fail(RootTaskError::OutOfMemory);
    }
    auto space = kernel.objects().create_vspace_sponsored(
        libk::move(space_charge).value(), kernel.kernel_vspace());
    auto cspace = kernel.objects().create_cspace_sponsored(
        libk::move(cspace_charge).value());
    if (!space || !cspace) {
        return fail(RootTaskError::OutOfMemory);
    }
    vspace_ = libk::move(space).value().publish();
    cspace_ = libk::move(cspace).value().publish();

    for (usize index = 0; index < package.segment_count(); ++index) {
        auto decoded = package.segment(index);
        if (!decoded) {
            return fail(RootTaskError::InvalidBundle);
        }
        const kernel::image::BundleSegment segment = decoded.value();
        const auto size = page_round(segment.memory_size);
        if (!size) {
            return fail(RootTaskError::InvalidBundle);
        }
        const kernel::mm::VirtRange range{
            kernel::mm::VirtAddr{segment.virtual_address}, *size};
        if (range.intersects(stack_range) || range.intersects(info_range)) {
            return fail(RootTaskError::InvalidBundle);
        }
        auto memory_charge = reserve(charge_pages(1 + *size / kernel::mm::page_size));
        if (!memory_charge) {
            return fail(RootTaskError::OutOfMemory);
        }
        auto memory = kernel.objects().create_anonymous_sponsored(
            libk::move(memory_charge).value(),
            *size,
            kernel::mm::AnonymousConfig{
                .access = segment.access,
                .eager = true,
            });
        if (!memory) {
            return fail(RootTaskError::OutOfMemory);
        }
        auto hold = libk::move(memory).value().publish();
        if (!write_memory(kernel.pmm(), hold.get(), segment.file)) {
            KASSERT(hold.retire());
            hold.reset();
            return fail(RootTaskError::OutOfMemory);
        }
        if (segment.access.contains(kernel::mm::Access::Execute)
            && !hold->seal()) {
            KASSERT(hold.retire());
            hold.reset();
            return fail(RootTaskError::InvalidState);
        }
        if (!map_memory(
                vspace_.get(), cpu, hold,
                kernel::mm::VirtAddr{segment.virtual_address},
                segment.access)) {
            KASSERT(hold.retire());
            hold.reset();
            return fail(RootTaskError::MappingFailed);
        }
        KASSERT(segments_.try_push_back(libk::move(hold)));
    }

    auto stack_charge = reserve(charge_pages(1 + root_stack_pages));
    auto info_charge = reserve(charge_pages(2));
    if (!stack_charge || !info_charge) {
        return fail(RootTaskError::OutOfMemory);
    }
    auto stack = kernel.objects().create_anonymous_sponsored(
        libk::move(stack_charge).value(),
        root_stack_size,
        kernel::mm::AnonymousConfig{
            .access = kernel::mm::AccessMask::of(
                kernel::mm::Access::Read, kernel::mm::Access::Write),
            .eager = true,
        });
    auto info = kernel.objects().create_anonymous_sponsored(
        libk::move(info_charge).value(),
        kernel::mm::page_size,
        kernel::mm::AnonymousConfig{
            .access = kernel::mm::AccessMask::of(
                kernel::mm::Access::Read, kernel::mm::Access::Write),
            .eager = true,
        });
    if (!stack || !info) {
        return fail(RootTaskError::OutOfMemory);
    }
    stack_ = libk::move(stack).value().publish();
    info_ = libk::move(info).value().publish();
    if (!map_memory(
            vspace_.get(), cpu, stack_, root_stack_address,
            kernel::mm::AccessMask::of(
                kernel::mm::Access::Read, kernel::mm::Access::Write))
        || !map_memory(
            vspace_.get(), cpu, info_, root_info_address,
            kernel::mm::AccessMask::of(kernel::mm::Access::Read))) {
        return fail(RootTaskError::MappingFailed);
    }

    auto thread_charge = reserve(
        kernel::resource::Traits<kernel::Thread>::fixed());
    auto kernel_stack_charge = reserve(kernel::resource::Budget{
        .memory = kernel::mm::KernelStackLayout::StackBytes});
    if (!thread_charge || !kernel_stack_charge) {
        return fail(RootTaskError::OutOfMemory);
    }
    auto stack_capacity = libk::move(kernel_stack_charge).value();
    auto home = kernel::KernelStack::create(kernel.kernel_vspace());
    auto execution_vspace = vspace_.ref();
    auto execution_cspace = cspace_.ref();
    if (!home || !execution_vspace || !execution_cspace) {
        return fail(RootTaskError::OutOfMemory);
    }
    auto execution = kernel::ExecutionBinding::user(
        libk::move(execution_vspace).value(),
        libk::move(execution_cspace).value());
    if (!execution) {
        return fail(RootTaskError::InvalidState);
    }
    auto pending_thread = kernel.objects().create_thread_sponsored(
        libk::move(thread_charge).value(),
        libk::move(stack_capacity).commit(),
        libk::move(home).value(),
        libk::move(execution).value(),
        kernel::Thread::UserStart{
            .entry = kernel::mm::VirtAddr{package.entry()},
            .stack = kernel::mm::VirtAddr{
                root_stack_address.raw() + root_stack_size},
            .arguments = {
                root_info_address.raw(), sizeof(myos_bootstrap_info)},
        });
    if (!pending_thread) {
        return fail(RootTaskError::OutOfMemory);
    }
    thread_ = libk::move(pending_thread).value().publish();

    const auto prepared = prepare_bootstrap(kernel);
    if (!prepared) {
        return fail(prepared.error());
    }

    auto target = thread_.clone();
    const auto admitted = kernel.kernel_domain().admit(context_.get(), cpu);
    if (!admitted || !target
        || !context_->bind(libk::move(target).value())) {
        return fail(RootTaskError::SchedulingFailed);
    }
    if (!runtime.dispatcher().make_ready(*context_->binding())) {
        return fail(RootTaskError::SchedulingFailed);
    }
    started_ = true;
    return libk::expected();
}

void RootTask::rollback(kernel::KernelState& kernel) noexcept {
    if (context_) {
        if (context_->binding() != nullptr) {
            KASSERT(context_->unbind());
        }
        if (context_->admitted()) {
            KASSERT(kernel.kernel_domain().unadmit(context_.get()));
        }
        KASSERT(context_.retire());
        context_.reset();
    }
    if (thread_) {
        KASSERT(thread_.retire());
        thread_.reset();
    }
    kernel.objects().drain_reclaim();
    if (cspace_) {
        KASSERT(cspace_.retire());
        cspace_.reset();
    }
    if (vspace_) {
        KASSERT(vspace_.retire());
        vspace_.reset();
    }
    if (info_) {
        KASSERT(info_.retire());
        info_.reset();
    }
    if (stack_) {
        KASSERT(stack_.retire());
        stack_.reset();
    }
    for (auto& segment : segments_) {
        KASSERT(segment.retire());
        segment.reset();
    }
    segments_.clear();
    kernel.objects().drain_reclaim();
    started_ = false;
}

} // namespace kernel::init
