#include <expected>
#include <cpu/setup.hpp>

#include <cpu/runtime.hpp>
#include <cpu/start.hpp>
#include <panic.hpp>
#include <libk/memory.hpp>
#include <optional>
#include <utility>
#include <mm/kspace.hpp>
#include <mm/pmm.hpp>
#include <object/pool.hpp>
#include <time/clock.hpp>

auto CpuSetup::prepare(
    CpuId id,
    mm::KSpace& vspace,
    Thread::Entry idle_entry) noexcept -> Result {
    return prepare_impl(id, vspace, nullptr, idle_entry);
}

auto CpuSetup::prepare_boot(
    CpuId id,
    mm::KSpace& vspace,
    mm::Stack& init_stack,
    Thread::Entry idle_entry) noexcept -> Result {
    if (init_stack.top() == 0) {
        return std::unexpected(Error::InvalidState);
    }
    return prepare_impl(id, vspace, &init_stack, idle_entry);
}

auto CpuSetup::prepare_impl(
    CpuId id,
    mm::KSpace& vspace,
    mm::Stack* supplied_init,
    Thread::Entry idle_entry) noexcept -> Result {
    if (idle_entry == nullptr) {
        return std::unexpected(Error::InvalidState);
    }

    auto reserved = registry_.reserve_runtime(id);
    if (!reserved) {
        switch (reserved.error()) {
        case CpuRegistry::RuntimeReserveError::InvalidState:
            return std::unexpected(Error::InvalidState);
        case CpuRegistry::RuntimeReserveError::MetadataAllocation:
            return std::unexpected(Error::MetadataAllocation);
        }
        __builtin_unreachable();
    }
    const auto target = reserved.value();
    auto* const runtime = libk::construct_at(
        static_cast<CpuRuntime*>(target.storage));

    std::optional<mm::Stack> created_init{};
    if (supplied_init == nullptr) {
        auto init = mm::Stack::create(vspace);
        if (!init) {
            libk::destroy_at(runtime);
            registry_.fail_runtime(target, CpuFailure::StackAllocation);
            return std::unexpected(Error::StackAllocation);
        }
        [[maybe_unused]] auto& owned =
            created_init.emplace(std::move(init).value());
        [[maybe_unused]] mm::Stack& init_stack =
            runtime->init_stack.emplace(std::move(*created_init));
    }

    auto irq = mm::Stack::create(vspace);
    if (!irq) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::StackAllocation);
        return std::unexpected(Error::StackAllocation);
    }
    [[maybe_unused]] mm::Stack& irq_stack =
        runtime->irq_stack.emplace(std::move(irq).value());

    auto emergency = mm::Stack::create(vspace);
    if (!emergency) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::StackAllocation);
        return std::unexpected(Error::StackAllocation);
    }
    [[maybe_unused]] mm::Stack& emergency_stack =
        runtime->emergency_stack.emplace(
            std::move(emergency).value());

    if (!runtime->init_log(pmm_, id, target.descriptor->hardware_id(), registry_)) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::MetadataAllocation);
        return std::unexpected(Error::MetadataAllocation);
    }

    auto home = mm::Stack::create(vspace);
    if (!home) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::StackAllocation);
        return std::unexpected(Error::StackAllocation);
    }
    auto pending_idle = threads_.create(
        std::move(home).value(),
        Env::kernel(vspace),
        Thread::KernelStart{idle_entry, runtime},
        Thread::Kind::Idle);
    if (!pending_idle) {
        libk::destroy_at(runtime);
        registry_.fail_runtime(target, CpuFailure::ObjectAllocation);
        return std::unexpected(Error::ObjectAllocation);
    }
    runtime->idle_thread = std::move(pending_idle).value().publish();

    runtime->owner_registry = &registry_;
    runtime->kernel = kernel_;
    runtime->local.initialize(*target.descriptor, *runtime);
    arch::publish_panic_state(
        runtime->local.arch_state,
        runtime->emergency_stack->top(),
        runtime->panic);
    const mm::Root translation = vspace.root();
    runtime->initial_translation.emplace(translation);
    [[maybe_unused]] auto& dispatcher = runtime->dispatcher_storage.emplace(
        runtime->local,
        target.descriptor->logical_id(),
        runtime->idle(),
        clock_);
    const usize init_top = supplied_init != nullptr
        ? supplied_init->top()
        : runtime->init_stack->top();
    runtime->start_context.initialize(
        target.descriptor->hardware_id(),
        translation.root(),
        init_top,
        *runtime,
        kernel_secondary_continue);

    // No fallible work may follow this transfer: a failure must never unmap
    // the stack on which the boot CPU is currently running.
    if (supplied_init != nullptr) {
        [[maybe_unused]] mm::Stack& init_stack =
            runtime->init_stack.emplace(std::move(*supplied_init));
    }

    registry_.publish_runtime(target, *runtime);
    return {};
}
