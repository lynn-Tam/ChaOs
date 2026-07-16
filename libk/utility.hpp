#pragma once

#include "libk/typetraits.hpp"

namespace libk {

template<typename T>
constexpr remove_ref_t<T>&& move(T&& value) noexcept {
    return static_cast<remove_ref_t<T>&&>(value);
}

template<typename T>
constexpr T&& forward(remove_ref_t<T>& value) noexcept {
    return static_cast<T&&>(value);
}

template<typename T>
constexpr T&& forward(remove_ref_t<T>&& value) noexcept {
    static_assert(!is_lvalue_reference_v<T>, "libk::forward cannot turn an rvalue into an lvalue");
    return static_cast<T&&>(value);
}

template<typename T>
[[nodiscard]] constexpr T* addressof(T& value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_addressof(value);
#else
    return reinterpret_cast<T*>(
        &const_cast<char&>(reinterpret_cast<const volatile char&>(value)));
#endif
}

template<typename T>
const T* addressof(const T&&) = delete;

template<typename T, typename U = T>
constexpr T exchange(T& object, U&& replacement)
    noexcept(is_nothrow_move_constructible_v<T>
             && is_nothrow_assignable_v<T&, U&&>) {
    T old = libk::move(object);
    object = libk::forward<U>(replacement);
    return old;
}

template<typename T>
constexpr void swap(T& lhs, T& rhs)
    noexcept(is_nothrow_move_constructible_v<T>
             && is_nothrow_move_assignable_v<T>) {
    T temporary(libk::move(lhs));
    lhs = libk::move(rhs);
    rhs = libk::move(temporary);
}

} // namespace libk
