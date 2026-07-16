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

struct IpcBufferRequest final {
    kernel::object::ObjectRef memory{};
    ObjectRange object{};
    MappingKey mapping{};
    VirtRange virtual_range{};
    AccessMask access{AccessMask::of(Access::Read, Access::Write)};
};

// Stable VSpace-owned relation storage. The binding owns its lifetime while
// Mapping owns only non-owning membership.
class IpcRelation final : private libk::noncopyable_nonmovable {
public:
    explicit IpcRelation(Mapping& mapping) noexcept : mapping_(&mapping) {}
    ~IpcRelation() noexcept;

    libk::IntrusiveListHook mapping_hook_{};

private:
    friend class VSpace;
    Mapping* mapping_{};
    bool active_{};
};

class IpcBufferBinding final : private libk::noncopyable {
public:
    IpcBufferBinding() noexcept = default;
    IpcBufferBinding(IpcBufferBinding&& other) noexcept;
    auto operator=(IpcBufferBinding&& other) noexcept -> IpcBufferBinding&;
    ~IpcBufferBinding() noexcept;

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
    IpcBufferBinding(
        VSpace& owner,
        IpcRelation& relation,
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
    IpcRelation* relation_{};
    kernel::object::ObjectRef memory_{};
    ObjectRange object_{};
    MappingKey mapping_{};
    VirtRange virtual_{};
    AccessMask access_{};
};

} // namespace kernel::mm
