#pragma once

#include <type_traits>
#include <concepts>

namespace libk {

template<typename T>
concept Void = std::is_void_v<T>;

template<typename T>
concept Reference = std::is_reference_v<T>;

template<typename T>
concept Function = std::is_function_v<T>;

template<typename T>
concept Object = std::is_object_v<T>;

template<typename T>
concept Pointer = std::is_pointer_v<T>;

template<typename T>
concept BuiltinArray = std::is_array_v<T>;

template<typename T, typename... Args>
concept ConstructibleFrom = std::is_constructible_v<T, Args...>;

template<typename Left, typename Right>
concept AssignableFrom = std::is_assignable_v<Left, Right>;

template<typename T, typename U>
concept Comparable = requires(T value, U other) {
    { value < other } -> std::convertible_to<bool>;
    { value > other } -> std::convertible_to<bool>;
    { value <= other } -> std::convertible_to<bool>;
    { value >= other } -> std::convertible_to<bool>;
};

template<typename T>
concept FloatingPoint = std::is_floating_point_v<T>;

template<typename T>
concept Arithmetic = std::is_integral_v<T> || std::is_floating_point_v<T>;

template<typename T>
concept Integral = std::is_integral_v<T>;

template<typename T>
concept BitIntegral = Integral<T> && !std::is_same_v<std::remove_cv_t<T>, bool>;

template<typename T>
concept UnsignedIntegral = BitIntegral<T>
    && (static_cast<std::remove_cv_t<T>>(-1) > std::remove_cv_t<T>{0});

} // namespace libk
