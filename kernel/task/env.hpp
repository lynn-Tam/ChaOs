#pragma once

#include <utility>


#include <base/types.hpp>
#include <expected>
#include <libk/noncopyable.hpp>
#include <variant>
#include <optional>
#include <ipc/buffer.hpp>
#include <mm/tlb.hpp>
#include <object/ref.hpp>

namespace mm {
class KSpace;
class VSpace;
}

namespace cap {
class CSpace;
}

enum class EnvError : u8 {
    InvalidRoot,
    RootUnavailable,
    InvalidBuffer,
};

// The only owner of one Thread's effective base roots. ref<> keeps root
// storage stable; the root-local execution relation independently prevents
// retirement while this binding can be scheduled.
class Env final : private libk::noncopyable {
    struct Detached final {};

    struct Kernel final {
        mm::KSpace* vspace{};
    };

    class User final : private libk::noncopyable {
    public:
        User(User&& other) noexcept;
        auto operator=(User&& other) noexcept -> User&;
        ~User() noexcept;

        void reset() noexcept;

        object::ref<> vspace_ref{};
        object::ref<> cspace_ref{};
        mm::VSpace* vspace{};
        cap::CSpace* cspace{};
        std::optional<ipc::Buffer> ipc{};

    private:
        friend class Env;
        User(
            object::ref<>&& vspace_owner,
            object::ref<>&& cspace_owner,
            mm::VSpace& address_space,
            cap::CSpace& capability_space) noexcept;
    };

    using Roots = std::variant<Detached, Kernel, User>;

public:
    Env(Env&&) noexcept = default;
    auto operator=(Env&&) noexcept
        -> Env& = default;
    ~Env() noexcept = default;

    [[nodiscard]] static auto kernel(mm::KSpace& vspace) noexcept
        -> Env;
    [[nodiscard]] static auto user(
        object::ref<>&& vspace,
        object::ref<>&& cspace,
        std::optional<ipc::Buffer> ipc = std::nullopt) noexcept
        -> std::expected<Env, EnvError>;

    [[nodiscard]] auto kernel_bound() const noexcept -> bool;
    [[nodiscard]] auto user_bound() const noexcept -> bool {
        return std::holds_alternative<User>(roots_);
    }
    [[nodiscard]] auto detached() const noexcept -> bool {
        return std::holds_alternative<Detached>(roots_);
    }
    // Terminal user execution releases its effective roots independently of
    // Thread object lifetime. Stale diagnostic/capability references may keep
    // the Thread slot alive, but can no longer pin its VSpace or CSpace.
    void detach_user() noexcept;
    [[nodiscard]] auto root() noexcept -> mm::Root;
    [[nodiscard]] auto kernel_vspace() const noexcept -> mm::KSpace*;
    [[nodiscard]] auto vspace() const noexcept -> mm::VSpace*;
    [[nodiscard]] auto cspace() const noexcept -> cap::CSpace*;
    [[nodiscard]] auto ipc_buffer() noexcept
        -> ipc::Buffer*;
    [[nodiscard]] auto ipc_buffer() const noexcept
        -> const ipc::Buffer*;

private:
    explicit Env(Kernel roots) noexcept
        : roots_(std::in_place_type<Kernel>, roots) {}
    explicit Env(User&& roots) noexcept
        : roots_(std::in_place_type<User>, std::move(roots)) {}

    Roots roots_;
};

