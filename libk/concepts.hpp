#pragma once

#include <libk/typetraits.hpp>

namespace libk {

template<typename T>
concept Void = is_void_v<T>;

template<typename T>
concept Reference = is_reference_v<T>;

template<typename T>
concept Function = is_function_v<T>;

template<typename T>
concept Object = is_object_v<T>;

template<typename T>
concept Pointer = is_pointer_v<T>;

template<typename T>
concept BuiltinArray = is_array_v<T>;

template<typename A, typename B>
concept SameAs = is_same_v<A, B> && is_same_v<B, A>;

template<typename T, typename... Args>
concept ConstructibleFrom = is_constructible_v<T, Args...>;

template<typename Left, typename Right>
concept AssignableFrom = is_assignable_v<Left, Right>;

template<typename From, typename To>
concept ConvertibleTo = requires(From value) {
    static_cast<To>(value);
};

template<typename T, typename U>
concept Comparable = requires(T value, U other) {
    { value < other } -> ConvertibleTo<bool>;
    { value > other } -> ConvertibleTo<bool>;
    { value <= other } -> ConvertibleTo<bool>;
    { value >= other } -> ConvertibleTo<bool>;
};

template<typename T>
concept FloatingPoint = is_floating_point_v<T>;

template<typename T>
concept Arithmetic = is_integral_v<T> || is_floating_point_v<T>;

template<typename T>
concept Integral = is_integral_v<T>;

template<typename T>
concept BitIntegral = Integral<T> && !is_same_v<remove_cv_t<T>, bool>;

template<typename T>
concept UnsignedIntegral = BitIntegral<T>
    && (static_cast<remove_cv_t<T>>(-1) > remove_cv_t<T>{0});

} // namespace libk
