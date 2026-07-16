#include <cpu/cpu_provisioner.hpp>

#include <cpu/cpu_runtime.hpp>
#include <cpu/start.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/kernel_vspace.hpp>
#include <mm/pmm.hpp>
#include <object/object_store.hpp>
#include <time/clock.hpp>

namespace kernel {

auto CpuProvisioner::prepare(
    CpuId id,
    kernel::mm::KernelVSpace& vspace,
    Thread::Entry idle_entry) noexcept -> Result {
    if (idle_entry == nullptr) {
        return libk::unexpected(Error::InvalidState);
    }

    auto reserved = registry_.reserve_runtime(id);
    if (!reserved) {
        switch (reserved.error()) {
        case CpuRegistry::RuntimeReserveError::InvalidState:
            return libk::unexpected(Error::InvalidState);
        case CpuRegistry::RuntimeReserveError::MetadataAllocation:
            return libk::unexpected(Error::MetadataAllocation);
        }
        __builtin_unreachable();
    }
    const auto target = reserved.value();
    auto* const runtime = libk::construct_at(
        static_cast<CpuRuntime*>(target.storage));

    auto init = KernelStack::create(vspace);
    if (!init) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::StackAllocation);
        return libk::unexpected(Error::StackAllocation);
    }
    [[maybe_unused]] KernelStack& init_stack =
        runtime->stacks.init.emplace(libk::move(init).value());

    auto irq = KernelStack::create(vspace);
    if (!irq) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::StackAllocation);
        return libk::unexpected(Error::StackAllocation);
    }
    [[maybe_unused]] KernelStack& irq_stack =
        runtime->stacks.irq.emplace(libk::move(irq).value());

    auto emergency = KernelStack::create(vspace);
    if (!emergency) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::StackAllocation);
        return libk::unexpected(Error::StackAllocation);
    }
    [[maybe_unused]] KernelStack& emergency_stack =
        runtime->stacks.emergency.emplace(
            libk::move(emergency).value());

    auto diagnostics_page = pmm_.allocate_page();
    if (!diagnostics_page) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::MetadataAllocation);
        return libk::unexpected(Error::MetadataAllocation);
    }
    runtime->diagnostics_page = libk::move(diagnostics_page).value();
    runtime->diagnostics = libk::construct_at(
        reinterpret_cast<diag::CpuDiagnostics*>(
            runtime->diagnostics_page.bytes()));
    runtime->diagnostics->panic.cpu = id;
    runtime->diagnostics->panic.hardware =
        target.descriptor->hardware_id();
    runtime->diagnostics->panic.registry = &registry_;

    auto home = KernelStack::create(vspace);
    if (!home) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::StackAllocation);
        return libk::unexpected(Error::StackAllocation);
    }
    auto pending_idle = objects_.create_thread(
        libk::move(home).value(),
        ExecutionBinding::kernel(vspace),
        Thread::KernelStart{idle_entry, runtime},
        Thread::Kind::Idle);
    if (!pending_idle) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::ObjectAllocation);
        return libk::unexpected(Error::ObjectAllocation);
    }
    runtime->idle_thread = libk::move(pending_idle).value().publish();

    runtime->owner_registry = &registry_;
    runtime->local.initialize(*target.descriptor, *runtime);
    arch::publish_panic_state(
        runtime->local.arch_state,
        runtime->stacks.emergency->top(),
        &runtime->diagnostics->panic);
    const kernel::mm::TranslationView translation = vspace.translation();
    runtime->initial_translation.emplace(translation);
    [[maybe_unused]] auto& dispatcher = runtime->dispatcher_storage.emplace(
        runtime->local,
        target.descriptor->logical_id(),
        runtime->idle(),
        clock_);
    runtime->start_context.initialize(
        target.descriptor->hardware_id(),
        translation.root(),
        runtime->stacks.init->top(),
        *runtime,
        kernel_secondary_continue);

    registry_.publish_runtime(target, *runtime);
    return libk::expected();
}

} // namespace kernel
