#include "kernel/syscall/internal.hpp"

#include <core/kernel_state.hpp>
#include <irq/irq.hpp>
#include <object/irq_pool.hpp>
#include <object/pager_pool.hpp>
#include <pager/pager.hpp>
#include <mm/memory_object.hpp>
#include <ipc/notification.hpp>
#include <uapi/syscall.h>

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
        || count > authority->max_pages) {
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
    const auto request = resolved.value()->try_claim();
    return request
        ? returned(MYOS_STATUS_OK, request.value().key.slot,
            request.value().key.generation)
        : returned(pager_error(request.error()));
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
    const kernel::pager::RequestKey key{
        .slot = static_cast<u16>(invocation.trap.arg(1)),
        .generation = invocation.trap.arg(2),
    };
    auto result = operation == MYOS_SYS_PAGER_REQUEUE
        ? resolved.value()->requeue(key)
        : operation == MYOS_SYS_PAGER_COMPLETE
            ? resolved.value()->complete(key)
            : resolved.value()->fail(key);
    return result ? returned(MYOS_STATUS_OK)
                  : returned(pager_error(result.error()));
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
    const u64 request_generation = invocation.trap.arg(4);
    const u64 claim_generation = invocation.trap.arg(5);
    if (request_generation == 0 || claim_generation == 0) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    auto transfer = source.value().object().begin_transfer(page);
    if (!transfer) {
        return returned(memory_error(transfer.error()));
    }
    auto supplied = target.value().object().pager_supply_transfer(
        libk::move(transfer).value(), page, request_generation,
        claim_generation, request_generation);
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
