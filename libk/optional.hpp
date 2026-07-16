#pragma once

#include "libk/assert.hpp"
#include "libk/concepts.hpp"
#include "libk/memory.hpp"

namespace libk {

struct nullopt_t {
    explicit constexpr nullopt_t(int) {}
};

inline constexpr nullopt_t nullopt{0};

struct optional_in_place_t {
    explicit constexpr optional_in_place_t(int) {}
};

inline constexpr optional_in_place_t optional_in_place{0};

template<typename T>
requires(Object<T> && !is_const_v<T>)
class optional;

namespace optional_detail {

template<typename T>
T&& declval() noexcept;

template<typename F, typename... Args>
using InvokeResultT = decltype(optional_detail::declval<F>()(optional_detail::declval<Args>()...));

template<typename T>
struct IsOptional : false_type {};

template<typename T>
struct IsOptional<optional<T>> : true_type {};

template<typename T>
inline constexpr bool IsOptionalV = IsOptional<remove_cvr_t<T>>::value;

template<typename R, bool IsOptionalResult = IsOptionalV<R>>
struct CallableReturnsOptional : false_type {};

template<typename R>
struct CallableReturnsOptional<R, true>
    : bool_constant<ConstructibleFrom<remove_cvr_t<R>, R>> {};

template<typename R>
inline constexpr bool CallableReturnsOptionalV = CallableReturnsOptional<R>::value;

template<typename R, typename T>
struct OptionalResultHasValue : false_type {};

template<typename RValue, typename T>
struct OptionalResultHasValue<optional<RValue>, T> : is_same<RValue, T> {};

template<typename R, typename T>
inline constexpr bool OptionalResultHasValueV =
    OptionalResultHasValue<remove_cvr_t<R>, T>::value;

template<typename R>
inline constexpr bool CanStoreTransformValueV =
    Object<remove_cvr_t<R>>
    && !BuiltinArray<remove_cvr_t<R>>
    && !SameAs<remove_cvr_t<R>, nullopt_t>
    && !SameAs<remove_cvr_t<R>, optional_in_place_t>;

template<typename R, bool CanStore = CanStoreTransformValueV<R>>
struct CallableTransformsToOptional : false_type {};

template<typename R>
struct CallableTransformsToOptional<R, true>
    : bool_constant<ConstructibleFrom<remove_cvr_t<R>, R>> {};

template<typename R>
inline constexpr bool CallableTransformsToOptionalV =
    CallableTransformsToOptional<R>::value;

template<typename R>
struct TransformResult {
    using type = optional<remove_cvr_t<R>>;
};

template<typename R>
using TransformResultT = typename TransformResult<R>::type;

template<typename R, typename T, typename Source, bool SameValue = OptionalResultHasValueV<R, T>>
struct CallableOrElseReturnsOptional : false_type {};

template<typename R, typename T, typename Source>
struct CallableOrElseReturnsOptional<R, T, Source, true>
    : bool_constant<
        ConstructibleFrom<remove_cvr_t<R>, R>
        && ConstructibleFrom<T, Source>
    > {};

template<typename R, typename T, typename Source>
inline constexpr bool CallableOrElseReturnsOptionalV =
    CallableOrElseReturnsOptional<R, T, Source>::value;

} // namespace optional_detail

template<typename T>
requires(Object<T> && !is_const_v<T>)
class optional {
public:
    constexpr optional() noexcept = default;
    constexpr optional(nullopt_t) noexcept {}

    template<typename... Args>
    requires ConstructibleFrom<T, Args&&...>
    constexpr explicit optional(optional_in_place_t, Args&&... args)
        : storage_(optional_in_place, libk::forward<Args>(args)...),
          engaged_(true) {}

    constexpr optional(const optional& other)
    requires(ConstructibleFrom<T, const T&>) {
        if (other.engaged_) {
            emplace(*other);
        }
    }

    constexpr optional(optional&& other)
    requires(ConstructibleFrom<T, T&&>) {
        if (other.engaged_) {
            emplace(libk::move(*other));
        }
    }

    template<typename U>
    requires(!SameAs<remove_cvr_t<U>, optional<T>> && ConstructibleFrom<T, U&&>)
    constexpr optional(U&& value)
        : optional(optional_in_place, libk::forward<U>(value)) {}

    constexpr ~optional() {
        reset();
    }

    constexpr auto operator=(nullopt_t) noexcept -> optional& {
        reset();
        return *this;
    }

    constexpr auto operator=(const optional& other) -> optional&
    requires(ConstructibleFrom<T, const T&>) {
        if (this == &other) {
            return *this;
        }
        if (other.engaged_) {
            if (engaged_) {
                if constexpr(AssignableFrom<T&, const T&>) {
                    **this = *other;
                } else {
                    reset();
                    emplace(*other);
                }
            } else {
                emplace(*other);
            }
        } else {
            reset();
        }
        return *this;
    }

    constexpr auto operator=(optional&& other) -> optional&
    requires(ConstructibleFrom<T, T&&>) {
        if (this == &other) {
            return *this;
        }
        if (other.engaged_) {
            if (engaged_) {
                if constexpr(AssignableFrom<T&, T&&>) {
                    **this = libk::move(*other);
                } else {
                    reset();
                    emplace(libk::move(*other));
                }
            } else {
                emplace(libk::move(*other));
            }
        } else {
            reset();
        }
        return *this;
    }

    template<typename U>
    requires(ConstructibleFrom<T, U&&>)
    constexpr auto operator=(U&& value) -> optional& {
        if (engaged_) {
            if constexpr(AssignableFrom<T&, U&&>) {
                **this = libk::forward<U>(value);
            } else {
                reset();
                emplace(libk::forward<U>(value));
            }
        } else {
            emplace(libk::forward<U>(value));
        }
        return *this;
    }

    constexpr auto has_value() const noexcept -> bool {
        return engaged_;
    }

    constexpr explicit operator bool() const noexcept {
        return engaged_;
    }

    constexpr auto operator->() noexcept -> T* {
        libk_assert(engaged_);
        return &storage_.value_;
    }

    constexpr auto operator->() const noexcept -> const T* {
        libk_assert(engaged_);
        return &storage_.value_;
    }

    constexpr auto operator*() & noexcept -> T& {
        libk_assert(engaged_);
        return storage_.value_;
    }

    constexpr auto operator*() const & noexcept -> const T& {
        libk_assert(engaged_);
        return storage_.value_;
    }

    constexpr auto operator*() && noexcept -> T&& {
        libk_assert(engaged_);
        return libk::move(storage_.value_);
    }

    constexpr auto operator*() const && noexcept -> const T&& {
        libk_assert(engaged_);
        return libk::move(storage_.value_);
    }

    constexpr auto value() & noexcept -> T& {
        libk_assert(engaged_);
        return storage_.value_;
    }

    constexpr auto value() const & noexcept -> const T& {
        libk_assert(engaged_);
        return storage_.value_;
    }

    constexpr auto value() && noexcept -> T&& {
        libk_assert(engaged_);
        return libk::move(storage_.value_);
    }

    constexpr auto value() const && noexcept -> const T&& {
        libk_assert(engaged_);
        return libk::move(storage_.value_);
    }

    template<typename... Args>
    requires(ConstructibleFrom<T, Args&&...>)
    constexpr auto emplace(Args&&... args) -> T& {
        reset();
        libk::construct_at(&storage_.value_, libk::forward<Args>(args)...);
        engaged_ = true;
        return storage_.value_;
    }

    constexpr void reset() noexcept {
        if (engaged_) {
            libk::destroy_at(&storage_.value_);
            engaged_ = false;
        }
    }

    template<typename F>
    requires(optional_detail::CallableReturnsOptionalV<
        optional_detail::InvokeResultT<F&&, T&>
    >)
    constexpr auto and_then(F&& f) &
        -> remove_cvr_t<optional_detail::InvokeResultT<F&&, T&>> {
        using Ret = remove_cvr_t<optional_detail::InvokeResultT<F&&, T&>>;
        if (engaged_) {
            return Ret{libk::forward<F>(f)(storage_.value_)};
        }
        return Ret{nullopt};
    }

    template<typename F>
    requires(optional_detail::CallableReturnsOptionalV<
        optional_detail::InvokeResultT<F&&, const T&>
    >)
    constexpr auto and_then(F&& f) const &
        -> remove_cvr_t<optional_detail::InvokeResultT<F&&, const T&>> {
        using Ret = remove_cvr_t<optional_detail::InvokeResultT<F&&, const T&>>;
        if (engaged_) {
            return Ret{libk::forward<F>(f)(storage_.value_)};
        }
        return Ret{nullopt};
    }

    template<typename F>
    requires(optional_detail::CallableReturnsOptionalV<
        optional_detail::InvokeResultT<F&&, T&&>
    >)
    constexpr auto and_then(F&& f) &&
        -> remove_cvr_t<optional_detail::InvokeResultT<F&&, T&&>> {
        using Ret = remove_cvr_t<optional_detail::InvokeResultT<F&&, T&&>>;
        if (engaged_) {
            return Ret{libk::forward<F>(f)(libk::move(storage_.value_))};
        }
        return Ret{nullopt};
    }

    template<typename F>
    requires(optional_detail::CallableReturnsOptionalV<
        optional_detail::InvokeResultT<F&&, const T&&>
    >)
    constexpr auto and_then(F&& f) const &&
        -> remove_cvr_t<optional_detail::InvokeResultT<F&&, const T&&>> {
        using Ret = remove_cvr_t<optional_detail::InvokeResultT<F&&, const T&&>>;
        if (engaged_) {
            return Ret{libk::forward<F>(f)(libk::move(storage_.value_))};
        }
        return Ret{nullopt};
    }

    template<typename F>
    requires(optional_detail::CallableTransformsToOptionalV<
        optional_detail::InvokeResultT<F&&, T&>
    >)
    constexpr auto transform(F&& f) &
        -> optional_detail::TransformResultT<optional_detail::InvokeResultT<F&&, T&>> {
        using Raw = optional_detail::InvokeResultT<F&&, T&>;
        using Ret = optional_detail::TransformResultT<Raw>;
        if (engaged_) {
            return Ret(optional_in_place, libk::forward<F>(f)(storage_.value_));
        }
        return Ret{nullopt};
    }

    template<typename F>
    requires(optional_detail::CallableTransformsToOptionalV<
        optional_detail::InvokeResultT<F&&, const T&>
    >)
    constexpr auto transform(F&& f) const &
        -> optional_detail::TransformResultT<optional_detail::InvokeResultT<F&&, const T&>> {
        using Raw = optional_detail::InvokeResultT<F&&, const T&>;
        using Ret = optional_detail::TransformResultT<Raw>;
        if (engaged_) {
            return Ret(optional_in_place, libk::forward<F>(f)(storage_.value_));
        }
        return Ret{nullopt};
    }

    template<typename F>
    requires(optional_detail::CallableTransformsToOptionalV<
        optional_detail::InvokeResultT<F&&, T&&>
    >)
    constexpr auto transform(F&& f) &&
        -> optional_detail::TransformResultT<optional_detail::InvokeResultT<F&&, T&&>> {
        using Raw = optional_detail::InvokeResultT<F&&, T&&>;
        using Ret = optional_detail::TransformResultT<Raw>;
        if (engaged_) {
            return Ret(optional_in_place, libk::forward<F>(f)(libk::move(storage_.value_)));
        }
        return Ret{nullopt};
    }

    template<typename F>
    requires(optional_detail::CallableTransformsToOptionalV<
        optional_detail::InvokeResultT<F&&, const T&&>
    >)
    constexpr auto transform(F&& f) const &&
        -> optional_detail::TransformResultT<optional_detail::InvokeResultT<F&&, const T&&>> {
        using Raw = optional_detail::InvokeResultT<F&&, const T&&>;
        using Ret = optional_detail::TransformResultT<Raw>;
        if (engaged_) {
            return Ret(optional_in_place, libk::forward<F>(f)(libk::move(storage_.value_)));
        }
        return Ret{nullopt};
    }

    template<typename F>
    requires(optional_detail::CallableOrElseReturnsOptionalV<
        optional_detail::InvokeResultT<F&&>, T, T&
    >)
    constexpr auto or_else(F&& f) &
        -> remove_cvr_t<optional_detail::InvokeResultT<F&&>> {
        using Ret = remove_cvr_t<optional_detail::InvokeResultT<F&&>>;
        if (engaged_) {
            return Ret(optional_in_place, storage_.value_);
        }
        return Ret{libk::forward<F>(f)()};
    }

    template<typename F>
    requires(optional_detail::CallableOrElseReturnsOptionalV<
        optional_detail::InvokeResultT<F&&>, T, const T&
    >)
    constexpr auto or_else(F&& f) const &
        -> remove_cvr_t<optional_detail::InvokeResultT<F&&>> {
        using Ret = remove_cvr_t<optional_detail::InvokeResultT<F&&>>;
        if (engaged_) {
            return Ret(optional_in_place, storage_.value_);
        }
        return Ret{libk::forward<F>(f)()};
    }

    template<typename F>
    requires(optional_detail::CallableOrElseReturnsOptionalV<
        optional_detail::InvokeResultT<F&&>, T, T&&
    >)
    constexpr auto or_else(F&& f) &&
        -> remove_cvr_t<optional_detail::InvokeResultT<F&&>> {
        using Ret = remove_cvr_t<optional_detail::InvokeResultT<F&&>>;
        if (engaged_) {
            return Ret(optional_in_place, libk::move(storage_.value_));
        }
        return Ret{libk::forward<F>(f)()};
    }

    template<typename F>
    requires(optional_detail::CallableOrElseReturnsOptionalV<
        optional_detail::InvokeResultT<F&&>, T, const T&&
    >)
    constexpr auto or_else(F&& f) const &&
        -> remove_cvr_t<optional_detail::InvokeResultT<F&&>> {
        using Ret = remove_cvr_t<optional_detail::InvokeResultT<F&&>>;
        if (engaged_) {
            return Ret(optional_in_place, libk::move(storage_.value_));
        }
        return Ret{libk::forward<F>(f)()};
    }

    template<typename U>
    requires(ConstructibleFrom<T, T&> && ConstructibleFrom<T, U&&>)
    constexpr auto value_or(U&& fallback) & -> T {
        if (engaged_) {
            return T(storage_.value_);
        }
        return T(libk::forward<U>(fallback));
    }

    template<typename U>
    requires(ConstructibleFrom<T, const T&> && ConstructibleFrom<T, U&&>)
    constexpr auto value_or(U&& fallback) const & -> T {
        if (engaged_) {
            return T(storage_.value_);
        }
        return T(libk::forward<U>(fallback));
    }

    template<typename U>
    requires(ConstructibleFrom<T, T&&> && ConstructibleFrom<T, U&&>)
    constexpr auto value_or(U&& fallback) && -> T {
        if (engaged_) {
            return T(libk::move(storage_.value_));
        }
        return T(libk::forward<U>(fallback));
    }

    template<typename U>
    requires(ConstructibleFrom<T, const T&&> && ConstructibleFrom<T, U&&>)
    constexpr auto value_or(U&& fallback) const && -> T {
        if (engaged_) {
            return T(libk::move(storage_.value_));
        }
        return T(libk::forward<U>(fallback));
    }

private:
    union Storage {
        constexpr Storage() : empty_{} {}
        template<typename... Args>
        requires ConstructibleFrom<T, Args&&...>
        constexpr Storage(optional_in_place_t, Args&&... args)
            : value_(libk::forward<Args>(args)...) {}
        constexpr ~Storage() {}

        char empty_;
        T value_;
    } storage_{};

    bool engaged_{false};
};

template<typename T>
constexpr auto make_optional(T&& value) -> optional<remove_ref_t<T>> {
    return optional<remove_ref_t<T>>(optional_in_place, libk::forward<T>(value));
}

template<typename T, typename... Args>
requires(ConstructibleFrom<T, Args&&...>)
constexpr auto make_optional(Args&&... args) -> optional<T> {
    return optional<T>(optional_in_place, libk::forward<Args>(args)...);
}

} // namespace libk
