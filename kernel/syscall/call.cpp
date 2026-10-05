#include <expected>
#include <optional>
#include <mm/mem.hpp>
#include <object/ref.hpp>
#include <syscall/call.hpp>
#include <state.hpp>
#include <cpu/local.hpp>
#include <cpu/registry.hpp>
#include <cpu/runtime.hpp>
#include <libk/checked_arithmetic.hpp>
#include <limits>
#include <uapi/syscall.h>
#include <cap/cap.hpp>
#include <task/thread.hpp>
#include <utility>
#include <object/group.hpp>
#include <ipc/notification.hpp>
#include <resource/sponsorship.hpp>
#include <irq/irq.hpp>
#include <mm/pager.hpp>
#include <uapi/pager.h>
#include <io/space.hpp>
#include <ipc/buffer.hpp>
#include <io/device.hpp>
#include <uapi/io.h>
#include <variant>
#include <uapi/vm.h>
#include <sched/dispatcher.hpp>
#include <uapi/status.h>
#include <sched/sc.hpp>
#include <sched/domain.hpp>

namespace syscall {

template<usize op>
auto clock_now(Call& inv) noexcept -> Result {
    auto* kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel);
    return returned(MYOS_STATUS_OK, kernel->clock().now().ticks());
}

template<usize op>
auto clock_frequency(Call& inv) noexcept -> Result {
    auto* kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel);
    return returned(MYOS_STATUS_OK, kernel->clock().ticks_per_second());
}

auto cap_status(cap::CSpaceError error) noexcept -> myos_status_t {
    switch (error) {
    case cap::CSpaceError::InvalidHandle:
    case cap::CSpaceError::WrongKind:
        return MYOS_STATUS_INVALID_CAP;
    case cap::CSpaceError::InvalidDescriptor:
        return MYOS_STATUS_BAD_ARGS;
    case cap::CSpaceError::Denied:
        return MYOS_STATUS_BAD_RIGHTS;
    case cap::CSpaceError::Amplification:
        return MYOS_STATUS_DENIED;
    case cap::CSpaceError::OutOfMemory:
    case cap::CSpaceError::SlotQuota:
    case cap::CSpaceError::PageQuota:
    case cap::CSpaceError::GenerationExhausted:
    case cap::CSpaceError::ResourceExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case cap::CSpaceError::InvalidState:
    case cap::CSpaceError::Contended:
    case cap::CSpaceError::GrantUnavailable:
        return MYOS_STATUS_BUSY;
    }
    return MYOS_STATUS_INTERNAL;
}

auto read_desc_bytes(
    Call& inv,
    cap::Handle handle,
    usize offset,
    libk::Span<byte> dest) noexcept
    -> std::expected<void, myos_status_t> {
    auto resolved = inv.cspace.resolve<mm::Mem>(
        handle, cap::Rights::of(cap::Right::Inspect));
    if (!resolved) {
        return std::unexpected(cap_status(resolved.error()));
    }
    auto& source = resolved.value();
    const cap::View effective = source.view();
    const auto* const authority = std::get_if<cap::MemLimit>(
        &effective.data);
    const auto end = libk::checked_add(offset, dest.size());
    if (authority == nullptr || !end
        || !authority->perms.contains(mm::Perm::Read)) {
        return std::unexpected(MYOS_STATUS_DENIED);
    }
    const auto rounded = libk::checked_add(
        *end, mm::page_size - 1);
    if (!rounded) {
        return std::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    const usize first = offset / mm::page_size;
    const usize last = *rounded / mm::page_size;
    if (!authority->range.contains(mm::ObjectRange{
            first, last - first})) {
        return std::unexpected(MYOS_STATUS_DENIED);
    }

    auto read = source->read(offset, dest);
    if (read) {
        return {};
    }
    switch (read.error()) {
    case mm::MemErr::OutOfMemory:
    case mm::MemErr::ResourceExhausted:
        return std::unexpected(MYOS_STATUS_NO_MEMORY);
    case mm::MemErr::Pending:
        return std::unexpected(MYOS_STATUS_WOULD_BLOCK);
    case mm::MemErr::BackingFailed:
        return std::unexpected(MYOS_STATUS_BACKING_FAILED);
    case mm::MemErr::Busy:
        return std::unexpected(MYOS_STATUS_BUSY);
    default:
        return std::unexpected(MYOS_STATUS_BAD_ARGS);
    }
}

auto vm_status(mm::VSpaceError error) noexcept -> myos_status_t {
    switch (error) {
    case mm::VSpaceError::InvalidAuthority:
    case mm::VSpaceError::InvalidAccess:
        return MYOS_STATUS_BAD_RIGHTS;
    case mm::VSpaceError::InvalidRegion:
    case mm::VSpaceError::InvalidMapping:
    case mm::VSpaceError::NotMapped:
        return MYOS_STATUS_NOT_FOUND;
    case mm::VSpaceError::InvalidRange:
    case mm::VSpaceError::Overlap:
    case mm::VSpaceError::NotRam:
        return MYOS_STATUS_BAD_ARGS;
    case mm::VSpaceError::OutOfMemory:
    case mm::VSpaceError::QuotaExceeded:
    case mm::VSpaceError::GenerationExhausted:
    case mm::VSpaceError::ResourceExhausted:
        return MYOS_STATUS_NO_MEMORY;
        return MYOS_STATUS_RETRY;
    case mm::VSpaceError::InvalidState:
    case mm::VSpaceError::Busy:
    case mm::VSpaceError::GrantUnavailable:
        return MYOS_STATUS_BUSY;
        return MYOS_STATUS_RETRY;
    case mm::VSpaceError::BackingFailed:
        return MYOS_STATUS_BACKING_FAILED;
    case mm::VSpaceError::TranslationCorrupt:
        libk_assert(false);
        __builtin_unreachable();
    }
    return MYOS_STATUS_INTERNAL;
}



auto handle_of(usize raw) noexcept -> cap::Handle {
    return cap::Handle::from_raw(static_cast<u64>(raw));
}

auto rights_of(usize raw) noexcept -> std::optional<cap::Rights> {
    return cap::Rights::parse(static_cast<u64>(raw), MYOS_RIGHT_MASK);
}

auto perms_of(usize raw) noexcept -> std::optional<mm::Perms> {
    if (raw > std::numeric_limits<u8>::max()) {
        return std::nullopt;
    }
    const auto access = mm::Perms::from_raw(static_cast<u8>(raw));
    return mm::valid_perms(access)
        ? std::optional<mm::Perms>{access}
        : std::nullopt;
}

auto range_of(usize base, usize size) noexcept
    -> std::optional<mm::VRange> {
    const mm::VRange range{mm::Virt{base}, size};
    return range.valid() && !range.empty()
        && (base & (mm::page_size - 1)) == 0
        && (size & (mm::page_size - 1)) == 0
        ? std::optional<mm::VRange>{range}
        : std::nullopt;
}

auto vm_context(CpuLocal& cpu) noexcept -> mm::VmCtx {
    libk_assert(cpu.descriptor != nullptr);
    return mm::VmCtx{
        .cpus = cpu.runtime().owner_registry,
        .local = cpu.descriptor->logical_id(),
    };
}

[[nodiscard]] auto dest(
    cap::CSpace& current,
    cap::Handle handle) noexcept
    -> std::expected<cap::Resolved<cap::CSpace>, cap::CSpaceError> {
    return current.resolve<cap::CSpace>(
        handle, cap::Rights::of(cap::Right::Manage));
}

template<typename Operation>
[[nodiscard]] auto with_dest(
    cap::CSpace& current,
    cap::Handle target,
    Operation&& op) noexcept -> Result {
    if (!target) {
        auto outcome = op(current);
        return returned(
            outcome ? MYOS_STATUS_OK : cap_status(outcome.error()),
            outcome ? outcome.value().raw() : 0);
    }
    auto resolved = dest(current, target);
    if (!resolved) {
        return returned(cap_status(resolved.error()));
    }
    auto outcome = op(resolved.value().object());
    return returned(
        outcome ? MYOS_STATUS_OK : cap_status(outcome.error()),
        outcome ? outcome.value().raw() : 0);
}



template<usize op>
auto cap_call(Call& inv) noexcept -> Result {
    cap::CSpace& cspace = inv.cspace;
    arch::TrapCtx& trap = inv.trap;
    Thread* const thread = inv.target;

    if constexpr (op == MYOS_SYS_CAP_CLOSE) {
        const cap::Handle source = handle_of(trap.arg(0));
        const usize destination_raw = trap.arg(1);
        if (!source) {
            return returned(MYOS_STATUS_INVALID_CAP);
        }
        if (destination_raw == 0) {
            auto closed = cspace.close(source);
            return returned(
                closed ? MYOS_STATUS_OK : cap_status(closed.error()));
        }

        const cap::Handle target = handle_of(destination_raw);
        if (!target) {
            return returned(MYOS_STATUS_INVALID_CAP);
        }
        auto resolved = dest(cspace, target);
        if (!resolved) {
            return returned(cap_status(resolved.error()));
        }
        auto closed = resolved.value().object().close(source);
        return returned(
            closed ? MYOS_STATUS_OK : cap_status(closed.error()));
    }
    if constexpr (op == MYOS_SYS_CAP_TYPED_DELEGATE) {
        const cap::Handle source = handle_of(trap.arg(0));
        const cap::Handle target = handle_of(trap.arg(1));
        const cap::Handle descriptor = handle_of(trap.arg(2));
        if (!source || !descriptor) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        byte bytes[MYOS_CAP_ATTENUATION_SIZE]{};
        auto read = read_desc_bytes(
            inv,
            descriptor,
            trap.arg(3),
            libk::Span<byte>{bytes, sizeof(bytes)});
        if (!read) {
            return returned(read.error());
        }
        auto decoded = cap::decode_attenuation(
            libk::Span<const byte>{bytes, sizeof(bytes)});
        if (!decoded) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        if (!target) {
            auto delegated = cspace.typed_delegate(
                source, cspace, decoded.value());
            return returned(
                delegated ? MYOS_STATUS_OK : cap_status(delegated.error()),
                delegated ? delegated.value().raw() : 0);
        }
        auto resolved = dest(cspace, target);
        if (!resolved) {
            return returned(cap_status(resolved.error()));
        }
        auto delegated = cspace.typed_delegate(
            source, resolved.value().object(), decoded.value());
        return returned(
            delegated ? MYOS_STATUS_OK : cap_status(delegated.error()),
            delegated ? delegated.value().raw() : 0);
    }
    if constexpr (op == MYOS_SYS_CAP_DUPLICATE
        || op == MYOS_SYS_CAP_DELEGATE) {
        const cap::Handle source = handle_of(trap.arg(0));
        const cap::Handle target = handle_of(trap.arg(1));
        const auto rights = rights_of(trap.arg(2));
        if (!source || !rights) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        return with_dest(cspace, target, [&](cap::CSpace& out) {
            return op == MYOS_SYS_CAP_DUPLICATE
                ? cspace.duplicate(source, out, *rights)
                : cspace.delegate(source, out, *rights);
        });
    }
    if constexpr (op == MYOS_SYS_CAP_MOVE) {
        const cap::Handle source = handle_of(trap.arg(0));
        const cap::Handle target = handle_of(trap.arg(1));
        if (!source) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        return with_dest(cspace, target, [&](cap::CSpace& out) {
            return cspace.move(source, out);
        });
    }
    if constexpr (op == MYOS_SYS_CAP_REVOKE) {
        if (thread == nullptr) {
            return returned(MYOS_STATUS_INVALID_OP);
        }
        if (thread->waiting()) {
            return returned(MYOS_STATUS_BUSY);
        }
        const cap::Handle source = handle_of(trap.arg(0));
        if (!source || trap.arg(1) > 1) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        libk_assert(inv.cpu.runtime().owner_registry != nullptr);
        Receipt receipt;
        cap::GrantRevoke done{sync::Latch::Notifier::bind<&Receipt::signal>(receipt)};
        libk_assert(thread->begin_wait(receipt.completion(),
            *inv.cpu.runtime().owner_registry));
        auto started = cspace.revoke(source, done, trap.arg(1) != 0);
        if (!started) {
            thread->cancel_wait();
            return returned(cap_status(started.error()));
        }
        receipt.commit();
        if (!done.arm()) receipt.signal();
        thread->block();
        return returned(MYOS_STATUS_OK);
    }
    if constexpr (op == MYOS_SYS_OBJECT_DESTROY) {
        auto destroyed = cspace.destroy(handle_of(trap.arg(0)));
        return returned(
            destroyed ? MYOS_STATUS_OK : cap_status(destroyed.error()));
    }
    return returned(MYOS_STATUS_INVALID_OP);
}

auto mem_status(mm::MemErr error) noexcept
    -> myos_status_t {
    switch (error) {
    case mm::MemErr::OutOfMemory:
    case mm::MemErr::ResourceExhausted:
    case mm::MemErr::GenerationExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case mm::MemErr::Dirty: return MYOS_STATUS_DIRTY;
    case mm::MemErr::Busy:
    case mm::MemErr::InvalidState:
    case mm::MemErr::AttachmentState:
        return MYOS_STATUS_BUSY;
    case mm::MemErr::Pending:
        return MYOS_STATUS_WOULD_BLOCK;
    case mm::MemErr::BackingFailed:
        return MYOS_STATUS_BACKING_FAILED;
    case mm::MemErr::InvalidSize:
    case mm::MemErr::InvalidRange:
    case mm::MemErr::InvalidAccess:
    case mm::MemErr::NotRam:
    case mm::MemErr::NotBacked:
    case mm::MemErr::OwnershipMismatch:
        return MYOS_STATUS_BAD_ARGS;
    }
    return MYOS_STATUS_INTERNAL;
}

template<usize op>
[[nodiscard]] auto memory_seal(Call& inv) noexcept -> Result {
    auto memory = inv.cspace.resolve<mm::Mem>(
        handle_of(inv.trap.arg(0)),
        cap::Rights::of(cap::Right::Manage));
    if (!memory) {
        return returned(cap_status(memory.error()));
    }
    auto sealed = memory.value()->seal();
    return returned(sealed ? MYOS_STATUS_OK : mem_status(sealed.error()));
}

template<usize op>
[[nodiscard]] auto memory_write(Call& inv) noexcept -> Result {
    auto memory = inv.cspace.resolve<mm::Mem>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Manage));
    if (!memory) return returned(cap_status(memory.error()));
    const usize offset = inv.trap.arg(1);
    const usize source = inv.trap.arg(2);
    const usize size = inv.trap.arg(3);
    if (size == 0 || size > mm::page_size - (offset & (mm::page_size - 1)))
        return returned(MYOS_STATUS_BAD_ARGS);
    const auto effective = memory.value().view();
    const auto* limit = std::get_if<cap::MemLimit>(&effective.data);
    if (limit == nullptr
        || !limit->range.contains(mm::ObjectRange{offset / mm::page_size, 1})
        || !limit->perms.contains(mm::Perm::Write))
        return returned(MYOS_STATUS_BAD_RIGHTS);
    auto* buffer = inv.target->ipc_buffer();
    if (buffer == nullptr) return returned(MYOS_STATUS_BAD_ARGS);
    auto access = buffer->access();
    if (!access) return returned(MYOS_STATUS_RETRY);
    // The source lease pins bytes without acquiring locks during the copy.
    // A dest aliased by this IPC mapping is rejected by write(), which
    // requires private storage with no attachments or other page leases.
    const auto bytes = access.value().bytes(source, size);
    if (bytes.empty()) return returned(MYOS_STATUS_BAD_ARGS);
    auto written = memory.value()->write(offset, bytes);
    return returned(written ? MYOS_STATUS_OK : mem_status(written.error()));
}

template<usize op>
[[nodiscard]] auto mem_call(Call& inv) noexcept -> Result {
    auto* thread = inv.target;
    if (thread == nullptr) return returned(MYOS_STATUS_INVALID_OP);
    if (thread->waiting()) return returned(MYOS_STATUS_BUSY);
    auto memory = inv.cspace.resolve<mm::Mem>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Manage));
    if (!memory) return returned(cap_status(memory.error()));
    const usize page = inv.trap.arg(1);
    const mm::ObjectRange range{page, op == MYOS_SYS_MEM_TRIM ? inv.trap.arg(2) : 1};
    if (range.empty() || !range.limit() || *range.limit() > memory.value()->page_count())
        return returned(MYOS_STATUS_BAD_ARGS);
    const auto view = memory->view();
    const auto* limit = std::get_if<cap::MemLimit>(&view.data);
    if (!limit || !limit->range.contains(range))
        return returned(MYOS_STATUS_BAD_RIGHTS);
    if (page >= memory.value()->size() / mm::page_size) return returned(MYOS_STATUS_BAD_ARGS);
    auto ref = memory->reference();
    if (!ref) return returned(MYOS_STATUS_BUSY);
    auto& mem = memory->object();
    memory->reset();
    auto* cpus = inv.cpu.runtime().owner_registry;
    libk_assert(cpus);
    const auto result = [&]() {
        if constexpr (op == MYOS_SYS_MEM_TRIM) return mem.trim(*thread, *cpus, range);
        else if constexpr (op == MYOS_SYS_MEM_WRITEBACK) return mem.writeback(page);
        else return mem.populate(*thread, *cpus, page);
    }();
    return returned(thread->stop_requested() ? MYOS_STATUS_CANCELED
                    : result ? MYOS_STATUS_OK : mem_status(result.error()));
}

template<usize op>
[[nodiscard]] auto resource_close(Call& inv) noexcept -> Result {
    Thread* const thread = inv.target;
    if (thread == nullptr) {
        return returned(MYOS_STATUS_INVALID_OP);
    }
    if (thread->waiting()) {
        return returned(MYOS_STATUS_BUSY);
    }
    Receipt receipt;
    {
        auto pool = inv.cspace.resolve<object::group>(
            handle_of(inv.trap.arg(0)),
            cap::Rights::of(cap::Right::Close));
        if (!pool) {
            return returned(cap_status(pool.error()));
        }
        auto self = pool.value().reference();
        if (!self) {
            return returned(MYOS_STATUS_BUSY);
        }
        CpuRegistry* const cpus = inv.cpu.runtime().owner_registry;
        libk_assert(cpus != nullptr);
        auto& object = pool.value().object();
        if (!thread->begin_wait(receipt.completion(), *cpus)) {
            return returned(MYOS_STATUS_BUSY);
        }
        if (!object.observe_refund(self.value(),
            resource::RefundNotifier::bind<&Receipt::signal>(receipt))) {
            thread->cancel_wait();
            return returned(MYOS_STATUS_BUSY);
        }
        receipt.commit();
        static_cast<void>(object.close());
    }
    thread->block();
    return returned(MYOS_STATUS_OK);
}

template<usize op>
[[nodiscard]] auto resource_close_async(Call& inv) noexcept -> Result {
    auto pool = inv.cspace.resolve<object::group>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Close));
    // Receive authority owns the dest inbox and may choose its event
    // bits. A borrowed Signal capability cannot inject arbitrary badges.
    auto notification = inv.cspace.resolve<ipc::Notification>(
        handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Receive));
    if (!pool || !notification) return returned(cap_status(!pool ? pool.error() : notification.error()));
    const auto badge = inv.trap.arg(2);
    if (badge == 0) return returned(MYOS_STATUS_BAD_ARGS);
    auto self = pool.value().reference();
    auto dest = notification.value().reference();
    if (!self || !dest) return returned(MYOS_STATUS_BUSY);
    const auto id = dest.value().id();
    auto& notifications = inv.cpu.runtime().kernel->pool<ipc::Notification>();
    const resource::RefundNotifier notifier{&notifications,
        [](void* ctx, usize slot, u64 generation, u64 bits) noexcept {
            // Weak generation identity: a dead inbox drops the delivery. No
            // reference can keep an object in the closing subtree alive.
            auto target = static_cast<object::pool<ipc::Notification>*>(ctx)->lookup(
                {slot, generation, object::ObjectKind::Notification});
            if (target) static_cast<void>(target.value()->signal(bits));
        }, id.slot, id.generation, badge};
    if (!pool.value()->observe_refund(self.value(), notifier)) return returned(MYOS_STATUS_BUSY);
    static_cast<void>(pool.value()->close());
    return returned(MYOS_STATUS_OK);
}

[[nodiscard]] static auto pager_error(Pager::Error error) noexcept -> myos_status_t {
    switch (error) {
    case Pager::Error::Closed:
        return MYOS_STATUS_CLOSED;
    case Pager::Error::InvalidRange:
        return MYOS_STATUS_BAD_ARGS;
    case Pager::Error::Busy:
        return MYOS_STATUS_WOULD_BLOCK;
    case Pager::Error::Stale:
    case Pager::Error::GenerationExhausted:
        return MYOS_STATUS_RETRY;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] static auto irq_error(irq::Error error) noexcept -> myos_status_t {
    switch (error) {
    case irq::Error::BadSequence:
        return MYOS_STATUS_BAD_ARGS;
    case irq::Error::InvalidState:
    case irq::Error::Busy:
        return MYOS_STATUS_BUSY;
    case irq::Error::StaleSequence:
        return MYOS_STATUS_REASSERTED;
    case irq::Error::Closed:
        return MYOS_STATUS_CLOSED;
    }
    return MYOS_STATUS_INTERNAL;
}



[[nodiscard]] static auto pager_descriptor(
    const Pager::Req& request) noexcept -> myos_pager_request {
    myos_pager_request descriptor{};
    descriptor.kind = request.kind == Pager::Kind::PageIn
        ? MYOS_PAGER_REQUEST_PAGE_IN : MYOS_PAGER_REQUEST_WRITEBACK;
    descriptor.urgency = request.urgency;
    descriptor.id = request.id;
    descriptor.page_index = request.page_index;
    if (request.kind == Pager::Kind::PageIn) {
        descriptor.payload.page_in.first = request.first;
        descriptor.payload.page_in.count = request.count;
    } else {
        descriptor.payload.writeback.dirty_epoch = request.dirty_epoch;
    }
    return descriptor;
}

static auto pg_claim(Call& inv) noexcept -> Result {
    auto resolved = inv.cspace.resolve<Pager>(
        handle_of(inv.trap.arg(0)),
        cap::Rights::of(cap::Right::Serve));
    if (!resolved) {
        return returned(cap_status(resolved.error()));
    }
    auto* const buffer = inv.target->ipc_buffer();
    if (buffer == nullptr) {
        return returned(MYOS_STATUS_WOULD_BLOCK);
    }
    auto access = buffer->access();
    if (!access) {
        return returned(MYOS_STATUS_RETRY);
    }
    const auto request = resolved.value()->claim(&inv.target->claims());
    if (!request) {
        return returned(pager_error(request.error()));
    }
    const auto descriptor = pager_descriptor(request.value());
    if (!access.value().write(
            0,
            libk::Span<const byte>{
                reinterpret_cast<const byte*>(&descriptor),
                sizeof(descriptor)})) {
        static_cast<void>(resolved.value()->requeue(
            request.value().id));
        return returned(MYOS_STATUS_RETRY);
    }
    return returned(MYOS_STATUS_OK, request.value().id);
}

template<usize op>
static auto pg_finish(Call& inv) noexcept -> Result {
    const auto right = op == MYOS_SYS_PAGER_REQUEUE ? cap::Right::Serve : cap::Right::Supply;
    auto pager = inv.cspace.resolve<Pager>(handle_of(inv.trap.arg(0)), cap::Rights::of(right));
    if (!pager) return returned(cap_status(pager.error()));
    if constexpr (op == MYOS_SYS_PAGER_REQUEUE) {
        const auto result = pager.value()->requeue(inv.trap.arg(1));
        return result ? returned(MYOS_STATUS_OK) : returned(pager_error(result.error()));
    }
    auto memory = inv.cspace.resolve<mm::Mem>(handle_of(inv.trap.arg(1)),
        cap::Rights::of(cap::Right::Manage));
    if (!memory) return returned(cap_status(memory.error()));
    const auto result = memory.value()->pager_finish(pager.value().object(), inv.trap.arg(2),
        op == MYOS_SYS_PAGER_FAIL);
    return result ? returned(MYOS_STATUS_OK) : returned(mem_status(result.error()));
}

static auto pg_supply(Call& inv) noexcept -> Result {
    auto pager = inv.cspace.resolve<Pager>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Supply));
    auto target = inv.cspace.resolve<mm::Mem>(
        handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto source = inv.cspace.resolve<mm::Mem>(
        handle_of(inv.trap.arg(2)), cap::Rights::of(cap::Right::Manage));
    if (!pager || !target || !source) {
        return returned(cap_status(!pager ? pager.error()
            : !target ? target.error() : source.error()));
    }
    auto transfer = source.value()->begin_transfer(inv.trap.arg(3));
    if (!transfer) return returned(mem_status(transfer.error()));
    auto supplied = target.value()->supply(pager.value().object(), std::move(*transfer),
        inv.trap.arg(4));
    return supplied ? returned(MYOS_STATUS_OK)
                    : returned(mem_status(supplied.error()));
}

static auto pg_bind(Call& inv) noexcept -> Result {
    auto pager = inv.cspace.resolve<Pager>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Serve));
    auto notification = inv.cspace.resolve<ipc::Notification>(
        handle_of(inv.trap.arg(1)),
        cap::Rights::of(cap::Right::Signal));
    if (!pager || !notification) {
        return returned(cap_status(!pager ? pager.error() : notification.error()));
    }
    const auto limit = notification.value().view();
    const auto* const data = std::get_if<cap::Badge>(
        &limit.data);
    const u64 badge = inv.trap.arg(2);
    if (data == nullptr || badge == 0 || badge != data->badge) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto result = pager.value()->bind(notification.value().object(), badge);
    return result ? returned(MYOS_STATUS_OK)
                  : returned(pager_error(result.error()));
}

static auto irq_bind(Call& inv) noexcept -> Result {
    auto irq = inv.cspace.resolve<irq::Irq>(
        handle_of(inv.trap.arg(0)),
        cap::Rights::of(cap::Right::Route));
    auto notification = inv.cspace.resolve<ipc::Notification>(
        handle_of(inv.trap.arg(1)),
        cap::Rights::of(cap::Right::Signal));
    if (!irq || !notification) {
        return returned(cap_status(!irq ? irq.error() : notification.error()));
    }
    const auto limit = notification.value().view();
    const auto* const notification_data =
        std::get_if<cap::Badge>(&limit.data);
    const u64 badge = inv.trap.arg(2);
    if (notification_data == nullptr || badge == 0
        || badge != notification_data->badge) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto result = irq.value()->bind(
        notification.value().object(), badge);
    return result ? returned(MYOS_STATUS_OK)
                  : returned(irq_error(result.error()));
}

template<usize op>
static auto irq_call(Call& inv) noexcept -> Result {
    if constexpr (op == MYOS_SYS_IRQ_BIND) {
        return irq_bind(inv);
    }
    if constexpr (op != MYOS_SYS_IRQ_OBSERVE
        && op != MYOS_SYS_IRQ_ACK) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const cap::Right right = op == MYOS_SYS_IRQ_OBSERVE
        ? cap::Right::Observe
        : cap::Right::Ack;
    auto irq = inv.cspace.resolve<irq::Irq>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(right));
    if (!irq) {
        return returned(cap_status(irq.error()));
    }
    if constexpr (op == MYOS_SYS_IRQ_OBSERVE) {
        const auto result = irq.value()->delivery();
        return result
            ? returned(MYOS_STATUS_OK, result.value().sequence,
                result.value().generation)
            : returned(irq_error(result.error()));
    }
    const auto result = irq.value()->ack(
        inv.trap.arg(1), inv.trap.arg(2));
    return result ? returned(MYOS_STATUS_OK)
                  : returned(irq_error(result.error()));
}

template<usize op>
auto pg_call(Call& inv) noexcept -> Result {
    if constexpr (op == MYOS_SYS_PAGER_CLAIM) {
        return pg_claim(inv);
    }
    if constexpr (op == MYOS_SYS_PAGER_REQUEUE
        || op == MYOS_SYS_PAGER_COMPLETE
        || op == MYOS_SYS_PAGER_FAIL) {
        return pg_finish<op>(inv);
    }
    if constexpr (op == MYOS_SYS_PAGER_SUPPLY) {
        return pg_supply(inv);
    }
    if constexpr (op == MYOS_SYS_PAGER_BIND) {
        return pg_bind(inv);
    }
    return irq_call<op>(inv);
}

template<usize op>
auto exit_call(Call& call) noexcept -> Result {
    auto thread = call.cspace.resolve<Thread>(handle_of(call.trap.arg(0)),
        cap::Rights::of(cap::Right::Observe));
    if (!thread) return returned(cap_status(thread.error()));
    auto& record = thread.value()->exit();
    if constexpr (op == MYOS_SYS_EXIT_QUERY) {
        const auto result = record.read();
        return returned(MYOS_STATUS_OK, result.sequence,
            static_cast<usize>(static_cast<isize>(result.status)));
    }
    auto notification = call.cspace.resolve<ipc::Notification>(
        handle_of(call.trap.arg(1)), cap::Rights::of(cap::Right::Signal));
    if (!notification) return returned(cap_status(notification.error()));
    const auto badge = call.trap.arg(2);
    if (!badge) return returned(MYOS_STATUS_BAD_ARGS);
    return returned(record.observe(notification.value().object(), badge)
        ? MYOS_STATUS_OK : MYOS_STATUS_BUSY);
}

static_assert(static_cast<u8>(io::SpaceState::Empty) == MYOS_IO_SPACE_EMPTY);
static_assert(static_cast<u8>(io::SpaceState::Binding) == MYOS_IO_SPACE_BINDING);
static_assert(static_cast<u8>(io::SpaceState::Opening) == MYOS_IO_SPACE_OPENING);
static_assert(static_cast<u8>(io::SpaceState::Active) == MYOS_IO_SPACE_ACTIVE);
static_assert(static_cast<u8>(io::SpaceState::Closing) == MYOS_IO_SPACE_CLOSING);
static_assert(static_cast<u8>(io::SpaceState::Closed) == MYOS_IO_SPACE_CLOSED);
static_assert(static_cast<u8>(io::SpaceState::Failed) == MYOS_IO_SPACE_FAILED);
static_assert(static_cast<u8>(io::SpaceState::Faulted) == MYOS_IO_SPACE_FAULTED);

static auto io_status(io::SpaceError error) noexcept -> myos_status_t {
    switch (error) {
    case io::SpaceError::InvalidState:
    case io::SpaceError::Busy: return MYOS_STATUS_BUSY;
    case io::SpaceError::Denied: return MYOS_STATUS_BAD_RIGHTS;
    case io::SpaceError::InvalidRange:
    case io::SpaceError::UnsupportedMemory: return MYOS_STATUS_BAD_ARGS;
    case io::SpaceError::BackingUnavailable: return MYOS_STATUS_BACKING_FAILED;
    case io::SpaceError::OutOfMemory:
    case io::SpaceError::QuotaExceeded: return MYOS_STATUS_NO_MEMORY;
    case io::SpaceError::Cancelled: return MYOS_STATUS_CANCELED;
    }
    return MYOS_STATUS_INTERNAL;
}

[[gnu::noinline]] static auto bind(Call& inv, cap::Resolved<io::Space>& space) noexcept -> Result {
    const auto& trap = inv.trap;
    auto device = inv.cspace.resolve<io::Device>(handle_of(trap.arg(1)),
        cap::Rights::of(cap::Right::Connect));
    auto memory = inv.cspace.resolve<mm::Mem>(handle_of(trap.arg(2)),
        cap::Rights::of(cap::Right::Map));
    if (!device || !memory) return returned(cap_status(!device ? device.error() : memory.error()));
    auto self = space.reference();
    if (!self) return returned(MYOS_STATUS_CLOSED);
    auto bound = space->bind(std::move(self).value(), device.value(), memory.value(),
        {trap.arg(3), trap.arg(4)}, trap.arg(5));
    return returned(bound ? MYOS_STATUS_OK : io_status(bound.error()));
}

static auto watch(Call& inv, io::Space& space) noexcept -> Result {
    auto notification = inv.cspace.resolve<ipc::Notification>(
        handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Signal));
    if (!notification) return returned(cap_status(notification.error()));
    const auto limit = notification.value().view();
    const auto* data = std::get_if<cap::Badge>(&limit.data);
    const u64 badge = inv.trap.arg(2);
    if (data == nullptr || badge == 0 || data->badge != badge)
        return returned(MYOS_STATUS_BAD_ARGS);
    const auto result = space.watch(notification.value().object(), badge);
    return returned(result ? MYOS_STATUS_OK : io_status(result.error()));
}

static auto write_info(Call& inv, const io::DeviceInfo& info) noexcept -> Result {
    auto* buffer = inv.target->ipc_buffer();
    return returned(buffer && buffer->write(inv.trap.arg(1),
        {reinterpret_cast<const byte*>(&info), sizeof(info)})
        ? MYOS_STATUS_OK : MYOS_STATUS_BAD_ARGS);
}

static auto info(Call& inv, io::Space& space) noexcept -> Result {
    auto snapshot = space.info();
    if (!snapshot) return returned(io_status(snapshot.error()));
    snapshot.value().version = MYOS_IO_INFO_VERSION;
    snapshot.value().requester = 0; // IO_INFO's second word is reserved.
    return write_info(inv, snapshot.value());
}

[[gnu::noinline]] static auto device_info(Call& inv) noexcept -> Result {
    auto device = inv.cspace.resolve<io::Device>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Inspect));
    if (!device) return returned(cap_status(device.error()));
    return write_info(inv, device.value()->info());
}

static auto install(Call& inv,
    std::expected<cap::GrantRef, io::SpaceError>&& exported) noexcept -> Result {
    if (!exported) return returned(io_status(exported.error()));
    auto lease = exported.value().acquire();
    if (!lease) return returned(MYOS_STATUS_CLOSED);
    const auto ceiling = lease.value().ceiling();
    auto installed = inv.cspace.insert(std::move(exported).value(),
        {ceiling.rights, ceiling.data});
    return installed ? returned(MYOS_STATUS_OK, installed.value().raw())
                     : returned(cap_status(installed.error()));
}

template<usize op>
auto io_call(Call& inv) noexcept -> Result {
    if constexpr (op == MYOS_SYS_DEVICE_INFO) return device_info(inv);
    const auto right = op == MYOS_SYS_IO_SPACE_STATE || op == MYOS_SYS_IO_SPACE_INFO
        ? cap::Right::Inspect : op == MYOS_SYS_IO_SPACE_CLOSE ? cap::Right::Close : cap::Right::Connect;
    auto space = inv.cspace.resolve<io::Space>(handle_of(inv.trap.arg(0)),
        cap::Rights::of(right));
    if (!space) return returned(cap_status(space.error()));
    switch (op) {
    case MYOS_SYS_IO_SPACE_BIND: return bind(inv, space.value());
    case MYOS_SYS_IO_SPACE_WATCH: return watch(inv, space.value().object());
    case MYOS_SYS_IO_SPACE_STATE: return returned(MYOS_STATUS_OK, static_cast<u8>(space.value()->state()));
    case MYOS_SYS_IO_SPACE_INFO: return info(inv, space.value().object());
    case MYOS_SYS_IO_SPACE_BAR: return install(inv, space.value()->bar(inv.trap.arg(1)));
    case MYOS_SYS_IO_SPACE_IRQ: return install(inv, space.value()->interrupt());
    case MYOS_SYS_IO_SPACE_CLOSE:
        space.value()->close();
        return returned(MYOS_STATUS_OK);
    default: return returned(MYOS_STATUS_INVALID_OP);
    }
}

template<usize op>
auto vm_call(Call& inv) noexcept -> Result {
    arch::TrapCtx& trap = inv.trap;
    cap::CSpace& cspace = inv.cspace;
    const cap::Handle vspace_handle = handle_of(trap.arg(0));
    const cap::Right required = op == MYOS_SYS_VM_MAP
        ? cap::Right::Map
        : op == MYOS_SYS_VM_UNMAP
            ? cap::Right::Unmap
            : op == MYOS_SYS_VM_PROTECT
                ? cap::Right::Protect
                : op == MYOS_SYS_VM_SLICE
                    ? cap::Right::Delegate
                    : op == MYOS_SYS_VM_CLEAR
                        ? cap::Right::Destroy
                        : cap::Right::Reserve;
    if (!vspace_handle) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    auto target = cspace.resolve<mm::VSpace>(
        vspace_handle, cap::Rights::of(required));
    if (!target) {
        return returned(cap_status(target.error()));
    }
    const cap::View effective = target.value().view();
    const auto* const where =
        std::get_if<cap::VmLimit>(&effective.data);
    libk_assert(where != nullptr);
    mm::VSpace& space = target.value().object();
    const mm::VmCtx vm = vm_context(inv.cpu);
    const auto finish = [&](mm::VmStatus status) noexcept -> myos_status_t {
        if (status == mm::VmStatus::Complete) return MYOS_STATUS_OK;
        auto reference = target.value().reference();
        libk_assert(reference);
        mm::Fence fence{std::move(reference).value(), space};
        target.value().reset(); // Admission lease cannot block cap revocation.
        auto* cpus = inv.cpu.runtime().owner_registry;
        libk_assert(cpus && inv.target->begin_wait(fence.completion(), *cpus));
        fence.start();
        inv.target->block();
        return MYOS_STATUS_OK;
    };


    if constexpr (op == MYOS_SYS_VM_CLEAR) {
        auto destroyed = space.clear(vm, where->range);
        return returned(destroyed
            ? finish(destroyed.value())
            : vm_status(destroyed.error()));
    }
    if constexpr (op == MYOS_SYS_VM_MAP) {
        const cap::Handle memory_handle = handle_of(trap.arg(1));
        const auto range = range_of(trap.arg(2), trap.arg(3));
        const usize flags = trap.arg(5);
        const auto access = perms_of(flags & (MYOS_VM_READ | MYOS_VM_WRITE | MYOS_VM_EXECUTE));
        if (!memory_handle || !range || !access
            || (flags & ~(MYOS_VM_READ | MYOS_VM_WRITE | MYOS_VM_EXECUTE
                | MYOS_VM_MAP_PRIVATE)) != 0) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        auto memory = cspace.resolve<mm::Mem>(
            memory_handle, cap::Rights::of(cap::Right::Map));
        if (!memory) {
            return returned(cap_status(memory.error()));
        }
        const usize pages = range->size() / mm::page_size;
        auto mapped = space.map(
            vm,
            *where,
            mm::MapReq{
                *range,
                mm::ObjectRange{trap.arg(4), pages},
                *access, (flags & MYOS_VM_MAP_PRIVATE) != 0},
            memory.value());
        memory.value().reset();
        return returned(mapped
            ? finish(mapped.value().status)
            : vm_status(mapped.error()));
    }

    const auto range = range_of(trap.arg(1), trap.arg(2));
    if (!range) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    if constexpr (op == MYOS_SYS_VM_UNMAP) {
        auto unmapped = space.unmap(vm, *where, *range);
        return returned(unmapped
            ? finish(unmapped.value())
            : vm_status(unmapped.error()));
    }
    if constexpr (op == MYOS_SYS_VM_PROTECT) {
        const auto access = perms_of(trap.arg(3));
        if (!access) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        auto protected_range = space.protect(vm, *where, *range, *access);
        return returned(protected_range
            ? finish(protected_range.value())
            : vm_status(protected_range.error()));
    }
    if constexpr (op == MYOS_SYS_VM_SLICE) {
        const auto access = perms_of(trap.arg(3));
        const auto rights = rights_of(trap.arg(4));
        if (!access || !rights || trap.arg(5) != 0) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        const cap::View slice{*rights, cap::VmLimit{*range,*access}};
        auto created = cspace.delegate(vspace_handle,cspace,slice,slice);
        return returned(created ? MYOS_STATUS_OK : cap_status(created.error()),
            created ? created->raw() : 0);
    }
    if constexpr (op == MYOS_SYS_VM_RESERVE || op == MYOS_SYS_VM_GUARD) {
        if (!where->range.contains(*range)) return returned(MYOS_STATUS_DENIED);
        auto reserved = op == MYOS_SYS_VM_RESERVE
            ? space.reserve(*range)
            : space.reserve(*range,true);
        return returned(
            reserved ? MYOS_STATUS_OK : vm_status(reserved.error()));
    }
    return returned(MYOS_STATUS_INVALID_OP);
}

[[nodiscard]] static auto bind_target(
    cap::Resolved<sched::Sc>& ctx,
    cap::Resolved<Thread>& target) noexcept -> Result {
    auto reference = target.reference();
    if (!reference) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto hold = std::move(reference).value().as<Thread>();
    if (!hold) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto bound = ctx->bind_authorized(
        std::move(hold).value(), ctx, target);
    return returned(bound ? MYOS_STATUS_OK : MYOS_STATUS_BUSY);
}

template<usize op>
[[nodiscard]] auto sc_bind(Call& inv) noexcept -> Result {
    auto ctx = inv.cspace.resolve<sched::Sc>(
        handle_of(inv.trap.arg(0)),
        cap::Rights::of(cap::Right::Control));
    if (!ctx) {
        return returned(cap_status(ctx.error()));
    }
    const cap::Handle target = handle_of(inv.trap.arg(1));
    auto thread = inv.cspace.resolve<Thread>(
        target,
        cap::Rights::of(cap::Right::Control));
    if (thread) {
        return bind_target(ctx.value(), thread.value());
    }
    return returned(cap_status(thread.error()));

}

[[nodiscard]] static auto start_target(
    Call& inv,
    cap::Resolved<Thread>& target) noexcept -> Result {
    sched::Sc* const binding = target->sc();
    if (target->state() != Thread::State::Prepared
        || binding == nullptr || !binding->startable()) {
        return returned(MYOS_STATUS_BUSY);
    }
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    auto started = sched::start(kernel->cpus(), *binding);
    return returned(started ? MYOS_STATUS_OK : MYOS_STATUS_BUSY);
}

template<usize op>
[[nodiscard]] auto thread_start(Call& inv) noexcept -> Result {
    const cap::Handle target = handle_of(inv.trap.arg(0));
    auto thread = inv.cspace.resolve<Thread>(
        target, cap::Rights::of(cap::Right::Control));
    if (thread) {
        return start_target(inv, thread.value());
    }
    return returned(cap_status(thread.error()));

}


template<usize op>
auto yield(Call&) noexcept -> Result {
    return Result{MYOS_STATUS_OK, 0, Disposition::Yield};
}

template<usize op>
auto exit(Call& inv) noexcept -> Result {
    const auto status = static_cast<myos_status_t>(inv.trap.arg(0));
    return Result{MYOS_STATUS_OK,
        static_cast<usize>(static_cast<isize>(status)), Disposition::Exit};
}

static void publish(arch::TrapCtx& ctx, const Result& result) noexcept {
    ctx.set_result(
        0, static_cast<usize>(static_cast<isize>(result.status)));
    ctx.set_result(1, result.value);
    ctx.set_result(2, result.value2);
}


auto handle(arch::TrapCtx& ctx) noexcept -> Disposition {
    CpuLocal& cpu = current_cpu();
    libk_assert(cpu.dispatcher() != nullptr);
    Thread* target = cpu.dispatcher()->current();
    cap::CSpace* const cspace = cpu.cspace();
    mm::VSpace* const vspace = cpu.vspace();
    libk_assert(target && cspace != nullptr && vspace != nullptr);

    target->note_user_syscall();
    ctx.complete_syscall();
    Call inv{cpu, target, *cspace, *vspace, ctx};
    const usize operation = ctx.arg(7);
    const bool leaf = target->activation() != nullptr;
    enum class Locus { Any, Base, Leaf };
    Result outcome{};
    switch (operation) {
#define CALL(name, nr, entry, locus, unit) \
    case nr: \
        outcome = ((Locus::locus == Locus::Base && leaf) \
            || (Locus::locus == Locus::Leaf && !leaf)) \
            ? returned(MYOS_STATUS_INVALID_OP) : entry<nr>(inv); \
        break;
#include <uapi/calls.def>
#undef CALL
    default:
        outcome = returned(MYOS_STATUS_INVALID_OP);
        break;
    }
    if (outcome.disposition != Disposition::Resume) {
        publish(ctx, outcome);
    }
    return outcome.disposition;
}

} // namespace syscall
