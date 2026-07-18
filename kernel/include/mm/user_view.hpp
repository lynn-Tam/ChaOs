#pragma once

#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <mm/addr.hpp>
#include <mm/object_range.hpp>
#include <mm/permissions.hpp>
#include <mm/vm_key.hpp>
#include <object/object_ref.hpp>

namespace kernel::mm {

class Mapping;
class VSpace;

struct UserViewRequest final {
    kernel::object::ObjectRef memory{};
    ObjectRange object{};
    VirtRange virtual_range{};
    AccessMask access{AccessMask::of(Access::Read, Access::Write)};
};

// Stable VSpace-owned relation storage. A UserView owns its lifetime while
// Mapping keeps only non-owning membership. This is shared by IPC buffers,
// Vproc runtime pages and future Endpoint activation views.
class UserViewRelation final : private libk::noncopyable_nonmovable {
public:
    explicit UserViewRelation(Mapping& mapping) noexcept : mapping_(&mapping) {}
    ~UserViewRelation() noexcept;

    libk::IntrusiveListHook mapping_hook_{};

private:
    friend class VSpace;
    Mapping* mapping_{};
    bool active_{};
};

class UserView final : private libk::noncopyable {
public:
    UserView() noexcept = default;
    UserView(UserView&& other) noexcept;
    auto operator=(UserView&& other) noexcept -> UserView&;
    ~UserView() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }
    [[nodiscard]] auto valid() const noexcept -> bool;
    [[nodiscard]] auto mapping() const noexcept -> MappingKey {
        return mapping_;
    }
    [[nodiscard]] auto virtual_range() const noexcept -> VirtRange {
        return virtual_;
    }
    [[nodiscard]] auto object_range() const noexcept -> ObjectRange {
        return object_;
    }
    [[nodiscard]] auto access() const noexcept -> AccessMask { return access_; }
    void reset() noexcept;

private:
    friend class VSpace;
    UserView(
        VSpace& owner,
        UserViewRelation& relation,
        kernel::object::ObjectRef&& memory,
        ObjectRange object,
        MappingKey mapping,
        VirtRange virtual_range,
        AccessMask access) noexcept
        : owner_(&owner),
          relation_(&relation),
          memory_(libk::move(memory)),
          object_(object),
          mapping_(mapping),
          virtual_(virtual_range),
          access_(access) {}

    VSpace* owner_{};
    UserViewRelation* relation_{};
    kernel::object::ObjectRef memory_{};
    ObjectRange object_{};
    MappingKey mapping_{};
    VirtRange virtual_{};
    AccessMask access_{};
};

} // namespace kernel::mm
