#include <expected>
#include <optional>
#include <mm/mem.hpp>
#include <object/ref.hpp>
#include <syscall/call.hpp>
#include <object/pool.hpp>
#include <cpu/cpu.hpp>
#include <libk/checked_arithmetic.hpp>
#include <limits>
#include <uapi/abi.h>
#include <cap/cap.hpp>
#include <task/thread.hpp>
#include <utility>
#include <ipc/notification.hpp>
#include <ipc/port.hpp>
#include <ipc/channel.hpp>
#include <resource/quota.hpp>
#include <irq/irq.hpp>
#include <mm/pager.hpp>
#include <uapi/mem.h>
#include <io/space.hpp>
#include <mm/vspace.hpp>
#include <variant>
#include <sched/dispatcher.hpp>
#include <sched/sched.hpp>

namespace syscall {

template<usize op>
auto clock_now(Call& inv) noexcept -> Result {
    return returned(STATUS_OK, inv.cpu.dispatcher().clock().rd_ticks());
}

template<usize op>
auto clock_frequency(Call& inv) noexcept -> Result {
    return returned(STATUS_OK, inv.cpu.dispatcher().clock().rd_freq());
}

auto cap_status(cap::CSpaceError error) noexcept -> status_t {
    switch (error) {
    case cap::CSpaceError::InvalidHandle:
    case cap::CSpaceError::WrongKind:
        return STATUS_INVALID_CAP;
    case cap::CSpaceError::InvalidDescriptor:
        return STATUS_BAD_ARGS;
    case cap::CSpaceError::Denied:
        return STATUS_BAD_RIGHTS;
    case cap::CSpaceError::Amplification:
        return STATUS_DENIED;
    case cap::CSpaceError::OutOfMemory:
    case cap::CSpaceError::SlotQuota:
    case cap::CSpaceError::PageQuota:
    case cap::CSpaceError::GenerationExhausted:
    case cap::CSpaceError::ResourceExhausted:
        return STATUS_NO_MEMORY;
    case cap::CSpaceError::InvalidState:
    case cap::CSpaceError::Contended:
        return STATUS_BUSY;
    case cap::CSpaceError::GrantUnavailable:
        return STATUS_CLOSED;
    }
    return STATUS_INTERNAL;
}

auto read_desc_bytes(
    Call& inv,
    cap::Handle handle,
    usize offset,
    std::span<byte> dest) noexcept
    -> std::expected<void, status_t> {
    auto resolved = inv.cspace.resolve<mm::Mem>(
        handle, cap::Rights::of(cap::Right::Inspect));
    if (!resolved) {
        return std::unexpected(cap_status(resolved.error()));
    }
    auto& source = resolved.value();
    const cap::View effective = source.view();
    const auto* const limit = std::get_if<cap::MemLimit>(
        &effective.data);
    const auto end = libk::checked_add(offset, dest.size());
    if (limit == nullptr || !end
        || !limit->perms.contains(mm::Perm::Read)) {
        return std::unexpected(STATUS_DENIED);
    }
    const auto rounded = libk::checked_add(
        *end, mm::page_size - 1);
    if (!rounded) {
        return std::unexpected(STATUS_BAD_ARGS);
    }
    const usize first = offset / mm::page_size;
    const usize last = *rounded / mm::page_size;
    if (!limit->range.contains(mm::ObjectRange{
            first, last - first})) {
        return std::unexpected(STATUS_DENIED);
    }

    auto read = source->read(offset, dest);
    if (read) {
        return {};
    }
    switch (read.error()) {
    case mm::MemErr::OutOfMemory:
    case mm::MemErr::ResourceExhausted:
        return std::unexpected(STATUS_NO_MEMORY);
    case mm::MemErr::Pending:
        return std::unexpected(STATUS_WOULD_BLOCK);
    case mm::MemErr::BackingFailed:
        return std::unexpected(STATUS_BACKING_FAILED);
    case mm::MemErr::Busy:
        return std::unexpected(STATUS_BUSY);
    default:
        return std::unexpected(STATUS_BAD_ARGS);
    }
}




auto handle_of(usize raw) noexcept -> cap::Handle {
    return cap::Handle::from_raw(static_cast<u64>(raw));
}

auto rights_of(usize raw) noexcept -> std::optional<cap::Rights> {
    return cap::Rights::parse(static_cast<u64>(raw), RIGHT_MASK);
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
    const mm::VRange range{mm::va_t{base}, size};
    return range.valid() && !range.empty()
        && (base & (mm::page_size - 1)) == 0
        && (size & (mm::page_size - 1)) == 0
        ? std::optional<mm::VRange>{range}
        : std::nullopt;
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
            outcome ? STATUS_OK : cap_status(outcome.error()),
            outcome ? outcome.value().raw() : 0);
    }
    auto resolved = dest(current, target);
    if (!resolved) {
        return returned(cap_status(resolved.error()));
    }
    auto outcome = op(resolved.value().object());
    return returned(
        outcome ? STATUS_OK : cap_status(outcome.error()),
        outcome ? outcome.value().raw() : 0);
}



// Slot storage belongs to the destination CSpace. A caller may separately
// authorize a longer-lived quota for a derived grant's lineage node.
static auto reserve_grant(Call& inv, cap_t quota) noexcept
    -> std::expected<resource::Reservation, status_t> {
    if (!quota) return resource::Reservation{};
    auto pool = inv.cspace.resolve<resource::Quota>(handle_of(quota), cap::Rights::of(cap::Right::Create));
    if (!pool) return std::unexpected(cap_status(pool.error()));
    const auto view = pool->view();
    const auto* limit = std::get_if<cap::QuotaLimit>(&view.data);
    if (!limit || !limit->budget.contains(cap::Graph::node_usage()))
        return std::unexpected(STATUS_DENIED);
    auto ref = pool->reference();
    if (!ref) return std::unexpected(STATUS_BUSY);
    auto fee = pool->object().reserve(std::move(*ref), cap::Graph::node_usage());
    if (!fee) return std::unexpected(fee.error() == resource::errc::exhausted
                                    ? STATUS_NO_MEMORY : STATUS_BUSY);
    return std::move(*fee);
}

template<usize op>
auto cap_call(Call& inv) noexcept -> Result {
    cap::CSpace& cspace = inv.cspace;
    arch::TrapCtx& trap = inv.trap;
    Thread* const thread = inv.target;

    if constexpr (op == SYS_CAP_CLOSE) {
        const cap::Handle source = handle_of(trap.arg(0));
        const usize destination_raw = trap.arg(1);
        if (!source) {
            return returned(STATUS_INVALID_CAP);
        }
        if (destination_raw == 0) {
            auto closed = cspace.close(source);
            return returned(
                closed ? STATUS_OK : cap_status(closed.error()));
        }

        const cap::Handle target = handle_of(destination_raw);
        if (!target) {
            return returned(STATUS_INVALID_CAP);
        }
        auto resolved = dest(cspace, target);
        if (!resolved) {
            return returned(cap_status(resolved.error()));
        }
        auto closed = resolved.value().object().close(source);
        return returned(
            closed ? STATUS_OK : cap_status(closed.error()));
    }
    if constexpr (op == SYS_CAP_TYPED_DELEGATE) {
        const cap::Handle source = handle_of(trap.arg(0));
        const cap::Handle target = handle_of(trap.arg(1));
        const cap::Handle descriptor = handle_of(trap.arg(2));
        if (!source || !descriptor) {
            return returned(STATUS_BAD_ARGS);
        }
        byte bytes[CAP_ATTENUATION_SIZE]{};
        auto read = read_desc_bytes(
            inv,
            descriptor,
            trap.arg(3),
            std::span<byte>{bytes, sizeof(bytes)});
        if (!read) {
            return returned(read.error());
        }
        auto decoded = cap::decode_attenuation(
            std::span<const byte>{bytes, sizeof(bytes)});
        if (!decoded) {
            return returned(STATUS_BAD_ARGS);
        }
        auto fee = reserve_grant(inv, trap.arg(4));
        if (!fee) return returned(fee.error());
        if (!target) {
            auto delegated = cspace.transfer(source, cspace, cap::XferOp::Derive, decoded.value(), std::move(*fee));
            return returned(
                delegated ? STATUS_OK : cap_status(delegated.error()),
                delegated ? delegated.value().raw() : 0);
        }
        auto resolved = dest(cspace, target);
        if (!resolved) {
            return returned(cap_status(resolved.error()));
        }
        auto delegated = cspace.transfer(source, resolved.value().object(), cap::XferOp::Derive, decoded.value(), std::move(*fee));
        return returned(
            delegated ? STATUS_OK : cap_status(delegated.error()),
            delegated ? delegated.value().raw() : 0);
    }
    if constexpr (op == SYS_CAP_DUPLICATE
        || op == SYS_CAP_DELEGATE) {
        const cap::Handle source = handle_of(trap.arg(0));
        const cap::Handle target = handle_of(trap.arg(1));
        const auto rights = rights_of(trap.arg(2));
        if (!source || !rights) {
            return returned(STATUS_BAD_ARGS);
        }
        auto fee = [&]() -> std::expected<resource::Reservation, status_t> {
            if constexpr (op == SYS_CAP_DELEGATE) return reserve_grant(inv, trap.arg(3));
            else return resource::Reservation{};
        }();
        if (!fee) return returned(fee.error());
        return with_dest(cspace, target, [&](cap::CSpace& out) {
            return cspace.transfer(source, out,
                op == SYS_CAP_DUPLICATE ? cap::XferOp::Copy : cap::XferOp::Derive, *rights, std::move(*fee));
        });
    }
    if constexpr (op == SYS_CAP_MOVE) {
        const cap::Handle source = handle_of(trap.arg(0));
        const cap::Handle target = handle_of(trap.arg(1));
        if (!source) {
            return returned(STATUS_BAD_ARGS);
        }
        return with_dest(cspace, target, [&](cap::CSpace& out) {
            return cspace.transfer(source, out, cap::XferOp::Move);
        });
    }
    if constexpr (op == SYS_CAP_REVOKE) {
        if (!thread || thread->waiting()) return returned(STATUS_BUSY);
        if (trap.arg(2) & ~u64{CAP_REVOKE_ASYNC}) return returned(STATUS_BAD_ARGS);
        std::optional<cap::Resolved<cap::CSpace>> destination;
        auto* owner = &cspace;
        if (trap.arg(1)) {
            auto target = dest(cspace, handle_of(trap.arg(1)));
            if (!target) return returned(cap_status(target.error()));
            destination = std::move(*target); owner = &destination->object();
        }
        Source::Context context{*thread, *inv.cpu.cpus, inv.cpu.dispatcher()};
        auto result = owner->revoke(handle_of(trap.arg(0)), trap.arg(2) & CAP_REVOKE_ASYNC ? nullptr : &context,
            cap::GrantId{trap.arg(3)});
        if (!result) return returned(cap_status(result.error()));
        if (result->status == STATUS_PENDING && !(trap.arg(2) & CAP_REVOKE_ASYNC)) return {STATUS_PENDING, 0, Disposition::Park};
        return returned(result->status, result->value, result->sequence);
    }
    if constexpr (op == SYS_OBJECT_DESTROY) {
        auto destroyed = cspace.destroy(handle_of(trap.arg(0)));
        return returned(
            destroyed ? STATUS_OK : cap_status(destroyed.error()));
    }
    return returned(STATUS_INVALID_OP);
}

auto mem_status(mm::MemErr error) noexcept
    -> status_t {
    switch (error) {
    case mm::MemErr::OutOfMemory:
    case mm::MemErr::ResourceExhausted:
    case mm::MemErr::GenerationExhausted:
        return STATUS_NO_MEMORY;
    case mm::MemErr::Dirty: return STATUS_DIRTY;
    case mm::MemErr::Busy:
    case mm::MemErr::InvalidState:
    case mm::MemErr::AttachmentState:
        return STATUS_BUSY;
    case mm::MemErr::Pending:
        return STATUS_WOULD_BLOCK;
    case mm::MemErr::BackingFailed:
        return STATUS_BACKING_FAILED;
    case mm::MemErr::InvalidSize:
    case mm::MemErr::InvalidRange:
    case mm::MemErr::InvalidAccess:
    case mm::MemErr::NotRam:
    case mm::MemErr::NotBacked:
    case mm::MemErr::OwnershipMismatch:
        return STATUS_BAD_ARGS;
    }
    return STATUS_INTERNAL;
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
    return returned(sealed ? STATUS_OK : mem_status(sealed.error()));
}

template<usize op>
[[nodiscard]] auto memory_copy(Call& inv) noexcept -> Result {
    constexpr bool write = op == SYS_MEMORY_WRITE;
    auto memory = inv.cspace.resolve<mm::Mem>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Map));
    if (!memory) return returned(cap_status(memory.error()));
    const usize offset = inv.trap.arg(1), aperture = inv.trap.arg(2), size = inv.trap.arg(3);
    if (!size || size > mm::page_size - offset % mm::page_size) return returned(STATUS_BAD_ARGS);
    const auto effective = memory->view();
    const auto* limit = std::get_if<cap::MemLimit>(&effective.data);
    if (!limit || !limit->range.contains(mm::ObjectRange{offset / mm::page_size, 1})
        || !limit->perms.contains(write ? mm::Perm::Write : mm::Perm::Read)) return returned(STATUS_BAD_RIGHTS);
    auto* buffer = inv.target->ipc_buffer();
    if (!buffer) return returned(STATUS_BAD_ARGS);
    auto access = buffer->access();
    if (!access) return returned(STATUS_RETRY);
    const auto bytes = access->bytes<write ? mm::Perm::Read : mm::Perm::Write>(aperture, size);
    if (bytes.empty()) return returned(STATUS_BAD_ARGS);
    // Both page pins survive this one bounded, alias-safe copy. No stack
    // snapshot or revocable userspace mapping is needed in either direction.
    const auto result = [&] {
        if constexpr (write) return memory.value()->write(offset, bytes);
        else return memory.value()->read(offset, bytes);
    }();
    return returned(result ? STATUS_OK : mem_status(result.error()));
}

template<usize op>
[[nodiscard]] auto mem_call(Call& inv) noexcept -> Result {
    auto* thread = inv.target;
    if (thread == nullptr) return returned(STATUS_INVALID_OP);
    if (thread->waiting()) return returned(STATUS_BUSY);
    auto memory = inv.cspace.resolve<mm::Mem>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Manage));
    if (!memory) return returned(cap_status(memory.error()));
    const usize page = inv.trap.arg(1);
    const mm::ObjectRange range{page, op == SYS_MEM_TRIM ? inv.trap.arg(2) : 1};
    if (range.empty() || !range.limit() || *range.limit() > memory.value()->page_count())
        return returned(STATUS_BAD_ARGS);
    const auto view = memory->view();
    const auto* limit = std::get_if<cap::MemLimit>(&view.data);
    if (!limit || !limit->range.contains(range))
        return returned(STATUS_BAD_RIGHTS);
    if (page >= memory.value()->size() / mm::page_size) return returned(STATUS_BAD_ARGS);
    auto ref = memory->reference();
    if (!ref) return returned(STATUS_BUSY);
    auto& mem = memory->object();
    auto* cpus = inv.cpu.cpus;
    libk_assert(cpus);
    const auto result = [&]() {
        if constexpr (op == SYS_MEM_TRIM) {
            Source::Context context{*thread, *cpus, inv.cpu.dispatcher()};
            std::array<const cap::GrantPin*, 1> grants{&memory->pin()};
            std::array<const object::ref<>*, 1> gates{&*ref};
            return mem.trim(range, context, inv.cpu.grants.admission(grants), gates, &memory->pin());
        }
        else if constexpr (op == SYS_MEM_WRITEBACK) return mem.writeback(page);
        else {
            Source::Context context{*thread, *cpus, inv.cpu.dispatcher()};
            const auto page_result = mem.materialize(page, &context);
            return page_result ? std::expected<void, mm::MemErr>{} : std::unexpected(page_result.error());
        }
    }();
    if constexpr (op == SYS_MEMORY_POPULATE)
        if (!result && result.error() == mm::MemErr::Pending) return {STATUS_PENDING, 0, Disposition::Retry};
    if constexpr (op == SYS_MEM_TRIM)
        if (!result && result.error() == mm::MemErr::Pending) return {STATUS_PENDING, 0, Disposition::Park};
    return returned(thread->stop_requested() ? STATUS_CANCELED
                    : result ? STATUS_OK : mem_status(result.error()));
}

template<usize op>
[[nodiscard]] auto scope_close(Call& inv) noexcept -> Result {
    auto scope = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Close));
    if (!scope) return returned(cap_status(scope.error()));
    return returned(STATUS_OK, static_cast<u64>(scope.value()->close()));
}

template<usize op>
[[nodiscard]] auto scope_members(Call& inv) noexcept -> Result {
    auto scope = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Inspect));
    if (!scope) return returned(cap_status(scope.error()));
    const auto view = scope->view();
    const auto* limit = std::get_if<cap::ScopeLimit>(&view.data);
    if (!limit || !limit->depth || !(limit->facets & SCOPE_MEMBERS)) return returned(STATUS_DENIED);
    auto source = scope->reference();
    if (!source) return returned(STATUS_BUSY);
    ScopeMembers page;
    const auto status = scope.value()->inspect(source->id(),
        {inv.trap.arg(2), inv.trap.arg(3), inv.trap.arg(4)}, limit->kinds, page, limit->depth);
    if (status != STATUS_OK) return returned(status);
    auto* buffer = inv.target->ipc_buffer();
    if (!buffer || !buffer->write(inv.trap.arg(1), {reinterpret_cast<const byte*>(&page), sizeof(page)}))
        return returned(STATUS_BAD_ARGS);
    return returned(STATUS_OK, sizeof(page));
}

template<usize op>
[[nodiscard]] auto scope_query(Call& inv) noexcept -> Result {
    auto source = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Inspect));
    if (!source) return returned(cap_status(source.error()));
    const auto view = source->view();
    const auto* limit = std::get_if<cap::ScopeLimit>(&view.data);
    const auto facet = inv.trap.arg(2);
    if (!limit || !std::has_single_bit(facet) || !(limit->facets & facet)) return returned(STATUS_DENIED);
    auto& scope = source->object();
    const object::Id id{inv.trap.arg(1)};
    auto write = [&](const auto& record) noexcept -> Result {
        // Membership may change while a typed provider samples its own lock.
        if (!scope.contains(id, limit->kinds, limit->depth)) return returned(STATUS_STALE);
        auto* buffer = inv.target->ipc_buffer();
        if (!buffer || !buffer->write(inv.trap.arg(5), {reinterpret_cast<const byte*>(&record), sizeof(record)}))
            return returned(STATUS_BAD_ARGS);
        return returned(STATUS_OK, sizeof(record));
    };
    if (facet == SCOPE_INFO) {
        auto info = scope.describe(id, limit->kinds, limit->depth);
        return info ? write(*info) : returned(info.error());
    }
    auto target = scope.lookup(id, limit->kinds, limit->depth);
    if (!target) return returned(STATUS_NOT_FOUND);
    if (facet == SCOPE_MEMBERS) {
        auto group = target.as<object::Scope>();
        if (!group) return returned(STATUS_INVALID_OP);
        ScopeMembers page;
        const auto cursor = inv.trap.arg(3) || inv.trap.arg(4)
            ? ScopeCursor{id.value, inv.trap.arg(4), inv.trap.arg(3)} : ScopeCursor{};
        const auto status = group->get().inspect(id, cursor, limit->kinds, page, limit->depth);
        if (status != STATUS_OK) return returned(status);
        auto* end = std::remove_if(page.records, page.records + page.count, [&](const ScopeMember& row) noexcept {
            return !scope.contains(object::Id{row.object_id}, limit->kinds, limit->depth);
        });
        page.count = end - page.records;
        return write(page);
    }
    if (facet == SCOPE_CAPS) {
        auto space = target.as<cap::CSpace>();
        if (!space) return returned(STATUS_INVALID_OP);
        CSpaceInfo page;
        auto sampled = space->get().inspect(handle_of(inv.trap.arg(3)), page, inv.trap.arg(4));
        if (!sampled) return returned(sampled.error() == cap::CSpaceError::InvalidHandle ? STATUS_STALE : cap_status(sampled.error()));
        for (auto& row : std::span{page.records, page.count}) {
            if (row.object_id && !scope.contains(object::Id{row.object_id}, limit->kinds, limit->depth)) {
                row.object_id = row.grant_id = 0; row.state |= CAP_INFO_BOUNDARY;
            }
        }
        return write(page);
    }
    if (facet == SCOPE_LINKS) {
        LinkRecords page;
        status_t status = STATUS_INVALID_OP;
        if (auto space = target.as<mm::VSpace>()) status = space->get().links(inv.trap.arg(3), inv.trap.arg(4), page);
        else if (auto space = target.as<io::Space>()) status = space->get().links(inv.trap.arg(3), inv.trap.arg(4), page);
        else {
            page = {}; page.version = 1; page.size = sizeof(page);
            status = object::Types::visit(target.kind(), [&]<class T>() noexcept -> status_t {
                auto& value = *static_cast<T*>(target.get());
                if constexpr (requires { value.links(inv.trap.arg(3), page); }) value.links(inv.trap.arg(3), page);
                else if constexpr (requires { value.op(); }) value.op().links(inv.trap.arg(3), page);
                else return STATUS_INVALID_OP;
                return page.epoch == UINT64_MAX || (inv.trap.arg(4) && inv.trap.arg(4) != page.epoch) ? STATUS_STALE : STATUS_OK;
            }, STATUS_INVALID_OP);
        }
        if (status != STATUS_OK) return returned(status);
        for (auto& row : std::span{page.records, page.count}) {
            if (row.object_id && !scope.contains(object::Id{row.object_id}, limit->kinds, limit->depth)) {
                row.object_id = row.grant_id = 0; row.flags |= OBS_BOUNDARY;
            } else if (!(limit->facets & SCOPE_GRANTS)) row.grant_id = 0;
        }
        return write(page);
    }
    if (facet == SCOPE_GRANTS) {
        auto space = target.as<cap::CSpace>();
        if (!space) return returned(STATUS_INVALID_OP);
        GrantQuery query;
        auto* buffer = inv.target->ipc_buffer();
        if (!buffer || !buffer->read(inv.trap.arg(5), {reinterpret_cast<byte*>(&query), sizeof(query)}))
            return returned(STATUS_BAD_ARGS);
        GrantRecords page;
        const auto status = space->get().inspect(query, limit->depth, page);
        if (status != STATUS_OK) return returned(status);
        auto clip = [&](GrantRecord& row) noexcept {
            if (row.object_id && !scope.contains(object::Id{row.object_id}, limit->kinds, limit->depth)) {
                row.object_id = 0; row.flags |= OBS_BOUNDARY;
            }
        };
        clip(page.root);
        for (auto& row : std::span{page.records, page.count}) clip(row);
        return write(page);
    }
    if (facet == SCOPE_USAGE) {
        if (auto thread = target.as<Thread>()) return write(thread->get().info(inv.cpu.dispatcher().clock().rd_ticks()));
        if (auto quota = target.as<resource::Quota>()) return write(quota->get().info(0));
        if (auto domain = target.as<sched::ScDomain>()) return write(domain->get().info(0));
        return returned(STATUS_INVALID_OP);
    }
    if (facet == SCOPE_OPS) {
        return object::Types::visit(target.kind(), [&]<class T>() noexcept -> Result {
            if constexpr (requires(T& value) { value.op(); }) {
                const auto result = static_cast<T*>(target.get())->op().query();
                if (!scope.contains(id, limit->kinds, limit->depth)) return returned(STATUS_STALE);
                return returned(result.status, result.value, result.sequence);
            } else return returned(STATUS_INVALID_OP);
        }, returned(STATUS_INVALID_OP));
    }
    return returned(STATUS_INVALID_OP);
}

static auto whole(const object::ref<>& target, const cap::View& view) noexcept -> bool {
    if (target.kind() == ObjVSpace) {
        const auto* limit = std::get_if<cap::VmLimit>(&view.data);
        return limit && limit->range == mm::VRange{mm::va_t{mm::UserBegin}, mm::UserEnd - mm::UserBegin};
    }
    if (target.kind() == ObjMem) {
        const auto* limit = std::get_if<cap::MemLimit>(&view.data);
        return limit && limit->range == mm::ObjectRange{0, static_cast<mm::Mem*>(target.get())->page_count()};
    }
    return true;
}

template<usize op>
[[nodiscard]] auto scope_adopt(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<void>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Adopt));
    auto source = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto destination = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(2)), cap::Rights::of(cap::Right::Create));
    if (!target) return returned(cap_status(target.error()));
    if (!source) return returned(cap_status(source.error()));
    if (!destination) return returned(cap_status(destination.error()));
    auto object = target->reference();
    auto from = source->reference();
    auto to = destination->reference();
    if (!object || !from || !to) return returned(STATUS_BUSY);
    const auto source_view = source->view(), destination_view = destination->view();
    const auto* a = std::get_if<cap::ScopeLimit>(&source_view.data);
    const auto* b = std::get_if<cap::ScopeLimit>(&destination_view.data);
    if (!a || !b || !(a->kinds & b->kinds & OBJ_BIT(object->kind()))) return returned(STATUS_DENIED);
    if (!whole(*object, target->view())) return returned(STATUS_DENIED);
    const auto check = inv.target->check_control(inv.cpu.objects.pmm(), *to, *object, 0);
    if (!check) return {check.error(), 0, check.error() == STATUS_PENDING ? Disposition::Continue : Disposition::Return};
    if (check->reachable) return returned(STATUS_DENIED);
    std::array<const cap::GrantPin*, 3> grants{&target->pin(), &source->pin(), &destination->pin()};
    auto admission = target->pin().graph().admission(std::span{grants});
    auto result = object::Scope::adopt(*object, std::move(*from).as<object::Scope>().value(),
        std::move(*to).as<object::Scope>().value(), inv.trap.arg(3), admission, &*check);
    if (result) return returned(STATUS_OK, *result);
    return returned(result.error());
}

template<usize op>
[[nodiscard]] auto object_info(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<void>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Inspect));
    if (!target) return returned(cap_status(target.error()));
    const auto view = target->view();
    if (const auto* scope = std::get_if<cap::ScopeLimit>(&view.data); scope && !(scope->facets & SCOPE_INFO))
        return returned(STATUS_DENIED);
    auto object = target->reference();
    if (!object) return returned(STATUS_BUSY);
    const ObjectInfo info{1, sizeof(ObjectInfo), object->id().value, object->allocation()->epoch(),
        static_cast<u32>(object->kind()), object->live() ? OBJECT_OPEN : OBJECT_CLOSING};
    auto* buffer = inv.target->ipc_buffer();
    if (!buffer || !buffer->write(inv.trap.arg(1), {reinterpret_cast<const byte*>(&info), sizeof(info)}))
        return returned(STATUS_BAD_ARGS);
    return returned(STATUS_OK, sizeof(info));
}

template<usize op>
[[nodiscard]] auto object_reassign(Call& inv) noexcept -> Result {
    const bool adopt = inv.trap.arg(4) || inv.trap.arg(5) || inv.trap.arg(6);
    if (adopt && (!inv.trap.arg(4) || !inv.trap.arg(5) || !inv.trap.arg(6))) return returned(STATUS_BAD_ARGS);
    const auto rights = adopt ? cap::Rights::of(cap::Right::Reassign, cap::Right::Adopt) : cap::Rights::of(cap::Right::Reassign);
    auto target = inv.cspace.resolve<void>(handle_of(inv.trap.arg(0)), rights);
    auto old = inv.cspace.resolve<resource::Quota>(handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Reassign));
    auto next = inv.cspace.resolve<resource::Quota>(handle_of(inv.trap.arg(2)), cap::Rights::of(cap::Right::Create));
    if (!target) return returned(cap_status(target.error()));
    if (!old) return returned(cap_status(old.error()));
    if (!next) return returned(cap_status(next.error()));
    auto object = target->reference(), from = old->reference(), to = next->reference();
    if (!object || !from || !to) return returned(STATUS_BUSY);
    const auto next_view = next->view();
    const auto* limit = std::get_if<cap::QuotaLimit>(&next_view.data);
    if (!limit || !(limit->object_kinds & OBJ_BIT(object->kind()))) return returned(STATUS_DENIED);
    if (!whole(*object, target->view())) return returned(STATUS_DENIED);
    std::optional<cap::Resolved<object::Scope>> source, destination;
    object::ref<object::Scope> source_owner, destination_owner;
    std::array<const cap::GrantPin*, 5> grants{&target->pin(), &old->pin(), &next->pin(), nullptr, nullptr};
    if (adopt) {
        auto from_scope = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(4)), cap::Rights::of(cap::Right::Manage));
        auto to_scope = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(5)), cap::Rights::of(cap::Right::Create));
        if (!from_scope) return returned(cap_status(from_scope.error()));
        if (!to_scope) return returned(cap_status(to_scope.error()));
        const auto a = from_scope->view(), b = to_scope->view();
        if (!(std::get<cap::ScopeLimit>(a.data).kinds & std::get<cap::ScopeLimit>(b.data).kinds & OBJ_BIT(object->kind())))
            return returned(STATUS_DENIED);
        auto source_ref = from_scope->reference(), destination_ref = to_scope->reference();
        if (!source_ref || !destination_ref) return returned(STATUS_BUSY);
        source_owner = std::move(*source_ref).as<object::Scope>().value();
        destination_owner = std::move(*destination_ref).as<object::Scope>().value();
        source.emplace(std::move(*from_scope)); destination.emplace(std::move(*to_scope));
        grants[3] = &source->pin(); grants[4] = &destination->pin();
    }
    std::optional<object::Control::Check> scope_check;
    if (adopt) {
        const auto check = inv.target->check_control(inv.cpu.objects.pmm(), destination_owner, *object, 0);
        if (!check) return {check.error(), 0, check.error() == STATUS_PENDING ? Disposition::Continue : Disposition::Return};
        if (check->reachable) return returned(STATUS_DENIED);
        scope_check = *check;
    }
    const auto quota_check = inv.target->check_control(inv.cpu.objects.pmm(), *to, *object, 1);
    if (!quota_check) return {quota_check.error(), 0, quota_check.error() == STATUS_PENDING ? Disposition::Continue : Disposition::Return};
    if (quota_check->reachable) return returned(STATUS_DENIED);
    object::Transfer admission{*object, std::move(source_owner), std::move(destination_owner), inv.trap.arg(6),
        target->pin().graph().admission(std::span{grants.data(), adopt ? usize{5} : usize{3}}), scope_check ? &*scope_check : nullptr};
    const auto valid = admission.validate();
    if (valid != STATUS_OK) return returned(valid);
    auto result = object::Types::visit(object->kind(), [&]<class T>() -> std::expected<u64, status_t> {
        auto typed = object->as<T>();
        libk_assert(typed);
        auto& allocation = *object->allocation();
        auto transaction = [&](auto&& visit, auto&& commit) {
            return object::reassign(inv.cpu.objects.pmm(), allocation, *object, *from, *to,
                inv.trap.arg(3), limit->budget, visit, commit, admission, &*quota_check);
        };
        if constexpr (requires { typed->get().reassign_usage(*to, transaction); })
            return typed->get().reassign_usage(*to, transaction);
        else if constexpr (requires { T::fixed_usage; }) {
            static_assert(T::fixed_usage);
            return object::reassign(inv.cpu.objects.pmm(), allocation, *object, *from, *to,
                inv.trap.arg(3), limit->budget, [](auto&&) {}, [] {}, admission, &*quota_check);
        } else return std::unexpected(STATUS_INVALID_OP);
    }, std::expected<u64, status_t>{std::unexpected(STATUS_INVALID_OP)});
    return result ? returned(STATUS_OK, *result, admission.epoch()) : returned(result.error());
}

template<usize op>
[[nodiscard]] auto quota_resize(Call& inv) noexcept -> Result {
    auto quota = inv.cspace.resolve<resource::Quota>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Control));
    if (!quota) return returned(cap_status(quota.error()));
    const auto view = quota->view();
    const auto& limit = std::get<cap::QuotaLimit>(view.data);
    const resource::Capacity target{inv.trap.arg(1), inv.trap.arg(2)};
    if (!limit.budget.contains(target)) return returned(STATUS_DENIED);
    auto self = quota->reference();
    if (!self) return returned(STATUS_BUSY);
    std::optional<cap::Resolved<resource::Quota>> source;
    object::ref<> source_ref;
    resource::Capacity ceiling;
    std::array<const cap::GrantPin*, 2> grants{&quota->pin(), nullptr};
    if (inv.trap.arg(3)) {
        auto resolved = inv.cspace.resolve<resource::Quota>(handle_of(inv.trap.arg(3)), cap::Rights::of(cap::Right::Split));
        if (!resolved) return returned(cap_status(resolved.error()));
        auto ref = resolved->reference();
        if (!ref) return returned(STATUS_BUSY);
        source_ref = std::move(*ref);
        ceiling = std::get<cap::QuotaLimit>(resolved->view().data).budget;
        source.emplace(std::move(*resolved));
        grants[1] = &source->pin();
    }
    auto admission = inv.cpu.grants.admission(std::span{grants.data(), source ? 2u : 1u});
    return returned(quota->object().resize(*self, target, source_ref, ceiling,
        [&](auto gates, auto&& commit) { return admission.publish(gates, commit); }));
}

template<usize op>
[[nodiscard]] auto quota_close(Call& inv) noexcept -> Result {
    auto quota = inv.cspace.resolve<resource::Quota>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Close));
    if (!quota) return returned(cap_status(quota.error()));
    const auto phase = quota->object().close();
    return returned(STATUS_OK, static_cast<u32>(phase));
}

[[nodiscard]] static auto pager_error(Pager::Error error) noexcept -> status_t {
    switch (error) {
    case Pager::Error::Closed:
        return STATUS_CLOSED;
    case Pager::Error::InvalidRange:
        return STATUS_BAD_ARGS;
    case Pager::Error::Busy:
        return STATUS_WOULD_BLOCK;
    case Pager::Error::Stale:
    case Pager::Error::GenerationExhausted:
        return STATUS_RETRY;
    }
    return STATUS_INTERNAL;
}

[[nodiscard]] static auto irq_error(irq::Error error) noexcept -> status_t {
    switch (error) {
    case irq::Error::BadSequence:
        return STATUS_BAD_ARGS;
    case irq::Error::InvalidState:
    case irq::Error::Busy:
        return STATUS_BUSY;
    case irq::Error::StaleSequence:
        return STATUS_REASSERTED;
    case irq::Error::Closed:
        return STATUS_CLOSED;
    }
    return STATUS_INTERNAL;
}



[[nodiscard]] static auto pager_descriptor(
    const Pager::Req& request) noexcept -> PagerReq {
    PagerReq descriptor{};
    descriptor.kind = request.kind == Pager::Kind::PageIn
        ? PAGER_REQUEST_PAGE_IN : PAGER_REQUEST_WRITEBACK;
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
        return returned(STATUS_WOULD_BLOCK);
    }
    auto access = buffer->access();
    if (!access) {
        return returned(STATUS_RETRY);
    }
    auto storage = resolved->reference();
    if (!storage) return returned(STATUS_CLOSED);
    const cap::GrantPin* grants[]{&resolved->pin()};
    const object::ref<>* gates[]{&*storage};
    const auto request = resolved.value()->claim(resolved->pin().graph().admission(grants), gates);
    if (!request) {
        return returned(pager_error(request.error()));
    }
    const auto descriptor = pager_descriptor(request.value());
    if (!access.value().write(
            0,
            std::span<const byte>{
                reinterpret_cast<const byte*>(&descriptor),
                sizeof(descriptor)})) {
        static_cast<void>(resolved.value()->requeue(
            request.value().id));
        return returned(STATUS_RETRY);
    }
    return returned(STATUS_OK, request.value().id);
}

template<usize op>
static auto pg_finish(Call& inv) noexcept -> Result {
    const auto right = op == SYS_PAGER_REQUEUE ? cap::Right::Serve : cap::Right::Supply;
    auto pager = inv.cspace.resolve<Pager>(handle_of(inv.trap.arg(0)), cap::Rights::of(right));
    if (!pager) return returned(cap_status(pager.error()));
    if constexpr (op == SYS_PAGER_REQUEUE) {
        auto storage = pager->reference();
        if (!storage) return returned(STATUS_CLOSED);
        const cap::GrantPin* grants[]{&pager->pin()};
        const object::ref<>* gates[]{&*storage};
        const auto result = pager.value()->requeue(inv.trap.arg(1), pager->pin().graph().admission(grants), gates);
        return result ? returned(STATUS_OK) : returned(pager_error(result.error()));
    }
    auto memory = inv.cspace.resolve<mm::Mem>(handle_of(inv.trap.arg(1)),
        cap::Rights::of(cap::Right::Manage));
    if (!memory) return returned(cap_status(memory.error()));
    auto pager_ref = pager->reference();
    auto memory_ref = memory->reference();
    if (!pager_ref || !memory_ref) return returned(STATUS_CLOSED);
    const cap::GrantPin* grants[]{&pager->pin(), &memory->pin()};
    const object::ref<>* gates[]{&*pager_ref, &*memory_ref};
    const auto result = memory.value()->pager_finish(pager.value().object(), inv.trap.arg(2),
        op == SYS_PAGER_FAIL, pager->pin().graph().admission(grants), gates);
    return result ? returned(STATUS_OK) : returned(mem_status(result.error()));
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
    auto pager_ref = pager->reference();
    auto target_ref = target->reference();
    auto source_ref = source->reference();
    if (!pager_ref || !target_ref || !source_ref) return returned(STATUS_CLOSED);
    const cap::GrantPin* grants[]{&pager->pin(), &target->pin(), &source->pin()};
    const object::ref<>* gates[]{&*pager_ref, &*target_ref, &*source_ref};
    auto supplied = target.value()->supply(pager.value().object(), std::move(*transfer),
        inv.trap.arg(4), pager->pin().graph().admission(grants), gates);
    return supplied ? returned(STATUS_OK)
                    : returned(mem_status(supplied.error()));
}

template<usize op>
static auto irq_call(Call& inv) noexcept -> Result {
    if constexpr (op == SYS_IRQ_SET) {
        if (inv.trap.arg(1) > 1) return returned(STATUS_BAD_ARGS);
        auto irq = inv.cspace.resolve<irq::Irq>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Route));
        if (!irq) return returned(cap_status(irq.error()));
        const auto result = irq->object().enable(inv.trap.arg(1));
        return result ? returned(STATUS_OK, *result) : returned(irq_error(result.error()));
    }
    if constexpr (op != SYS_IRQ_OBSERVE
        && op != SYS_IRQ_ACK) {
        return returned(STATUS_BAD_ARGS);
    }
    const cap::Right right = op == SYS_IRQ_OBSERVE
        ? cap::Right::Observe
        : cap::Right::Ack;
    auto irq = inv.cspace.resolve<irq::Irq>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(right));
    if (!irq) {
        return returned(cap_status(irq.error()));
    }
    if constexpr (op == SYS_IRQ_OBSERVE) {
        const auto result = irq.value()->delivery();
        return result
            ? returned(STATUS_OK, result.value().sequence,
                result.value().generation)
            : returned(irq_error(result.error()));
    }
    const auto result = irq.value()->ack(
        inv.trap.arg(1), inv.trap.arg(2));
    return result ? returned(STATUS_OK)
                  : returned(irq_error(result.error()));
}

template<usize op>
auto pg_call(Call& inv) noexcept -> Result {
    if constexpr (op == SYS_PAGER_CLAIM) {
        return pg_claim(inv);
    }
    if constexpr (op == SYS_PAGER_REQUEUE
        || op == SYS_PAGER_COMPLETE
        || op == SYS_PAGER_FAIL) {
        return pg_finish<op>(inv);
    }
    if constexpr (op == SYS_PAGER_SUPPLY) {
        return pg_supply(inv);
    }
    return irq_call<op>(inv);
}

template<usize op>
auto vm_exec(Call& inv) noexcept -> Result {
    auto vm = inv.cspace.resolve<mm::VSpace>(handle_of(inv.trap.arg(0)), cap::Rights::of(
        op == SYS_VSPACE_STOP ? cap::Right::Control : cap::Right::Observe));
    if (!vm) return returned(cap_status(vm.error()));
    const auto view = vm->view();
    const auto* limit = std::get_if<cap::VmLimit>(&view.data);
    if (!limit || !vm->object().can_destroy_object(*limit)) return returned(STATUS_DENIED);
    if constexpr (op == SYS_VSPACE_STOP) { vm->object().stop_exec(); return returned(STATUS_OK); }
    const auto info = vm->object().exec_info();
    auto* ipc = inv.target->ipc_buffer();
    return returned(ipc && ipc->write(inv.trap.arg(1), std::span{reinterpret_cast<const byte*>(&info), sizeof(info)}) ? STATUS_OK : STATUS_BAD_ARGS);
}

template<usize op>
auto exit_call(Call& call) noexcept -> Result {
    auto thread = call.cspace.resolve<Thread>(handle_of(call.trap.arg(0)),
        cap::Rights::of(cap::Right::Observe));
    if (!thread) return returned(cap_status(thread.error()));
    const auto result = thread.value()->exit().read();
    return returned(STATUS_OK, result.has_value(),
        static_cast<usize>(static_cast<isize>(result.value_or(STATUS_OK))));
}

template<usize op>
auto io_call(Call& inv) noexcept -> Result {
    const auto handle = handle_of(inv.trap.arg(0));
    if constexpr (op == SYS_IOMMU_STATE) {
        auto ctl = inv.cspace.resolve<io::Iommu>(handle, cap::Rights::of(cap::Right::Inspect));
        return ctl ? returned(STATUS_OK, ctl->object().state()) : returned(cap_status(ctl.error()));
    } else {
        constexpr auto right = op == SYS_IO_SPACE_STATE ? cap::Right::Inspect : cap::Right::Close;
        auto domain = inv.cspace.resolve<io::Space>(handle, cap::Rights::of(right));
        if (!domain) return returned(cap_status(domain.error()));
        if constexpr (op == SYS_IO_SPACE_STATE) return returned(STATUS_OK, domain->object().state());
        if constexpr (op == SYS_IO_SPACE_CLOSE) { domain->object().close(); return returned(STATUS_OK); }

    }
    return returned(STATUS_INVALID_OP);
}

template<usize op> auto op_call(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<void>(handle_of(inv.trap.arg(0)), cap::Rights::of(
        op == SYS_OP_QUERY ? cap::Right::Inspect : cap::Right::Control));
    if (!target) return returned(cap_status(target.error()));
    return object::Types::visit(target->pin().kind(), [&]<class T>() noexcept -> Result {
        if constexpr (requires(T& value) { value.op(); }) {
            auto& value = *static_cast<T*>(target->pin().get());
            if constexpr (op == SYS_OP_CANCEL) return returned(value.op().cancel());
            else { const auto r = value.op().query(); return returned(r.status, r.value, r.sequence); }
        } else return returned(STATUS_INVALID_OP);
    }, returned(STATUS_INVALID_OP));
}

template<usize op>
auto vm_call(Call& inv) noexcept -> Result {
    arch::TrapCtx& trap = inv.trap;
    cap::CSpace& cspace = inv.cspace;
    const cap::Handle vspace_handle = handle_of(trap.arg(0));
    const auto required = op == SYS_VM_SLICE ? cap::Right::Delegate : cap::Right::Reserve;
    if (!vspace_handle) {
        return returned(STATUS_BAD_ARGS);
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
    if constexpr (op == SYS_VM_RESERVE) {
        if (!effective.rights.contains(cap::Right::Delegate)) return returned(STATUS_BAD_RIGHTS);
        VmAlloc request{};
        auto* ipc = inv.target->ipc_buffer();
        if (!ipc || trap.arg(2) || trap.arg(3) || trap.arg(4) || trap.arg(5)
            || !ipc->read(trap.arg(1), std::span{reinterpret_cast<byte*>(&request), sizeof(request)}))
            return returned(STATUS_BAD_ARGS);
        cap::Handle handle{};
        auto publish = [&](cap::VmLimit limit) noexcept -> status_t {
            const auto rights = effective.rights.intersect(cap::Rights::of(cap::Right::Reserve,
                cap::Right::Map, cap::Right::Unmap, cap::Right::Protect,
                cap::Right::Inspect, cap::Right::Duplicate, cap::Right::Delegate));
            auto created = cspace.transfer(vspace_handle, cspace, cap::XferOp::Derive,
                cap::View{rights, limit});
            if (!created) return cap_status(created.error());
            handle = *created; return STATUS_OK;
        };
        auto result = space.reserve(request, *where, libk::delegate<status_t(cap::VmLimit)>::bind(publish));
        if (!result && result.error() == STATUS_RETRY) return vm_wait(inv, space);
        return returned(result ? STATUS_OK : result.error(), handle.raw(), result ? result->range.base().raw() : 0);
    }
    const auto range = range_of(trap.arg(1), trap.arg(2));
    if (!range) {
        return returned(STATUS_BAD_ARGS);
    }
    if constexpr (op == SYS_VM_SLICE) {
        const auto access = perms_of(trap.arg(3));
        const auto rights = rights_of(trap.arg(4));
        if (!access || !rights || trap.arg(5) != 0) {
            return returned(STATUS_BAD_ARGS);
        }
        const cap::View slice{*rights, cap::VmLimit{*range,*access,where->claim()}};
        auto created = cspace.transfer(vspace_handle, cspace, cap::XferOp::Derive, slice);
        return returned(created ? STATUS_OK : cap_status(created.error()),
            created ? created->raw() : 0);
    }
    if constexpr (op == SYS_VM_GUARD) {
        if (!where->range.contains(*range)) return returned(STATUS_DENIED);
        auto reserved = space.guard(*where, *range);
        if (!reserved && reserved.error() == mm::VSpaceError::Contended) return vm_wait(inv, space);
        return returned(reserved ? STATUS_OK : mm::status(reserved.error()));
    }
    return returned(STATUS_INVALID_OP);
}

[[nodiscard]] static auto bind_target(
    cap::Resolved<sched::Sc>& ctx,
    cap::Resolved<Thread>& target) noexcept -> Result {
    auto reference = ctx.reference();
    if (!reference) return returned(STATUS_BUSY);
    auto hold = std::move(*reference).as<sched::Sc>();
    if (!hold) return returned(STATUS_BUSY);
    auto bound = ctx->bind(std::move(*hold), target.object(), ctx, target);
    return returned(bound ? STATUS_OK : STATUS_BUSY, bound ? ctx->generation() : 0);
}

template<usize op>
[[nodiscard]] auto sc_bind(Call& inv) noexcept -> Result {
    auto ctx = inv.cspace.resolve<sched::Sc>(
        handle_of(inv.trap.arg(0)),
        cap::Rights::of(cap::Right::Attach));
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

template<usize op>
[[nodiscard]] auto execution_set(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<Thread>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Control));
    if (!target) return returned(cap_status(target.error()));
    if (inv.trap.arg(1) > 1) return returned(STATUS_BAD_ARGS);
    const auto sequence = target.value()->execution(inv.trap.arg(1));
    return returned(sequence ? STATUS_OK : STATUS_BUSY, sequence.value_or(0));
}

template<usize op>
[[nodiscard]] auto sc_unbind(Call& inv) noexcept -> Result {
    auto context = inv.cspace.resolve<sched::Sc>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Attach));
    if (!context) return returned(cap_status(context.error()));
    const auto result = context.value()->withdraw(inv.trap.arg(1));
    return returned(result ? STATUS_OK : result.error() == sched::Sc::Error::Stale ? STATUS_STALE : STATUS_BUSY);
}

template<usize op>
[[nodiscard]] auto execution_stop(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<Thread>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Control));
    if (!target) return returned(cap_status(target.error()));
    target.value()->stop();
    return returned(STATUS_OK);
}
template<usize op>
[[nodiscard]] auto execution_detach(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<Thread>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Control));
    if (!target) return returned(cap_status(target.error()));
    return returned(target.value()->detach_resource(inv.trap.arg(1)));
}
template<usize op>
[[nodiscard]] auto info_call(Call& inv) noexcept -> Result {
    const auto write = [&](const auto& info) noexcept -> Result {
        auto* buffer = inv.target->ipc_buffer();
        if (!buffer || !buffer->write(inv.trap.arg(1), std::span{reinterpret_cast<const byte*>(&info), sizeof(info)}))
            return returned(STATUS_BAD_ARGS);
        return returned(STATUS_OK, sizeof(info));
    };
    if constexpr (op == SYS_THREAD_INFO)
        if (!inv.trap.arg(0)) return write(inv.target->info(inv.cpu.dispatcher().clock().rd_ticks()));
    const auto inspect = [&]<class T>() -> Result {
        auto target = inv.cspace.resolve<T>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Inspect));
        if (!target) return returned(cap_status(target.error()));
        if constexpr (std::is_same_v<T, mm::VSpace>) {
            const auto view = target->view();
            const auto* limit = std::get_if<cap::VmLimit>(&view.data);
            if (!limit || limit->range != mm::VRange{mm::va_t{mm::UserBegin}, mm::UserEnd - mm::UserBegin})
                return returned(STATUS_DENIED);
        }
        if constexpr (std::is_same_v<T, object::Scope>) {
            const auto view = target->view();
            const auto* limit = std::get_if<cap::ScopeLimit>(&view.data);
            if (!limit || !(limit->facets & SCOPE_INFO)) return returned(STATUS_DENIED);
        }
        const auto info = [&] {
            if constexpr (std::is_same_v<T, object::Scope>) {
                const auto view = target->view();
                return target.value()->info(0, std::get<cap::ScopeLimit>(view.data).kinds);
            } else return target.value()->info(inv.cpu.dispatcher().clock().rd_ticks());
        }();
        return write(info);
    };
    if constexpr (op == SYS_THREAD_INFO) return inspect.template operator()<Thread>();
    if constexpr (op == SYS_DOMAIN_INFO) return inspect.template operator()<sched::ScDomain>();
    if constexpr (op == SYS_SC_INFO) return inspect.template operator()<sched::Sc>();
    if constexpr (op == SYS_SCOPE_INFO) return inspect.template operator()<object::Scope>();
    if constexpr (op == SYS_QUOTA_INFO) return inspect.template operator()<resource::Quota>();
    if constexpr (op == SYS_VSPACE_INFO) return inspect.template operator()<mm::VSpace>();
    return returned(STATUS_INVALID_OP);
}
template<usize op>
[[nodiscard]] auto vm_records(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<mm::VSpace>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Inspect));
    if (!target) return returned(cap_status(target.error()));
    const auto view = target->view();
    const auto* limit = std::get_if<cap::VmLimit>(&view.data);
    libk_assert(limit);
    auto info = target.value()->inspect(limit->range, mm::va_t{inv.trap.arg(2)});
    if (!info) return returned(mm::status(info.error()));
    auto* buffer = inv.target->ipc_buffer();
    if (!buffer || !buffer->write(inv.trap.arg(1), {reinterpret_cast<const byte*>(&*info), sizeof(*info)}))
        return returned(STATUS_BAD_ARGS);
    return returned(STATUS_OK, sizeof(*info));
}
template<usize op>
[[nodiscard]] auto cspace_info(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<cap::CSpace>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Inspect));
    if (!target) return returned(cap_status(target.error()));
    CSpaceInfo info;
    const auto result = target.value()->inspect(handle_of(inv.trap.arg(2)), info, inv.trap.arg(3));
    if (!result) return returned(result.error() == cap::CSpaceError::InvalidHandle ? STATUS_STALE : cap_status(result.error()));
    auto* buffer = inv.target->ipc_buffer();
    if (!buffer || !buffer->write(inv.trap.arg(1), {reinterpret_cast<const byte*>(&info), sizeof(info)}))
        return returned(STATUS_BAD_ARGS);
    return returned(STATUS_OK, sizeof(info));
}
template<usize op> auto cap_info(Call& inv) noexcept -> Result {
    const auto info = inv.cspace.inspect(handle_of(inv.trap.arg(0)));
    if (!info) return returned(cap_status(info.error()));
    auto* buffer = inv.target->ipc_buffer();
    if (!buffer || !buffer->write(inv.trap.arg(1), {reinterpret_cast<const byte*>(&*info), sizeof(*info)}))
        return returned(STATUS_BAD_ARGS);
    return returned(STATUS_OK, sizeof(*info));
}
template<usize op>
[[nodiscard]] auto execution_move(Call& inv) noexcept -> Result {
    std::optional<cap::Resolved<Thread>> target;
    auto* thread = inv.target;
    if (inv.trap.arg(0)) {
        auto resolved = inv.cspace.resolve<Thread>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Control));
        if (!resolved) return returned(cap_status(resolved.error()));
        target = std::move(*resolved); thread = &target->object();
    }
    const auto result = thread->migrate(*inv.cpu.cpus, CpuId{inv.trap.arg(1)}, inv.trap.arg(2));
    return result ? returned(STATUS_OK, *result) : returned(result.error());
}

template<usize op>
[[nodiscard]] auto domain_configure(Call& inv) noexcept -> Result {
    auto domain = inv.cspace.resolve<sched::ScDomain>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Control));
    if (!domain) return returned(cap_status(domain.error()));
    if constexpr (op == SYS_DOMAIN_EXECUTION) {
        if (inv.trap.arg(1) > 1) return returned(STATUS_BAD_ARGS);
        const auto result = domain.value()->configure({}, nullptr, bool(inv.trap.arg(1)));
        return result ? returned(STATUS_OK, *result) : returned(result.error());
    }
    auto& clock = inv.cpu.dispatcher().clock();
    const auto budget = clock.ticks<time::ns>(inv.trap.arg(1));
    const auto period = clock.ticks<time::ns>(inv.trap.arg(2));
    if (!budget || !period || inv.trap.arg(3) > 65535) return returned(STATUS_BAD_ARGS);
    std::optional<cap::Resolved<sched::ScDomain>> parent;
    if (inv.trap.arg(5)) {
        auto resolved = inv.cspace.resolve<sched::ScDomain>(handle_of(inv.trap.arg(5)), cap::Rights::of(cap::Right::Control));
        if (!resolved) return returned(cap_status(resolved.error()));
        parent = std::move(*resolved);
    }
    const sched::ScDomain::Config config{.cpus=inv.trap.arg(4), .budget=*budget, .period=*period,
        .weight=static_cast<u32>(inv.trap.arg(3))};
    const auto result = domain.value()->configure(config, parent ? &parent->object() : nullptr);
    return result ? returned(STATUS_OK, *result) : returned(result.error());
}

template<usize op>
[[nodiscard]] auto domain_move(Call& inv) noexcept -> Result {
    auto domain = inv.cspace.resolve<sched::ScDomain>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Control));
    auto parent = inv.cspace.resolve<sched::ScDomain>(handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Create));
    if (!domain) return returned(cap_status(domain.error()));
    if (!parent) return returned(cap_status(parent.error()));
    const auto result = domain->object().reparent(*domain, *parent);
    return result ? returned(STATUS_OK, *result) : returned(result.error());
}

template<usize op>
[[nodiscard]] auto domain_close(Call& inv) noexcept -> Result {
    auto domain = inv.cspace.resolve<sched::ScDomain>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Close));
    if (!domain) return returned(cap_status(domain.error()));
    return returned(STATUS_OK, domain.value()->close());
}

template<usize op>
[[nodiscard]] auto sc_configure(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<sched::Sc>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Control));
    if (!target) return returned(cap_status(target.error()));
    auto& clock = inv.cpu.dispatcher().clock();
    const auto budget = clock.ticks<time::ns>(inv.trap.arg(1));
    const auto period = clock.ticks<time::ns>(inv.trap.arg(2));
    if (!budget || !period || inv.trap.arg(3) > 65535) return returned(STATUS_BAD_ARGS);
    const sched::Sc::Config config{.budget = *budget, .period = *period, .cpus = inv.trap.arg(4),
                                  .weight = static_cast<u32>(inv.trap.arg(3))};
    return returned(target.value()->configure(config, clock.rd_ticks()) ? STATUS_OK : STATUS_BUSY);
}


template<usize op>
auto yield(Call&) noexcept -> Result {
    return Result{STATUS_OK, 0, Disposition::Yield};
}

template<usize op>
auto exit(Call& inv) noexcept -> Result {
    const auto status = static_cast<status_t>(inv.trap.arg(0));
    return Result{STATUS_OK,
        static_cast<usize>(static_cast<isize>(status)), Disposition::Exit};
}

auto vm_wait(Call& inv, mm::VSpace& space) noexcept -> Result {
    // Replay preserves this thread's registers/IPC and revalidates authority.
    const auto ready = space.wait_pending(*inv.target, *inv.cpu.cpus, inv.cpu.dispatcher());
    if (ready.status == STATUS_PENDING) return {STATUS_PENDING, 0, Disposition::Retry};
    return ready.status == STATUS_OK ? Result{STATUS_RETRY, 0, Disposition::Continue} : returned(ready.status);
}

static void publish(arch::TrapCtx& ctx, const Result& result) noexcept {
    ctx.set_result(
        0, static_cast<usize>(static_cast<isize>(result.status)));
    ctx.set_result(1, result.value);
    ctx.set_result(2, result.value2);
}


auto handle(arch::TrapCtx& ctx) noexcept -> Disposition {
    Cpu& cpu = current_cpu();
    Thread* target = cpu.current;
    cap::CSpace* const cspace = target ? target->env().cspace() : nullptr;
    mm::VSpace* const vspace = target ? target->env().vspace() : nullptr;
    libk_assert(target && cspace != nullptr && vspace != nullptr);

    target->note_user_syscall();
    ctx.complete_syscall();
    Call inv{cpu, target, *cspace, *vspace,
             *cpu.idle().env().kernel_vspace(), ctx};
    const usize operation = ctx.arg(7);
    Result outcome{};
    if (const auto status = target->control_status()) outcome = returned(*status);
    else switch (operation) {
#define CALL(name, nr, entry, unit) \
    case nr: outcome = entry<nr>(inv); break;
#include <uapi/calls.def>
#undef CALL
    default:
        outcome = returned(STATUS_INVALID_OP);
        break;
    }
    if (outcome.disposition != Disposition::Continue && !target->finish_control(outcome.status))
        outcome.disposition = Disposition::Continue;
    if (outcome.disposition == Disposition::Retry || outcome.disposition == Disposition::Continue) ctx.retry_syscall();
    else publish(ctx, outcome);
    return outcome.disposition;
}

} // namespace syscall
