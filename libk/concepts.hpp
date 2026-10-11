#pragma once
#include <concepts>
#include <type_traits>

namespace libk {
template<class T>
concept unsigned_integer = std::unsigned_integral<T> && !std::same_as<std::remove_cv_t<T>, bool>;
}
