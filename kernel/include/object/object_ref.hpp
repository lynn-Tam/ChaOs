#pragma once

#include <core/debug.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/utility.hpp>
#include <object/object_anchor.hpp>
#include <object/object_traits.hpp>

namespace kernel::object {

template<typename T>
class ObjectHold;

} // namespace kernel::object

namespace kernel::resource {
class ResourcePool;
}

namespace kernel::object {

template<typename T>
class ObjectPin final : private libk::noncopyable {
    static_assert(StorableObject<T>);

public:
    ObjectPin() noexcept = default;

    ObjectPin(ObjectPin&& other) noexcept
        : anchor_(libk::exchange(other.anchor_, nullptr)),
          object_(libk::exchange(other.object_, nullptr)),
          generation_(libk::exchange(other.generation_, u64{})) {}

    auto operator=(ObjectPin&& other) noexcept -> ObjectPin& {
        if (this == &other) {
            return *this;
        }
        reset();
        anchor_ = libk::exchange(other.anchor_, nullptr);
        object_ = libk::exchange(other.object_, nullptr);
        generation_ = libk::exchange(other.generation_, u64{});
        return *this;
    }

    ~ObjectPin() noexcept { reset(); }

    [[nodiscard]] explicit operator bool() const noexcept {
        return anchor_ != nullptr;
    }

    [[nodiscard]] auto get() noexcept -> T& {
        KASSERT(object_ != nullptr);
        return *object_;
    }

    [[nodiscard]] auto get() const noexcept -> const T& {
        KASSERT(object_ != nullptr);
        return *object_;
    }

    [[nodiscard]] auto operator->() noexcept -> T* { return &get(); }
    [[nodiscard]] auto operator->() const noexcept -> const T* {
        return &get();
    }

    void reset() noexcept {
        ObjectAnchor* const anchor = libk::exchange(anchor_, nullptr);
        object_ = nullptr;
        const u64 generation = libk::exchange(generation_, u64{});
        if (anchor != nullptr) {
            anchor->ops_->drop_pin(anchor->owner_, *anchor, generation);
        }
    }

private:
    friend class ObjectRef;
    template<typename U>
    friend class ObjectPool;

    ObjectPin(ObjectAnchor& anchor, T& object, u64 generation) noexcept
        : anchor_(&anchor), object_(&object), generation_(generation) {}

    ObjectAnchor* anchor_{};
    T* object_{};
    u64 generation_{};
};

// Type-erased structural object reference. Holding it keeps the typed slot and
// payload storage stable, but object access still requires a typed operation
// pin. It can only be minted by a trusted typed pool reference.
class ObjectRef final : private libk::noncopyable {
public:
    ObjectRef() noexcept = default;
    ObjectRef(ObjectRef&& other) noexcept;
    auto operator=(ObjectRef&& other) noexcept -> ObjectRef&;
    ~ObjectRef() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return anchor_ != nullptr;
    }

    [[nodiscard]] auto kind() const noexcept -> ObjectKind;
    [[nodiscard]] auto id() const noexcept -> ObjectId;
    [[nodiscard]] auto clone() const noexcept
        -> libk::Expected<ObjectRef, ObjectError>;
    [[nodiscard]] auto retire() const noexcept -> bool;

    template<typename T>
    [[nodiscard]] auto pin() const noexcept
        -> libk::Expected<ObjectPin<T>, ObjectError> {
        static_assert(StorableObject<T>);
        if (anchor_ == nullptr) {
            return libk::unexpected(ObjectError::InvalidIdentity);
        }
        if (anchor_->kind_ != ObjectTraits<T>::kind) {
            return libk::unexpected(ObjectError::WrongKind);
        }
        void* const payload = anchor_->ops_->try_pin(
            anchor_->owner_, *anchor_, generation_);
        if (payload == nullptr) {
            return libk::unexpected(ObjectError::InvalidLifecycle);
        }
        return libk::expected(ObjectPin<T>{
            *anchor_, *static_cast<T*>(payload), generation_});
    }

    template<typename T>
    [[nodiscard]] auto into_hold() && noexcept
        -> libk::Expected<ObjectHold<T>, ObjectError>;

    void reset() noexcept;

private:
    template<typename T>
    friend class ObjectPool;
    friend class kernel::resource::ResourcePool;

    ObjectRef(ObjectAnchor& anchor, u64 generation) noexcept
        : anchor_(&anchor), generation_(generation) {}

    ObjectAnchor* anchor_{};
    u64 generation_{};
};

// Typed structural ownership of a live object. Unlike ObjectPin, a hold keeps
// the slot and payload alive without representing an operation in progress.
// The pool is the only minting authority; consumers depend only on this common
// object contract and never on the pool's page/slot implementation.
template<typename T>
class ObjectHold final : private libk::noncopyable {
public:
    ObjectHold() noexcept = default;
    ObjectHold(ObjectHold&&) noexcept = default;
    auto operator=(ObjectHold&&) noexcept -> ObjectHold& = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(ref_);
    }

    [[nodiscard]] auto get() noexcept -> T& {
        KASSERT(object_ != nullptr);
        return *object_;
    }

    [[nodiscard]] auto get() const noexcept -> const T& {
        KASSERT(object_ != nullptr);
        return *object_;
    }

    [[nodiscard]] auto operator->() noexcept -> T* { return &get(); }
    [[nodiscard]] auto operator->() const noexcept -> const T* {
        return &get();
    }

    [[nodiscard]] auto id() const noexcept -> ObjectId { return ref_.id(); }

    [[nodiscard]] auto clone() const noexcept
        -> libk::Expected<ObjectHold, ObjectError> {
        auto cloned = ref_.clone();
        if (!cloned) {
            return libk::unexpected(cloned.error());
        }
        return libk::expected(ObjectHold{
            libk::move(cloned).value(), *object_});
    }

    [[nodiscard]] auto ref() const noexcept
        -> libk::Expected<ObjectRef, ObjectError> {
        return ref_.clone();
    }

    [[nodiscard]] auto retire() const noexcept -> bool {
        return ref_.retire();
    }

    void reset() noexcept {
        object_ = nullptr;
        ref_.reset();
    }

private:
    friend class ObjectRef;
    template<typename U>
    friend class ObjectPool;

    ObjectHold(ObjectRef&& ref, T& object) noexcept
        : ref_(libk::move(ref)), object_(&object) {}

    ObjectRef ref_{};
    T* object_{};
};

template<typename T>
auto ObjectRef::into_hold() && noexcept
    -> libk::Expected<ObjectHold<T>, ObjectError> {
    static_assert(StorableObject<T>);
    if (anchor_ == nullptr) {
        return libk::unexpected(ObjectError::InvalidIdentity);
    }
    if (anchor_->kind_ != ObjectTraits<T>::kind) {
        return libk::unexpected(ObjectError::WrongKind);
    }
    void* const payload = anchor_->ops_->try_pin(
        anchor_->owner_, *anchor_, generation_);
    if (payload == nullptr) {
        return libk::unexpected(ObjectError::InvalidLifecycle);
    }
    auto* const object = static_cast<T*>(payload);
    anchor_->ops_->drop_pin(anchor_->owner_, *anchor_, generation_);
    return libk::expected(ObjectHold<T>{libk::move(*this), *object});
}

} // namespace kernel::object
