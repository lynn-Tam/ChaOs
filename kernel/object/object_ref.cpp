#include <object/object_ref.hpp>

namespace kernel::object {

ObjectRef::ObjectRef(ObjectRef&& other) noexcept
    : anchor_(libk::exchange(other.anchor_, nullptr)),
      generation_(libk::exchange(other.generation_, u64{})) {}

auto ObjectRef::operator=(ObjectRef&& other) noexcept -> ObjectRef& {
    if (this == &other) {
        return *this;
    }
    reset();
    anchor_ = libk::exchange(other.anchor_, nullptr);
    generation_ = libk::exchange(other.generation_, u64{});
    return *this;
}

ObjectRef::~ObjectRef() noexcept {
    reset();
}

auto ObjectRef::kind() const noexcept -> ObjectKind {
    return anchor_ != nullptr ? anchor_->kind_ : ObjectKind::Invalid;
}

auto ObjectRef::id() const noexcept -> ObjectId {
    if (anchor_ == nullptr) {
        return {};
    }
    return ObjectId{
        .slot = reinterpret_cast<usize>(anchor_),
        .generation = generation_,
        .kind = anchor_->kind_,
    };
}

auto ObjectRef::clone() const noexcept
    -> libk::Expected<ObjectRef, ObjectError> {
    if (anchor_ == nullptr) {
        return libk::unexpected(ObjectError::InvalidIdentity);
    }
    if (!anchor_->ops_->try_ref(anchor_->owner_, *anchor_, generation_)) {
        return libk::unexpected(ObjectError::InvalidLifecycle);
    }
    return libk::expected(ObjectRef{*anchor_, generation_});
}

auto ObjectRef::retire() const noexcept -> bool {
    return anchor_ != nullptr
        && anchor_->ops_->request_retire(
            anchor_->owner_, *anchor_, generation_);
}

void ObjectRef::reset() noexcept {
    ObjectAnchor* const anchor = libk::exchange(anchor_, nullptr);
    const u64 generation = libk::exchange(generation_, u64{});
    if (anchor != nullptr) {
        anchor->ops_->drop_ref(anchor->owner_, *anchor, generation);
    }
}

} // namespace kernel::object
