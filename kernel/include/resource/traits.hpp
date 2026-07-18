#pragma once

#include <object/object_pool.hpp>
#include <object/object_traits.hpp>
#include <resource/sponsorship.hpp>

namespace kernel::resource {

// Compile-time construction policy. Object kinds remain explicit in the UAPI;
// this trait only centralizes the fixed capacity charged for their canonical
// ObjectPool slot.
template<typename T>
struct Traits;

template<kernel::object::StorableObject T>
struct Traits<T> final {
    [[nodiscard]] static constexpr auto fixed() noexcept -> Budget {
        return kernel::object::ObjectPool<T>::slot_charge();
    }
};

template<typename T>
concept SponsoredObject = kernel::object::StorableObject<T>
    && requires {
        { Traits<T>::fixed() };
    };

} // namespace kernel::resource
