#include "kernel/syscall/internal.hpp"

#include <core/kernel_state.hpp>
#include <irq/irq.hpp>
#include <object/irq_pool.hpp>
#include <object/pager_pool.hpp>
#include <pager/pager.hpp>
#include <mm/memory_object.hpp>
#include <ipc/notification.hpp>
#include <uapi/syscall.h>
#include <uapi/pager.h>

namespace kernel::syscall {
namespace {

[[nodiscard]] auto pager_error(pager::Error error) noexcept -> myos_status_t {
    switch (error) {
    case pager::Error::Closed:
        return MYOS_STATUS_CLOSED;
    case pager::Error::Full:
        return MYOS_STATUS_BUSY;
    case pager::Error::InvalidKey:
    case pager::Error::InvalidRange:
        return MYOS_STATUS_BAD_ARGS;
    case pager::Error::Busy:
        return MYOS_STATUS_WOULD_BLOCK;
    case pager::Error::Stale:
    case pager::Error::GenerationExhausted:
        return MYOS_STATUS_RETRY;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] auto irq_error(irq::Error error) noexcept -> myos_status_t {
    switch (error) {
    case irq::Error::InvalidSource:
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

[[nodiscard]] auto memory_error(mm::MemoryError error) noexcept
    -> myos_status_t {
    switch (error) {
    case mm::MemoryError::OutOfMemory:
    case mm::MemoryError::ResourceExhausted:
    case mm::MemoryError::GenerationExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case mm::MemoryError::Pressure:
        return MYOS_STATUS_RETRY;
    case mm::MemoryError::Busy:
        return MYOS_STATUS_BUSY;
    case mm::MemoryError::Pending:
        return MYOS_STATUS_WOULD_BLOCK;
    case mm::MemoryError::BackingFailed:
        return MYOS_STATUS_BACKING_FAILED;
    case mm::MemoryError::InvalidState:
    case mm::MemoryError::AttachmentState:
        return MYOS_STATUS_BUSY;
    case mm::MemoryError::InvalidSize:
    case mm::MemoryError::InvalidRange:
    case mm::MemoryError::InvalidAccess:
    case mm::MemoryError::InvalidMemoryType:
    case mm::MemoryError::NotBacked:
    case mm::MemoryError::OwnershipMismatch:
        return MYOS_STATUS_BAD_ARGS;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] auto pager_descriptor(
    const kernel::pager::Request& request) noexcept -> myos_pager_request {
    myos_pager_request descriptor{};
    descriptor.version = MYOS_PAGER_REQUEST_VERSION;
    descriptor.kind = request.kind == kernel::pager::DeliveryKind::PageIn
        ? MYOS_PAGER_REQUEST_PAGE_IN : MYOS_PAGER_REQUEST_WRITEBACK;
    descriptor.flags = MYOS_PAGER_REQUEST_FLAGS_NONE;
    descriptor.urgency = request.urgency;
    descriptor.delivery_slot = request.key.slot;
    descriptor.delivery_generation = request.key.generation;
    descriptor.claim_generation = request.claim.generation;
    descriptor.page_generation = request.page_key.generation;
    descriptor.page_index = request.page_key.index;
    if (request.kind == kernel::pager::DeliveryKind::PageIn) {
        descriptor.payload.page_in.first = request.first;
        descriptor.payload.page_in.count = request.count;
        descriptor.payload.page_in.backing_epoch = request.backing_epoch;
    } else {
        descriptor.payload.writeback.writeback_generation =
            request.writeback_generation;
        descriptor.payload.writeback.dirty_epoch = request.dirty_epoch;
    }
    return descriptor;
}

[[nodiscard]] auto read_ipc_descriptor(
    Invocation& invocation,
    usize offset) noexcept -> libk::Expected<myos_pager_request, myos_status_t> {
    auto* const buffer = invocation.target.ipc_buffer();
    if (buffer == nullptr) {
        return libk::unexpected(MYOS_STATUS_WOULD_BLOCK);
    }
    auto access = buffer->access();
    if (!access) {
        return libk::unexpected(MYOS_STATUS_RETRY);
    }
    myos_pager_request descriptor{};
    if (!access.value().read(
            offset,
            libk::Span<byte>{
                reinterpret_cast<byte*>(&descriptor), sizeof(descriptor)})) {
        return libk::unexpected(MYOS_STATUS_RETRY);
    }
    return libk::expected(descriptor);
}

auto handle_pager_request(Invocation& invocation) noexcept -> Result {
    auto resolved = invocation.cspace.resolve<kernel::pager::Pager>(
        handle_of(invocation.trap.arg(0)),
        cap::Rights::of(cap::Right::Serve));
    if (!resolved) {
        return returned(cap_status(resolved.error()));
    }
    const auto effective = resolved.value().authority();
    const auto authority = libk::get_if<cap::PagerAuthority>(
        &effective.data);
    const usize first = invocation.trap.arg(2);
    const usize count = invocation.trap.arg(3);
    if (authority == nullptr || invocation.trap.arg(1) == 0
        || invocation.trap.arg(4) == 0 || count == 0
        || count > authority->max_pages
        || invocation.trap.arg(5) > libk::numeric_limits<u8>::max()) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto request = resolved.value()->publish(
        mm::PageKey{
            .generation = invocation.trap.arg(1),
            .index = first,
        },
        first,
        count,
        invocation.trap.arg(4),
        static_cast<u8>(invocation.trap.arg(5)));
    return request
        ? returned(MYOS_STATUS_OK, request.value().key.slot,
            request.value().key.generation)
        : returned(pager_error(request.error()));
}

auto handle_pager_claim(Invocation& invocation) noexcept -> Result {
    auto resolved = invocation.cspace.resolve<kernel::pager::Pager>(
        handle_of(invocation.trap.arg(0)),
        cap::Rights::of(cap::Right::Serve));
    if (!resolved) {
        return returned(cap_status(resolved.error()));
    }
    auto* const buffer = invocation.target.ipc_buffer();
    if (buffer == nullptr) {
        return returned(MYOS_STATUS_WOULD_BLOCK);
    }
    auto access = buffer->access();
    if (!access) {
        return returned(MYOS_STATUS_RETRY);
    }
    const auto request = resolved.value()->try_claim();
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
            request.value().claim, request.value()));
        return returned(MYOS_STATUS_RETRY);
    }
    /*luna change: bind the claim to the claiming execution's index, reason:
      execution terminal must invalidate the claim through the requeue owner
      edge so worker death cannot strand the record in Claimed*/
    kernel::pager::ClaimIndex* claims{};
    if (kernel::Thread* const thread = invocation.target.thread();
        thread != nullptr) {
        claims = &thread->pager_claims();
    } else if (kernel::Vproc* const vproc = invocation.target.vproc();
               vproc != nullptr) {
        claims = &vproc->pager_claims();
    }
    if (claims != nullptr) {
        const auto registered = resolved.value()->register_claim(
            request.value().claim, *claims);
        if (!registered) {
            static_cast<void>(resolved.value()->requeue(
                request.value().claim, request.value()));
            return returned(pager_error(registered.error()));
        }
    }
    return returned(MYOS_STATUS_OK, request.value().key.slot,
        request.value().key.generation);
}

auto handle_pager_finish(
    usize operation,
    Invocation& invocation) noexcept -> Result {
    const cap::Right right = operation == MYOS_SYS_PAGER_REQUEUE
        ? cap::Right::Serve : cap::Right::Supply;
    auto resolved = invocation.cspace.resolve<kernel::pager::Pager>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(right));
    if (!resolved) {
        return returned(cap_status(resolved.error()));
    }
    auto descriptor = read_ipc_descriptor(
        invocation, invocation.trap.arg(4));
    if (!descriptor) {
        return returned(descriptor.error());
    }
    if (descriptor.value().version != MYOS_PAGER_REQUEST_VERSION
        || descriptor.value().flags != MYOS_PAGER_REQUEST_FLAGS_NONE
        || descriptor.value().delivery_slot
            > libk::numeric_limits<u16>::max()
        || descriptor.value().delivery_generation == 0
        || descriptor.value().claim_generation == 0
        || descriptor.value().page_generation == 0
        || descriptor.value().urgency > libk::numeric_limits<u8>::max()) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const kernel::pager::ClaimKey key{
        .delivery = kernel::pager::RequestKey{
            .slot = static_cast<u16>(descriptor.value().delivery_slot),
            .generation = descriptor.value().delivery_generation},
        .generation = descriptor.value().claim_generation,
    };
    if (operation == MYOS_SYS_PAGER_REQUEUE) {
        if (descriptor.value().flags != MYOS_PAGER_REQUEST_FLAGS_NONE) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        kernel::pager::Request expected{
            .key = key.delivery,
            .claim = key,
            .kind = descriptor.value().kind
                == MYOS_PAGER_REQUEST_PAGE_IN
                ? kernel::pager::DeliveryKind::PageIn
                : descriptor.value().kind == MYOS_PAGER_REQUEST_WRITEBACK
                    ? kernel::pager::DeliveryKind::Writeback
                    : kernel::pager::DeliveryKind::PageIn,
            .page_key = mm::PageKey{
                descriptor.value().page_generation,
                descriptor.value().page_index},
            .urgency = static_cast<u8>(descriptor.value().urgency),
        };
        if (descriptor.value().kind == MYOS_PAGER_REQUEST_PAGE_IN) {
            expected.first = descriptor.value().payload.page_in.first;
            expected.count = descriptor.value().payload.page_in.count;
            expected.backing_epoch =
                descriptor.value().payload.page_in.backing_epoch;
        } else if (descriptor.value().kind
            == MYOS_PAGER_REQUEST_WRITEBACK) {
            expected.first = descriptor.value().page_index;
            expected.count = 1;
            expected.backing_epoch =
                descriptor.value().payload.writeback.dirty_epoch;
            expected.writeback_generation =
                descriptor.value().payload.writeback.writeback_generation;
            expected.dirty_epoch =
                descriptor.value().payload.writeback.dirty_epoch;
        } else {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        if (expected.page_key.generation == 0 || expected.count == 0
            || (expected.kind == kernel::pager::DeliveryKind::Writeback
                && (!expected.writeback_generation
                    || !expected.dirty_epoch))) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        const auto result = resolved.value()->requeue(key, expected);
        return result ? returned(MYOS_STATUS_OK)
                      : returned(pager_error(result.error()));
    }
    auto target = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(invocation.trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    if (!target) {
        return returned(cap_status(target.error()));
    }
    const usize page = descriptor.value().page_index;
    const mm::PageKey page_key{
        descriptor.value().page_generation, page};
    libk::Expected<void, mm::MemoryError> result{
        libk::unexpected(mm::MemoryError::InvalidState)};
    if (descriptor.value().kind == MYOS_PAGER_REQUEST_PAGE_IN) {
        if (operation != MYOS_SYS_PAGER_FAIL
            || descriptor.value().flags != MYOS_PAGER_REQUEST_FLAGS_NONE) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        result = target.value().object().pager_fail(
            resolved.value().object(), page, page_key, key);
    } else if (descriptor.value().kind == MYOS_PAGER_REQUEST_WRITEBACK) {
        if (descriptor.value().payload.writeback.writeback_generation == 0
            || descriptor.value().payload.writeback.dirty_epoch == 0) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        const mm::WritebackKey writeback_key{
            page_key,
            descriptor.value().payload.writeback.writeback_generation,
            descriptor.value().payload.writeback.dirty_epoch};
        if (operation == MYOS_SYS_PAGER_COMPLETE) {
            result = target.value().object().complete_writeback(
                resolved.value().object(), page, writeback_key,
                key.delivery.generation, key.generation);
        } else {
            result = target.value().object().fail_writeback(
                resolved.value().object(), page, writeback_key,
                key.delivery.generation, key.generation,
                mm::WritebackFailure::Io);
        }
    } else {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    return result ? returned(MYOS_STATUS_OK)
                  : returned(memory_error(result.error()));
}

auto handle_pager_supply(Invocation& invocation) noexcept -> Result {
    auto pager = invocation.cspace.resolve<kernel::pager::Pager>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(cap::Right::Supply));
    auto target = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(invocation.trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto source = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(invocation.trap.arg(2)), cap::Rights::of(cap::Right::Manage));
    if (!pager || !target || !source) {
        return returned(cap_status(!pager ? pager.error()
            : !target ? target.error() : source.error()));
    }
    const usize page = invocation.trap.arg(3);
    auto descriptor = read_ipc_descriptor(
        invocation, invocation.trap.arg(4));
    if (!descriptor) {
        return returned(descriptor.error());
    }
    if (descriptor.value().version != MYOS_PAGER_REQUEST_VERSION
        || descriptor.value().kind != MYOS_PAGER_REQUEST_PAGE_IN
        || descriptor.value().flags != MYOS_PAGER_REQUEST_FLAGS_NONE
        || descriptor.value().page_generation == 0
        || descriptor.value().claim_generation == 0
        || descriptor.value().delivery_generation == 0
        || descriptor.value().delivery_slot
            > libk::numeric_limits<u16>::max()
        || descriptor.value().payload.page_in.content_epoch == 0) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    auto transfer = source.value().object().begin_transfer(page);
    if (!transfer) {
        return returned(memory_error(transfer.error()));
    }
    const usize target_page = descriptor.value().page_index;
    auto supplied = target.value().object().pager_supply_transfer(
        pager.value().object(), libk::move(transfer).value(), target_page,
        mm::PageKey{descriptor.value().page_generation, target_page},
        kernel::pager::ClaimKey{
            kernel::pager::RequestKey{
                static_cast<u16>(descriptor.value().delivery_slot),
                descriptor.value().delivery_generation},
            descriptor.value().claim_generation},
        descriptor.value().payload.page_in.content_epoch);
    return supplied ? returned(MYOS_STATUS_OK)
                    : returned(memory_error(supplied.error()));
}

auto handle_pager_bind(Invocation& invocation) noexcept -> Result {
    auto pager = invocation.cspace.resolve<kernel::pager::Pager>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(cap::Right::Serve));
    auto notification = invocation.cspace.resolve<kernel::ipc::Notification>(
        handle_of(invocation.trap.arg(1)),
        cap::Rights::of(cap::Right::Signal));
    if (!pager || !notification) {
        return returned(cap_status(!pager ? pager.error() : notification.error()));
    }
    const auto authority = notification.value().authority();
    const auto* const data = libk::get_if<cap::NotificationAuthority>(
        &authority.data);
    const u64 badge = invocation.trap.arg(2);
    if (data == nullptr || badge == 0 || badge != data->badge) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto result = pager.value()->bind(notification.value().object(), badge);
    return result ? returned(MYOS_STATUS_OK)
                  : returned(pager_error(result.error()));
}

auto handle_irq_bind(Invocation& invocation) noexcept -> Result {
    auto irq = invocation.cspace.resolve<kernel::irq::Irq>(
        handle_of(invocation.trap.arg(0)),
        cap::Rights::of(cap::Right::Route));
    auto notification = invocation.cspace.resolve<kernel::ipc::Notification>(
        handle_of(invocation.trap.arg(1)),
        cap::Rights::of(cap::Right::Signal));
    if (!irq || !notification) {
        return returned(cap_status(!irq ? irq.error() : notification.error()));
    }
    const auto authority = notification.value().authority();
    const auto* const notification_data =
        libk::get_if<cap::NotificationAuthority>(&authority.data);
    const u64 badge = invocation.trap.arg(2);
    if (notification_data == nullptr || badge == 0
        || badge != notification_data->badge) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto result = irq.value()->bind(
        notification.value().object(), badge);
    return result ? returned(MYOS_STATUS_OK)
                  : returned(irq_error(result.error()));
}

auto handle_irq(usize operation, Invocation& invocation) noexcept -> Result {
    if (operation == MYOS_SYS_IRQ_BIND) {
        return handle_irq_bind(invocation);
    }
    const cap::Right right = operation == MYOS_SYS_IRQ_ACK
        ? cap::Right::Ack : cap::Right::Route;
    auto irq = invocation.cspace.resolve<kernel::irq::Irq>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(right));
    if (!irq) {
        return returned(cap_status(irq.error()));
    }
    if (operation == MYOS_SYS_IRQ_OBSERVE) {
        const auto result = irq.value()->delivery();
        return result
            ? returned(MYOS_STATUS_OK, result.value().sequence,
                result.value().generation)
            : returned(irq_error(result.error()));
    }
    const auto result = irq.value()->ack(invocation.trap.arg(1));
    return result ? returned(MYOS_STATUS_OK)
                  : returned(irq_error(result.error()));
}

} // namespace

auto handle_pager_irq(usize operation, Invocation& invocation) noexcept -> Result {
    if (operation == MYOS_SYS_PAGER_REQUEST) {
        return handle_pager_request(invocation);
    }
    if (operation == MYOS_SYS_PAGER_CLAIM) {
        return handle_pager_claim(invocation);
    }
    if (operation == MYOS_SYS_PAGER_REQUEUE
        || operation == MYOS_SYS_PAGER_COMPLETE
        || operation == MYOS_SYS_PAGER_FAIL) {
        return handle_pager_finish(operation, invocation);
    }
    if (operation == MYOS_SYS_PAGER_SUPPLY) {
        return handle_pager_supply(invocation);
    }
    if (operation == MYOS_SYS_PAGER_BIND) {
        return handle_pager_bind(invocation);
    }
    return handle_irq(operation, invocation);
}

} // namespace kernel::syscall
