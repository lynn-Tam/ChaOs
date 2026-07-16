#pragma once

#include <libk/concepts.hpp>
#include <object/object_cleanup.hpp>
#include <object/object_id.hpp>

namespace kernel::object {

template<typename T>
struct ObjectTraits;

// Compile-time contract between a typed object payload and ObjectPool. The
// policy remains external to the payload: the concept only verifies that its
// ObjectTraits specialization supplies a stable kind and non-throwing retire
// and destruction operations.
template<typename T>
concept StorableObject = requires(T& object) {
    requires libk::SameAs<
        decltype(ObjectTraits<T>::kind), const ObjectKind>;
    { ObjectTraits<T>::destroy(object) } noexcept -> libk::SameAs<void>;
} && (requires(T& object) {
    { ObjectTraits<T>::retire(object) } noexcept -> libk::SameAs<void>;
} || requires(T& object, ObjectCleanup&& cleanup) {
    { ObjectTraits<T>::retire(object, libk::move(cleanup)) } noexcept
        -> libk::SameAs<void>;
});

} // namespace kernel::object
