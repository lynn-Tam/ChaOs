#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/variant.hpp>
#include <libk/optional.hpp>
#include <mm/ipc_buffer.hpp>
#include <mm/translation.hpp>
#include <object/object_ref.hpp>

namespace kernel::mm {
class KernelVSpace;
class VSpace;
}

namespace kernel::cap {
class CSpace;
}

namespace kernel {

enum class FaultRoute : u8 {
    Terminate,
};

enum class ExecutionError : u8 {
    InvalidRoot,
    RootUnavailable,
    InvalidIpcBuffer,
};

// The only owner of one Thread's effective base roots. ObjectRef keeps root
// storage stable; the root-local execution relation independently prevents
// retirement while this binding can be scheduled.
class ExecutionBinding final : private libk::noncopyable {
    struct KernelRoots final {
        kernel::mm::KernelVSpace* vspace{};
    };

    class UserRoots final : private libk::noncopyable {
    public:
        UserRoots(UserRoots&& other) noexcept;
        auto operator=(UserRoots&& other) noexcept -> UserRoots&;
        ~UserRoots() noexcept;

        void reset() noexcept;

        kernel::object::ObjectRef vspace_ref{};
        kernel::object::ObjectRef cspace_ref{};
        kernel::mm::VSpace* vspace{};
        kernel::cap::CSpace* cspace{};
        FaultRoute fault{FaultRoute::Terminate};
        libk::optional<kernel::mm::IpcBufferBinding> ipc{};

    private:
        friend class ExecutionBinding;
        UserRoots(
            kernel::object::ObjectRef&& vspace_owner,
            kernel::object::ObjectRef&& cspace_owner,
            kernel::mm::VSpace& address_space,
            kernel::cap::CSpace& capability_space,
            FaultRoute route) noexcept;
    };

    using Roots = libk::variant<KernelRoots, UserRoots>;

public:
    ExecutionBinding(ExecutionBinding&&) noexcept = default;
    auto operator=(ExecutionBinding&&) noexcept
        -> ExecutionBinding& = default;
    ~ExecutionBinding() noexcept = default;

    [[nodiscard]] static auto kernel(kernel::mm::KernelVSpace& vspace) noexcept
        -> ExecutionBinding;
    [[nodiscard]] static auto user(
        kernel::object::ObjectRef&& vspace,
        kernel::object::ObjectRef&& cspace,
        FaultRoute route = FaultRoute::Terminate,
        libk::optional<kernel::mm::IpcBufferRequest> ipc = libk::nullopt) noexcept
        -> libk::Expected<ExecutionBinding, ExecutionError>;

    [[nodiscard]] auto kernel_bound() const noexcept -> bool;
    [[nodiscard]] auto user_bound() const noexcept -> bool {
        return !kernel_bound();
    }
    [[nodiscard]] auto translation() noexcept -> kernel::mm::TranslationView;
    [[nodiscard]] auto kernel_vspace() const noexcept -> kernel::mm::KernelVSpace*;
    [[nodiscard]] auto vspace() const noexcept -> kernel::mm::VSpace*;
    [[nodiscard]] auto cspace() const noexcept -> kernel::cap::CSpace*;
    [[nodiscard]] auto fault_route() const noexcept -> FaultRoute;
    [[nodiscard]] auto ipc_buffer() const noexcept
        -> const kernel::mm::IpcBufferBinding*;

private:
    explicit ExecutionBinding(KernelRoots roots) noexcept
        : roots_(libk::in_place_type<KernelRoots>, roots) {}
    explicit ExecutionBinding(UserRoots&& roots) noexcept
        : roots_(libk::in_place_type<UserRoots>, libk::move(roots)) {}

    Roots roots_;
};

} // namespace kernel
