#pragma once

namespace libk {
template <bool B> struct bool_constant {
    static constexpr bool value = B;
};
using true_type = bool_constant<true>;
using false_type = bool_constant<false>;

template <typename A, typename B> struct is_same : false_type {};
template <typename A> struct is_same<A, A> : true_type {};
template <typename A, typename B> inline constexpr bool is_same_v = is_same<A, B>::value;

template <typename T> struct remove_const {
    using type = T;
};
template <typename T> struct remove_const<const T> {
    using type = T;
};
template <typename T> using remove_const_t = typename remove_const<T>::type;

template <typename T> struct remove_volatile {
    using type = T;
};
template <typename T> struct remove_volatile<volatile T> {
    using type = T;
};
template <typename T> using remove_volatile_t = typename remove_volatile<T>::type;

template <typename T> struct remove_cv {
    using type = typename remove_volatile<typename remove_const<T>::type>::type;
};
template <typename T> using remove_cv_t = typename remove_cv<T>::type;

template <typename T> struct is_void : is_same<remove_cv_t<T>, void> {};

template <typename T> inline constexpr bool is_void_v = is_void<T>::value;

template <typename T> struct is_reference : false_type {};
template <typename T> struct is_reference<T&> : true_type {};
template <typename T> struct is_reference<T&&> : true_type {};

template <typename T> inline constexpr bool is_reference_v = is_reference<T>::value;

template <typename T> struct is_function {
    static constexpr bool value = false;
};

template <typename R, typename... Args> struct is_function<R(Args...)> {
    static constexpr bool value = true;
};

template <typename R, typename... Args> struct is_function<R(Args..., ...)> {
    static constexpr bool value = true;
};

template <typename R, typename... Args> struct is_function<R(Args...) noexcept> {
    static constexpr bool value = true;
};

template <typename R, typename... Args> struct is_function<R(Args..., ...) noexcept> {
    static constexpr bool value = true;
};

template <typename T> inline constexpr bool is_function_v = is_function<T>::value;

template <typename T> struct is_object {
    static constexpr bool value = !is_void_v<T> && !is_reference_v<T> && !is_function_v<T>;
};
template <typename T> inline constexpr bool is_object_v = is_object<T>::value;

template <typename T> struct remove_ref {
    using type = T;
};
template <typename T> struct remove_ref<T&> {
    using type = T;
};
template <typename T> struct remove_ref<T&&> {
    using type = T;
};
template <typename T> using remove_ref_t = remove_ref<T>::type;

template <typename T> using remove_cvr_t = remove_cv_t<remove_ref_t<T>>;

template <typename T> struct IsPointer : false_type {};
template <typename T> struct IsPointer<T*> : true_type {};
template <typename T> struct is_pointer : IsPointer<remove_cv_t<T>> {};
template <typename T> inline constexpr bool is_pointer_v = is_pointer<T>::value;

template <typename T> struct IsIntegralBase : false_type {};
template <> struct IsIntegralBase<bool> : true_type {};
template <> struct IsIntegralBase<char> : true_type {};
template <> struct IsIntegralBase<wchar_t> : true_type {};
template <> struct IsIntegralBase<char8_t> : true_type {};
template <> struct IsIntegralBase<char16_t> : true_type {};
template <> struct IsIntegralBase<char32_t> : true_type {};
template <> struct IsIntegralBase<signed char> : true_type {};
template <> struct IsIntegralBase<unsigned char> : true_type {};
template <> struct IsIntegralBase<short> : true_type {};
template <> struct IsIntegralBase<unsigned short> : true_type {};
template <> struct IsIntegralBase<int> : true_type {};
template <> struct IsIntegralBase<unsigned int> : true_type {};
template <> struct IsIntegralBase<long> : true_type {};
template <> struct IsIntegralBase<unsigned long> : true_type {};
template <> struct IsIntegralBase<long long> : true_type {};
template <> struct IsIntegralBase<unsigned long long> : true_type {};
template <typename T> struct IsIntegral : IsIntegralBase<remove_cv_t<T>> {};
template <typename T> inline constexpr bool is_integral_v = IsIntegral<T>::value;

template <typename T> struct is_const : false_type {};

template <typename T> struct is_const<const T> : true_type {};

template <typename T> inline constexpr bool is_const_v = is_const<T>::value;

template <typename T> struct is_volatile : false_type {};

template <typename T> struct is_volatile<volatile T> : true_type {};

template <typename T> inline constexpr bool is_volatile_v = is_volatile<T>::value;

template <typename T> struct remove_extent {
    using type = T;
};

template <typename T> struct remove_extent<T[]> {
    using type = T;
};

template <typename T, unsigned long N> struct remove_extent<T[N]> {
    using type = T;
};

template <typename T> using remove_extent_t = typename remove_extent<T>::type;

template <typename T> struct is_array : false_type {};

template <typename T> struct is_array<T[]> : true_type {};

template <typename T, unsigned long N> struct is_array<T[N]> : true_type {};

template <typename T> inline constexpr bool is_array_v = is_array<T>::value;
template <typename T> struct is_floating_point : false_type {};

template <> struct is_floating_point<float> : true_type {};

template <> struct is_floating_point<double> : true_type {};

template <> struct is_floating_point<long double> : true_type {};

template<typename T>
inline constexpr bool is_floating_point_v = is_floating_point<T>::value;


template <typename T>
struct type_identity {
    using type = T;
};

template <bool B, typename T, typename F>
struct conditional {
    using type = T;
};

template <typename T, typename F>
struct conditional<false, T, F> {
    using type = F;
};

template <bool B, typename T, typename F>
using conditional_t = typename conditional<B, T, F>::type;

template <bool B, typename T = void>
struct enable_if {};

template <typename T>
struct enable_if<true, T> {
    using type = T;
};

template <bool B, typename T = void>
using enable_if_t = typename enable_if<B, T>::type;

template <typename T>
struct is_lvalue_reference : false_type {};

template <typename T>
struct is_lvalue_reference<T&> : true_type {};

template <typename T>
inline constexpr bool is_lvalue_reference_v = is_lvalue_reference<T>::value;

template <typename T>
struct is_rvalue_reference : false_type {};

template <typename T>
struct is_rvalue_reference<T&&> : true_type {};

template <typename T>
inline constexpr bool is_rvalue_reference_v = is_rvalue_reference<T>::value;

template <typename T, typename... Args>
struct is_constructible : bool_constant<__is_constructible(T, Args...)> {};

template <typename T, typename... Args>
inline constexpr bool is_constructible_v = is_constructible<T, Args...>::value;

template <typename T, typename... Args>
struct is_nothrow_constructible : bool_constant<__is_nothrow_constructible(T, Args...)> {};

template <typename T, typename... Args>
inline constexpr bool is_nothrow_constructible_v = is_nothrow_constructible<T, Args...>::value;

template <typename T>
struct is_destructible {
    static constexpr bool value = requires(T* p) { p->~T(); };
};

template <typename T>
inline constexpr bool is_destructible_v = is_destructible<T>::value;

#if defined(__clang__)
#  if !__has_builtin(__is_trivially_destructible)
#    error "libk::is_trivially_destructible requires compiler builtin support"
#  endif
template <typename T>
struct is_trivially_destructible : bool_constant<__is_trivially_destructible(T)> {};
#elif defined(__GNUC__)
template <typename T>
struct is_trivially_destructible : bool_constant<__has_trivial_destructor(T)> {};
#else
#  error "libk::is_trivially_destructible requires compiler builtin support"
#endif

template <typename T>
inline constexpr bool is_trivially_destructible_v = is_trivially_destructible<T>::value;

template <typename T>
struct is_copy_constructible : is_constructible<T, const T&> {};

template <typename T>
inline constexpr bool is_copy_constructible_v = is_copy_constructible<T>::value;

template <typename T>
struct is_move_constructible : is_constructible<T, T&&> {};

template <typename T>
inline constexpr bool is_move_constructible_v = is_move_constructible<T>::value;

template <typename T>
struct is_trivially_copy_constructible : bool_constant<__is_trivially_constructible(T, const T&)> {};

template <typename T>
inline constexpr bool is_trivially_copy_constructible_v = is_trivially_copy_constructible<T>::value;

template <typename T>
struct is_trivially_move_constructible : bool_constant<__is_trivially_constructible(T, T&&)> {};

template <typename T>
inline constexpr bool is_trivially_move_constructible_v = is_trivially_move_constructible<T>::value;

template <typename L, typename R>
struct is_assignable : bool_constant<__is_assignable(L, R)> {};

template <typename L, typename R>
inline constexpr bool is_assignable_v = is_assignable<L, R>::value;

template <typename L, typename R>
struct is_nothrow_assignable : bool_constant<__is_nothrow_assignable(L, R)> {};

template <typename L, typename R>
inline constexpr bool is_nothrow_assignable_v = is_nothrow_assignable<L, R>::value;

template <typename T>
struct is_copy_assignable : is_assignable<T&, const T&> {};

template <typename T>
inline constexpr bool is_copy_assignable_v = is_copy_assignable<T>::value;

template <typename T>
struct is_move_assignable : is_assignable<T&, T&&> {};

template <typename T>
inline constexpr bool is_move_assignable_v = is_move_assignable<T>::value;

template <typename T>
struct is_trivially_copy_assignable : bool_constant<__is_trivially_assignable(T&, const T&)> {};

template <typename T>
inline constexpr bool is_trivially_copy_assignable_v = is_trivially_copy_assignable<T>::value;

template <typename T>
struct is_trivially_move_assignable : bool_constant<__is_trivially_assignable(T&, T&&)> {};

template <typename T>
inline constexpr bool is_trivially_move_assignable_v = is_trivially_move_assignable<T>::value;

#if defined(__clang__)
#  if !__has_builtin(__is_standard_layout)
#    error "libk::is_standard_layout requires compiler builtin __is_standard_layout"
#  endif
#elif !defined(__GNUC__)
#  error "libk::is_standard_layout requires compiler builtin __is_standard_layout"
#endif

template <typename T>
struct is_standard_layout : bool_constant<__is_standard_layout(T)> {};

template <typename T>
inline constexpr bool is_standard_layout_v = is_standard_layout<T>::value;



template <typename T>
struct is_enum : bool_constant<__is_enum(T)> {};

template <typename T>
inline constexpr bool is_enum_v = is_enum<T>::value;

template <typename T>
struct underlying_type {
    using type = __underlying_type(T);
};

template <typename T>
using underlying_type_t = typename underlying_type<T>::type;

template <typename T>
struct is_default_constructible : is_constructible<T> {};

template <typename T>
inline constexpr bool is_default_constructible_v = is_default_constructible<T>::value;

template <typename T>
struct is_nothrow_default_constructible : is_nothrow_constructible<T> {};

template <typename T>
inline constexpr bool is_nothrow_default_constructible_v = is_nothrow_default_constructible<T>::value;

template <typename T>
struct is_nothrow_copy_constructible : is_nothrow_constructible<T, const T&> {};

template <typename T>
inline constexpr bool is_nothrow_copy_constructible_v = is_nothrow_copy_constructible<T>::value;

template <typename T>
struct is_nothrow_move_constructible : is_nothrow_constructible<T, T&&> {};

template <typename T>
inline constexpr bool is_nothrow_move_constructible_v = is_nothrow_move_constructible<T>::value;

template <typename T>
struct is_nothrow_copy_assignable : is_nothrow_assignable<T&, const T&> {};

template <typename T>
inline constexpr bool is_nothrow_copy_assignable_v = is_nothrow_copy_assignable<T>::value;

template <typename T>
struct is_nothrow_move_assignable : is_nothrow_assignable<T&, T&&> {};

template <typename T>
inline constexpr bool is_nothrow_move_assignable_v = is_nothrow_move_assignable<T>::value;

template <typename T>
struct remove_pointer {
    using type = T;
};

template <typename T>
struct remove_pointer<T*> {
    using type = T;
};

template <typename T>
struct remove_pointer<T* const> {
    using type = T;
};

template <typename T>
struct remove_pointer<T* volatile> {
    using type = T;
};

template <typename T>
struct remove_pointer<T* const volatile> {
    using type = T;
};

template <typename T>
using remove_pointer_t = typename remove_pointer<T>::type;

template <typename T>
struct is_member_function_pointer
    : bool_constant<__is_member_function_pointer(remove_cv_t<T>)> {};

template <typename T>
inline constexpr bool is_member_function_pointer_v =
    is_member_function_pointer<T>::value;



} // namespace libk
