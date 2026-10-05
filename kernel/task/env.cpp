#include <expected>
#include <optional>
#include <task/env.hpp>

#include <cap/cspace.hpp>
#include <libk/assert.hpp>
#include <utility>
#include <mm/kspace.hpp>
#include <mm/vspace.hpp>
#include <object/ref.hpp>
#include <object/id.hpp>

Env::User::User(
    object::ref<>&& vspace_owner,
    object::ref<>&& cspace_owner,
    mm::VSpace& address_space,
    cap::CSpace& capability_space) noexcept
    : vspace_ref(std::move(vspace_owner)),
      cspace_ref(std::move(cspace_owner)),
      vspace(&address_space),
      cspace(&capability_space) {}

Env::User::User(User&& other) noexcept
    : vspace_ref(std::move(other.vspace_ref)),
      cspace_ref(std::move(other.cspace_ref)),
      vspace(std::exchange(other.vspace, nullptr)),
      cspace(std::exchange(other.cspace, nullptr)),
      ipc(std::move(other.ipc)) {}

auto Env::User::operator=(User&& other) noexcept
    -> User& {
    if (this != &other) {
        reset();
        vspace_ref = std::move(other.vspace_ref);
        cspace_ref = std::move(other.cspace_ref);
        vspace = std::exchange(other.vspace, nullptr);
        cspace = std::exchange(other.cspace, nullptr);
        ipc = std::move(other.ipc);
    }
    return *this;
}

Env::User::~User() noexcept {
    reset();
}

void Env::User::reset() noexcept {
    ipc.reset();
    mm::VSpace* const address_space = std::exchange(vspace, nullptr);
    cap::CSpace* const capability_space = std::exchange(cspace, nullptr);
    if (address_space != nullptr) {
        address_space->detach_execution();
    }
    if (capability_space != nullptr) {
        capability_space->detach_execution();
    }
    vspace_ref.reset();
    cspace_ref.reset();
}

auto Env::kernel(mm::KSpace& vspace) noexcept
    -> Env {
    return Env{Kernel{&vspace}};
}

auto Env::user(
    object::ref<>&& vspace_ref,
    object::ref<>&& cspace_ref,
    std::optional<ipc::Buffer> ipc) noexcept
    -> std::expected<Env, EnvError> {
    if (!vspace_ref || !cspace_ref
        || vspace_ref.kind() != object::ObjectKind::VSpace
        || cspace_ref.kind() != object::ObjectKind::CSpace) {
        return std::unexpected(EnvError::InvalidRoot);
    }
    auto cspace_pin = cspace_ref.as<cap::CSpace>();
    auto vspace_pin = vspace_ref.as<mm::VSpace>();
    if (!cspace_pin || !vspace_pin) {
        return std::unexpected(EnvError::RootUnavailable);
    }
    cap::CSpace& cspace = cspace_pin.value().get();
    mm::VSpace& vspace = vspace_pin.value().get();
    if (!cspace.attach_execution()) {
        return std::unexpected(EnvError::RootUnavailable);
    }
    if (!vspace.attach_execution()) {
        cspace.detach_execution();
        return std::unexpected(EnvError::RootUnavailable);
    }
    User roots{
        std::move(vspace_ref),
        std::move(cspace_ref),
        vspace,
        cspace};
    if (ipc) {
        if (!ipc->valid()) {
            return std::unexpected(EnvError::InvalidBuffer);
        }
        roots.ipc.emplace(std::move(*ipc));
    }
    return (Env{std::move(roots)});
}

auto Env::kernel_bound() const noexcept -> bool {
    return std::holds_alternative<Kernel>(roots_);
}

void Env::detach_user() noexcept {
    if (auto* const user = std::get_if<User>(&roots_)) {
        user->reset();
        roots_.template emplace<Detached>();
        return;
    }
    libk_assert(kernel_bound() || detached());
}

auto Env::root() noexcept -> mm::Root {
    if (auto* kernel = std::get_if<Kernel>(&roots_)) {
        libk_assert(kernel->vspace != nullptr);
        return kernel->vspace->root();
    }
    auto* const user = std::get_if<User>(&roots_);
    libk_assert(user != nullptr && user->vspace != nullptr);
    return user->vspace->root();
}

auto Env::kernel_vspace() const noexcept -> mm::KSpace* {
    const auto* const roots = std::get_if<Kernel>(&roots_);
    return roots != nullptr ? roots->vspace : nullptr;
}

auto Env::vspace() const noexcept -> mm::VSpace* {
    const auto* const roots = std::get_if<User>(&roots_);
    return roots != nullptr ? roots->vspace : nullptr;
}

auto Env::cspace() const noexcept -> cap::CSpace* {
    const auto* const roots = std::get_if<User>(&roots_);
    return roots != nullptr ? roots->cspace : nullptr;
}

auto Env::ipc_buffer() noexcept
    -> ipc::Buffer* {
    auto* const roots = std::get_if<User>(&roots_);
    return roots != nullptr && roots->ipc ? &*roots->ipc : nullptr;
}

auto Env::ipc_buffer() const noexcept
    -> const ipc::Buffer* {
    const auto* const roots = std::get_if<User>(&roots_);
    return roots != nullptr && roots->ipc ? &*roots->ipc : nullptr;
}

