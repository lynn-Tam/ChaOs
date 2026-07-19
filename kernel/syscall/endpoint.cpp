#include "kernel/syscall/internal.hpp"

#include <cpu/cpu_local.hpp>
#include <cpu/cpu_runtime.hpp>
#include <core/kernel_state.hpp>
#include <ipc/endpoint.hpp>
#include <object/endpoint_pool.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>
#include <uapi/endpoint.h>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

[[nodiscard]] auto endpoint_status(ipc::EndpointError error) noexcept
    -> myos_status_t {
    switch (error) {
    case ipc::EndpointError::Closed:
        return MYOS_STATUS_CLOSED;
    case ipc::EndpointError::Busy:
    case ipc::EndpointError::QueueFull:
        return MYOS_STATUS_WOULD_BLOCK;
    case ipc::EndpointError::InvalidConfig:
    case ipc::EndpointError::InvalidCaller:
        return MYOS_STATUS_BAD_ARGS;
    case ipc::EndpointError::DepthExceeded:
    case ipc::EndpointError::BudgetTooLow:
    case ipc::EndpointError::Denied:
        return MYOS_STATUS_DENIED;
    case ipc::EndpointError::GenerationExhausted:
        return MYOS_STATUS_INTERNAL;
    case ipc::EndpointError::TransferFailed:
        return MYOS_STATUS_BAD_ARGS;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] auto call(Invocation& invocation) noexcept -> Result {
    Thread* const thread = invocation.target.thread();
    if (thread == nullptr) {
        return returned(MYOS_STATUS_INVALID_OP);
    }
    auto endpoint = invocation.cspace.resolve<ipc::Endpoint>(
        handle_of(invocation.trap.arg(0)),
        cap::Rights::of(cap::Right::Call));
    if (!endpoint) {
        return returned(cap_status(endpoint.error()));
    }
    const usize arguments[3]{
        invocation.trap.arg(1),
        invocation.trap.arg(2),
        invocation.trap.arg(3),
    };
    auto entered = endpoint.value()->call(
        endpoint.value(),
        *thread,
        invocation.trap,
        *invocation.cpu.dispatcher(),
        invocation.cpu.runtime().kernel->cpus(),
        arguments);
    if (!entered) {
        return returned(endpoint_status(entered.error()));
    }
    switch (entered.value().disposition) {
    case ipc::CallDisposition::Entered:
        return Result{MYOS_STATUS_OK, 0, Disposition::Resume};
    case ipc::CallDisposition::Blocking:
        return Result{MYOS_STATUS_OK, 0, Disposition::Block};
    }
    __builtin_unreachable();
}

[[nodiscard]] auto reply(Invocation& invocation) noexcept -> Result {
    Thread* const thread = invocation.target.thread();
    if (thread == nullptr) {
        return returned(MYOS_STATUS_INVALID_OP);
    }
    execution::Frame* const frame = thread->active_frame();
    if (frame == nullptr
        || frame->kind() != execution::Frame::Kind::Endpoint) {
        return returned(MYOS_STATUS_INVALID_OP);
    }
    auto& activation = *static_cast<ipc::Activation*>(frame->owner());
    const isize status = static_cast<isize>(invocation.trap.arg(0));
    const usize value = invocation.trap.arg(1);
    auto replied = activation.endpoint().reply(
        *thread,
        invocation.trap,
        *invocation.cpu.dispatcher(),
        status,
        value);
    return replied
        ? Result{MYOS_STATUS_OK, 0, Disposition::Resume}
        : returned(endpoint_status(replied.error()));
}

[[nodiscard]] auto close(Invocation& invocation) noexcept -> Result {
    auto endpoint = invocation.cspace.resolve<ipc::Endpoint>(
        handle_of(invocation.trap.arg(0)),
        cap::Rights::of(cap::Right::Close));
    if (!endpoint) {
        return returned(cap_status(endpoint.error()));
    }
    endpoint.value()->close();
    return returned(MYOS_STATUS_OK);
}

[[nodiscard]] auto mint(Invocation& invocation) noexcept -> Result {
    const cap::CapHandle source = handle_of(invocation.trap.arg(0));
    auto endpoint = invocation.cspace.resolve<ipc::Endpoint>(
        source, cap::Rights::of(cap::Right::Delegate));
    const auto rights = rights_of(invocation.trap.arg(4));
    const usize cap_limit = invocation.trap.arg(3);
    if (!endpoint || !rights || !rights->contains(cap::Right::Call)
        || cap_limit > MYOS_ENDPOINT_MAX_CAPS) {
        return returned(!endpoint
            ? cap_status(endpoint.error()) : MYOS_STATUS_BAD_ARGS);
    }

    const cap::EndpointAuthority data{
        .badge = invocation.trap.arg(2),
        .fixed = ~u64{},
        .cap_limit = cap_limit,
    };
    const auto publish = [&](cap::CSpace& destination) noexcept -> Result {
        auto minted = invocation.cspace.delegate(
            source,
            destination,
            cap::GrantCeiling{*rights, data},
            cap::CapView{*rights, data});
        return returned(
            minted ? MYOS_STATUS_OK : cap_status(minted.error()),
            minted ? minted.value().raw() : 0);
    };
    const cap::CapHandle target = handle_of(invocation.trap.arg(1));
    if (!target) {
        return publish(invocation.cspace);
    }
    auto destination = invocation.cspace.resolve<cap::CSpace>(
        target, cap::Rights::of(cap::Right::Manage));
    return destination
        ? publish(destination.value().object())
        : returned(cap_status(destination.error()));
}

} // namespace

auto handle_endpoint(usize operation, Invocation& invocation) noexcept
    -> Result {
    switch (operation) {
    case MYOS_SYS_ENDPOINT_CALL:
        return call(invocation);
    case MYOS_SYS_ENDPOINT_REPLY:
        return reply(invocation);
    case MYOS_SYS_ENDPOINT_CLOSE:
        return close(invocation);
    case MYOS_SYS_ENDPOINT_MINT:
        return mint(invocation);
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
