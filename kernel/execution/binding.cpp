#include <execution/binding.hpp>

#include <cap/cspace.hpp>
#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <mm/kernel_vspace.hpp>
#include <mm/vspace.hpp>
#include <object/cspace_pool.hpp>
#include <object/object_id.hpp>
#include <object/vspace_pool.hpp>

namespace kernel {

ExecutionBinding::UserRoots::UserRoots(
    object::ObjectRef&& vspace_owner,
    object::ObjectRef&& cspace_owner,
    kernel::mm::VSpace& address_space,
    cap::CSpace& capability_space,
    FaultRoute route) noexcept
    : vspace_ref(libk::move(vspace_owner)),
      cspace_ref(libk::move(cspace_owner)),
      vspace(&address_space),
      cspace(&capability_space),
      fault(route) {}

ExecutionBinding::UserRoots::UserRoots(UserRoots&& other) noexcept
    : vspace_ref(libk::move(other.vspace_ref)),
      cspace_ref(libk::move(other.cspace_ref)),
      vspace(libk::exchange(other.vspace, nullptr)),
      cspace(libk::exchange(other.cspace, nullptr)),
      fault(other.fault),
      ipc(libk::move(other.ipc)) {}

auto ExecutionBinding::UserRoots::operator=(UserRoots&& other) noexcept
    -> UserRoots& {
    if (this != &other) {
        reset();
        vspace_ref = libk::move(other.vspace_ref);
        cspace_ref = libk::move(other.cspace_ref);
        vspace = libk::exchange(other.vspace, nullptr);
        cspace = libk::exchange(other.cspace, nullptr);
        fault = other.fault;
        ipc = libk::move(other.ipc);
    }
    return *this;
}

ExecutionBinding::UserRoots::~UserRoots() noexcept {
    reset();
}

void ExecutionBinding::UserRoots::reset() noexcept {
    ipc.reset();
    kernel::mm::VSpace* const address_space = libk::exchange(vspace, nullptr);
    cap::CSpace* const capability_space = libk::exchange(cspace, nullptr);
    if (address_space != nullptr) {
        address_space->detach_execution();
    }
    if (capability_space != nullptr) {
        capability_space->detach_execution();
    }
    vspace_ref.reset();
    cspace_ref.reset();
}

auto ExecutionBinding::kernel(kernel::mm::KernelVSpace& vspace) noexcept
    -> ExecutionBinding {
    return ExecutionBinding{KernelRoots{&vspace}};
}

auto ExecutionBinding::user(
    object::ObjectRef&& vspace_ref,
    object::ObjectRef&& cspace_ref,
    FaultRoute route,
    libk::optional<kernel::ipc::Buffer> ipc) noexcept
    -> libk::Expected<ExecutionBinding, ExecutionError> {
    if (!vspace_ref || !cspace_ref
        || vspace_ref.kind() != object::ObjectKind::VSpace
        || cspace_ref.kind() != object::ObjectKind::CSpace) {
        return libk::unexpected(ExecutionError::InvalidRoot);
    }
    auto cspace_pin = cspace_ref.pin<cap::CSpace>();
    auto vspace_pin = vspace_ref.pin<kernel::mm::VSpace>();
    if (!cspace_pin || !vspace_pin) {
        return libk::unexpected(ExecutionError::RootUnavailable);
    }
    cap::CSpace& cspace = cspace_pin.value().get();
    kernel::mm::VSpace& vspace = vspace_pin.value().get();
    if (!cspace.attach_execution()) {
        return libk::unexpected(ExecutionError::RootUnavailable);
    }
    if (!vspace.attach_execution()) {
        cspace.detach_execution();
        return libk::unexpected(ExecutionError::RootUnavailable);
    }
    UserRoots roots{
        libk::move(vspace_ref),
        libk::move(cspace_ref),
        vspace,
        cspace,
        route};
    if (ipc) {
        if (!ipc->valid()) {
            return libk::unexpected(ExecutionError::InvalidIpcBuffer);
        }
        roots.ipc.emplace(libk::move(*ipc));
    }
    return libk::expected(ExecutionBinding{libk::move(roots)});
}

auto ExecutionBinding::kernel_bound() const noexcept -> bool {
    return libk::holds_alternative<KernelRoots>(roots_);
}

void ExecutionBinding::detach_user() noexcept {
    if (auto* const user = libk::get_if<UserRoots>(&roots_)) {
        user->reset();
        roots_.template emplace<DetachedRoots>();
        return;
    }
    KASSERT(kernel_bound() || detached());
}

auto ExecutionBinding::translation() noexcept -> kernel::mm::TranslationView {
    if (auto* kernel = libk::get_if<KernelRoots>(&roots_)) {
        KASSERT(kernel->vspace != nullptr);
        return kernel->vspace->translation();
    }
    auto* const user = libk::get_if<UserRoots>(&roots_);
    KASSERT(user != nullptr && user->vspace != nullptr);
    return user->vspace->translation();
}

auto ExecutionBinding::kernel_vspace() const noexcept -> kernel::mm::KernelVSpace* {
    const auto* const roots = libk::get_if<KernelRoots>(&roots_);
    return roots != nullptr ? roots->vspace : nullptr;
}

auto ExecutionBinding::vspace() const noexcept -> kernel::mm::VSpace* {
    const auto* const roots = libk::get_if<UserRoots>(&roots_);
    return roots != nullptr ? roots->vspace : nullptr;
}

auto ExecutionBinding::cspace() const noexcept -> cap::CSpace* {
    const auto* const roots = libk::get_if<UserRoots>(&roots_);
    return roots != nullptr ? roots->cspace : nullptr;
}

auto ExecutionBinding::fault_route() const noexcept -> FaultRoute {
    const auto* const roots = libk::get_if<UserRoots>(&roots_);
    KASSERT(roots != nullptr);
    return roots->fault;
}

auto ExecutionBinding::ipc_buffer() noexcept
    -> kernel::ipc::Buffer* {
    auto* const roots = libk::get_if<UserRoots>(&roots_);
    return roots != nullptr && roots->ipc ? &*roots->ipc : nullptr;
}

auto ExecutionBinding::ipc_buffer() const noexcept
    -> const kernel::ipc::Buffer* {
    const auto* const roots = libk::get_if<UserRoots>(&roots_);
    return roots != nullptr && roots->ipc ? &*roots->ipc : nullptr;
}

} // namespace kernel
