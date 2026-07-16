#pragma once

#include "libk/assert.hpp"
#include "libk/concepts.hpp"
#include "libk/memory.hpp"
#include "libk/typetraits.hpp"
#include "libk/utility.hpp"

namespace libk {

template<typename T, typename E>
class Expected;

struct ExpectedVoidValue {};

namespace expected_detail {

template<typename T>
T&& declval() noexcept;

template<decltype(sizeof(0))... Is>
struct IndexSequence {};

template<decltype(sizeof(0)) N, decltype(sizeof(0))... Is>
struct MakeIndexSequenceImpl : MakeIndexSequenceImpl<N - 1, N - 1, Is...> {};

template<decltype(sizeof(0))... Is>
struct MakeIndexSequenceImpl<0, Is...> {
    using type = IndexSequence<Is...>;
};

template<decltype(sizeof(0)) N>
using MakeIndexSequence = typename MakeIndexSequenceImpl<N>::type;

template<typename T>
struct StoredArg {
    using type = remove_cvr_t<T>;
};

template<typename T>
struct StoredArg<T&> {
    using type = T&;
};

template<typename T>
struct StoredArg<const T&> {
    using type = const T&;
};

template<typename T>
using StoredArgT = typename StoredArg<T>::type;

template<typename T>
struct ForwardedArg {
    using type = T&&;
};

template<typename T>
struct ForwardedArg<T&> {
    using type = T&;
};

template<typename T>
struct ForwardedArg<const T&> {
    using type = const T&;
};

template<typename T>
using ForwardedArgT = typename ForwardedArg<T>::type;

template<decltype(sizeof(0)) I, typename T>
class ArgHolder {
public:
    template<typename U>
    explicit constexpr ArgHolder(U&& value)
        : value_(libk::forward<U>(value)) {}

    constexpr auto forward_arg() && -> decltype(auto) {
        if constexpr (Reference<T>) {
            return value_;
        } else {
            return libk::move(value_);
        }
    }

private:
    T value_;
};

template<typename IndexSeq, typename... Args>
class ArgPack;

template<decltype(sizeof(0))... Is, typename... Args>
class ArgPack<IndexSequence<Is...>, Args...> : private ArgHolder<Is, Args>... {
public:
    template<typename... Us>
    explicit constexpr ArgPack(Us&&... values)
        : ArgHolder<Is, Args>(libk::forward<Us>(values))... {}

    template<typename F>
    constexpr auto apply(F&& f) && -> decltype(auto) {
        return libk::forward<F>(f)(
            libk::move(static_cast<ArgHolder<Is, Args>&>(*this)).forward_arg()...
        );
    }
};

template<typename T>
struct IsExpected : false_type {};

template<typename V, typename E>
struct IsExpected<Expected<V, E>> : true_type {};

template<typename T>
inline constexpr bool IsExpectedV = IsExpected<remove_cvr_t<T>>::value;

template<typename T>
struct ExpectedTraits;

template<typename V, typename E>
struct ExpectedTraits<Expected<V, E>> {
    using value_type = V;
    using error_type = E;
};

template<typename T>
using ExpectedValueT = typename ExpectedTraits<remove_cvr_t<T>>::value_type;

template<typename T>
using ExpectedErrorT = typename ExpectedTraits<remove_cvr_t<T>>::error_type;

template<typename F, typename... Args>
using InvokeResultT = decltype(expected_detail::declval<F>()(expected_detail::declval<Args>()...));

template<typename R, typename E>
struct ExpectedResultHasError : false_type {};

template<typename V, typename RError, typename E>
struct ExpectedResultHasError<Expected<V, RError>, E> : is_same<RError, E> {};

template<typename R, typename E>
inline constexpr bool ExpectedResultHasErrorV =
    ExpectedResultHasError<remove_cvr_t<R>, E>::value;

template<typename R, typename V>
struct ExpectedResultHasValue : false_type {};

template<typename RValue, typename E, typename V>
struct ExpectedResultHasValue<Expected<RValue, E>, V> : is_same<RValue, V> {};

template<typename R, typename V>
inline constexpr bool ExpectedResultHasValueV =
    ExpectedResultHasValue<remove_cvr_t<R>, V>::value;

template<typename U, typename E, bool IsVoid = Void<remove_cvr_t<U>>>
struct TransformResult {
    using type = Expected<remove_cvr_t<U>, E>;
};

template<typename U, typename E>
struct TransformResult<U, E, true> {
    using type = Expected<void, E>;
};

template<typename U, typename E>
using TransformResultT = typename TransformResult<U, E>::type;

} // namespace expected_detail

template<typename T>
requires(!Reference<T> && !Void<T>)
class ExpectedValue {
public:
    template<typename U>
    requires(SameAs<T, remove_ref_t<U>>)
    explicit constexpr ExpectedValue(U&& value)
        : value_(libk::forward<U>(value)) {}

    constexpr auto take() && -> T&& {
        return libk::move(value_);
    }

private:
    T value_;
};

template<typename E>
requires(!Reference<E>)
class UnexpectedValue {
public:
    template<typename G>
    requires(SameAs<E, remove_ref_t<G>>)
    explicit constexpr UnexpectedValue(G&& error)
        : error_(libk::forward<G>(error)) {}

    constexpr auto take() && -> E&& {
        return libk::move(error_);
    }

private:
    E error_;
};

template<typename... Args>
class ExpectedInPlaceValue {
public:
    template<typename... Us>
    explicit constexpr ExpectedInPlaceValue(Us&&... args)
        : args_(libk::forward<Us>(args)...) {}

    template<typename T>
    constexpr void construct_into(T* ptr) && {
        libk::move(args_).apply([ptr](auto&&... args) {
            libk::construct_at(ptr, libk::forward<decltype(args)>(args)...);
        });
    }

private:
    expected_detail::ArgPack<
        expected_detail::MakeIndexSequence<sizeof...(Args)>,
        Args...
    > args_;
};

template<typename... Args>
class UnexpectedInPlaceValue {
public:
    template<typename... Us>
    explicit constexpr UnexpectedInPlaceValue(Us&&... args)
        : args_(libk::forward<Us>(args)...) {}

    template<typename E>
    constexpr void construct_into(E* ptr) && {
        libk::move(args_).apply([ptr](auto&&... args) {
            libk::construct_at(ptr, libk::forward<decltype(args)>(args)...);
        });
    }

private:
    expected_detail::ArgPack<
        expected_detail::MakeIndexSequence<sizeof...(Args)>,
        Args...
    > args_;
};

inline constexpr auto expected() -> ExpectedVoidValue {
    return ExpectedVoidValue{};
}

template<typename T>
requires(!Void<remove_ref_t<T>>)
inline constexpr auto expected(T&& value) -> ExpectedValue<remove_ref_t<T>> {
    return ExpectedValue<remove_ref_t<T>>(libk::forward<T>(value));
}

template<typename E>
inline constexpr auto unexpected(E&& error) -> UnexpectedValue<remove_ref_t<E>> {
    return UnexpectedValue<remove_ref_t<E>>(libk::forward<E>(error));
}

template<typename... Args>
inline constexpr auto expected_in_place(Args&&... args)
    -> ExpectedInPlaceValue<expected_detail::StoredArgT<Args&&>...> {
    return ExpectedInPlaceValue<expected_detail::StoredArgT<Args&&>...>(
        libk::forward<Args>(args)...
    );
}

template<typename... Args>
inline constexpr auto unexpected_in_place(Args&&... args)
    -> UnexpectedInPlaceValue<expected_detail::StoredArgT<Args&&>...> {
    return UnexpectedInPlaceValue<expected_detail::StoredArgT<Args&&>...>(
        libk::forward<Args>(args)...
    );
}

template<typename T, typename E>
class Expected {
    static_assert(!Void<T>, "Expected<void, E> is implemented by specialization");
    static_assert(!Reference<T>, "Expected<T, E> cannot store a reference value");
    static_assert(!Reference<E>, "Expected<T, E> cannot store a reference error");

public:
    using value_type = T;
    using error_type = E;

    template<typename U>
    requires(ConstructibleFrom<T, U&&>)
    constexpr Expected(ExpectedValue<U>&& value)
        : Expected(SuccessTag{}, libk::move(value).take()) {}

    template<typename G>
    requires(ConstructibleFrom<E, G&&>)
    constexpr Expected(UnexpectedValue<G>&& error)
        : Expected(ErrorTag{}, libk::move(error).take()) {}

    template<typename... Args>
    requires(ConstructibleFrom<T, expected_detail::ForwardedArgT<Args>...>)
    constexpr Expected(ExpectedInPlaceValue<Args...>&& value) {
        libk::move(value).template construct_into<T>(&storage_.value_);
        has_value_ = true;
    }

    template<typename... Args>
    requires(ConstructibleFrom<E, expected_detail::ForwardedArgT<Args>...>)
    constexpr Expected(UnexpectedInPlaceValue<Args...>&& error) {
        libk::move(error).template construct_into<E>(&storage_.error_);
        has_value_ = false;
    }

    constexpr Expected(const Expected& other)
    requires(ConstructibleFrom<T, const T&> && ConstructibleFrom<E, const E&>) {
        copy_construct_from(other);
    }

    constexpr auto operator=(const Expected& other) -> Expected&
    requires(ConstructibleFrom<T, const T&> && ConstructibleFrom<E, const E&>) {
        if (this == &other) {
            return *this;
        }
        destroy_active();
        copy_construct_from(other);
        return *this;
    }

    constexpr Expected(Expected&& other)
    requires(ConstructibleFrom<T, T&&> && ConstructibleFrom<E, E&&>) {
        move_construct_from(libk::move(other));
    }

    constexpr auto operator=(Expected&& other) -> Expected&
    requires(ConstructibleFrom<T, T&&> && ConstructibleFrom<E, E&&>) {
        if (this == &other) {
            return *this;
        }
        destroy_active();
        move_construct_from(libk::move(other));
        return *this;
    }

    template<typename... Args>
    requires(ConstructibleFrom<T, Args&&...>)
    static constexpr auto success(Args&&... args) -> Expected {
        return Expected(SuccessTag{}, libk::forward<Args>(args)...);
    }

    template<typename... Args>
    requires(ConstructibleFrom<E, Args&&...>)
    static constexpr auto failure(Args&&... args) -> Expected {
        return Expected(ErrorTag{}, libk::forward<Args>(args)...);
    }

    [[nodiscard]] constexpr auto has_value() const noexcept -> bool {
        return has_value_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return has_value();
    }

    constexpr auto value() & noexcept -> T& {
        libk_assert(has_value_);
        return storage_.value_;
    }

    constexpr auto value() const & noexcept -> const T& {
        libk_assert(has_value_);
        return storage_.value_;
    }

    constexpr auto value() && noexcept -> T&& {
        libk_assert(has_value_);
        return libk::move(storage_.value_);
    }

    constexpr auto value() const && noexcept -> const T&& {
        libk_assert(has_value_);
        return libk::move(storage_.value_);
    }

    constexpr auto error() & noexcept -> E& {
        libk_assert(!has_value_);
        return storage_.error_;
    }

    constexpr auto error() const & noexcept -> const E& {
        libk_assert(!has_value_);
        return storage_.error_;
    }

    constexpr auto error() && noexcept -> E&& {
        libk_assert(!has_value_);
        return libk::move(storage_.error_);
    }

    constexpr auto error() const && noexcept -> const E&& {
        libk_assert(!has_value_);
        return libk::move(storage_.error_);
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasErrorV<
            expected_detail::InvokeResultT<F&&, T&>, E
        > && ConstructibleFrom<E, E&>
    )
    constexpr auto and_then(F&& f) &
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, T&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, T&>>;
        if (has_value_) {
            return libk::forward<F>(f)(storage_.value_);
        }
        return Ret::failure(storage_.error_);
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasErrorV<
            expected_detail::InvokeResultT<F&&, const T&>, E
        > && ConstructibleFrom<E, const E&>
    )
    constexpr auto and_then(F&& f) const &
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, const T&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, const T&>>;
        if (has_value_) {
            return libk::forward<F>(f)(storage_.value_);
        }
        return Ret::failure(storage_.error_);
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasErrorV<
            expected_detail::InvokeResultT<F&&, T&&>, E
        > && ConstructibleFrom<E, E&&>
    )
    constexpr auto and_then(F&& f) &&
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, T&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, T&&>>;
        if (has_value_) {
            return libk::forward<F>(f)(libk::move(storage_.value_));
        }
        return Ret::failure(libk::move(storage_.error_));
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasErrorV<
            expected_detail::InvokeResultT<F&&, const T&&>, E
        > && ConstructibleFrom<E, const E&&>
    )
    constexpr auto and_then(F&& f) const &&
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, const T&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, const T&&>>;
        if (has_value_) {
            return libk::forward<F>(f)(libk::move(storage_.value_));
        }
        return Ret::failure(libk::move(storage_.error_));
    }

    template<typename F>
    requires(ConstructibleFrom<E, E&>)
    constexpr auto transform(F&& f) &
        -> expected_detail::TransformResultT<expected_detail::InvokeResultT<F&&, T&>, E> {
        using Raw = expected_detail::InvokeResultT<F&&, T&>;
        using Ret = expected_detail::TransformResultT<Raw, E>;
        if (has_value_) {
            if constexpr (Void<remove_cvr_t<Raw>>) {
                libk::forward<F>(f)(storage_.value_);
                return Ret::success();
            } else {
                return Ret::success(libk::forward<F>(f)(storage_.value_));
            }
        }
        return Ret::failure(storage_.error_);
    }

    template<typename F>
    requires(ConstructibleFrom<E, const E&>)
    constexpr auto transform(F&& f) const &
        -> expected_detail::TransformResultT<expected_detail::InvokeResultT<F&&, const T&>, E> {
        using Raw = expected_detail::InvokeResultT<F&&, const T&>;
        using Ret = expected_detail::TransformResultT<Raw, E>;
        if (has_value_) {
            if constexpr (Void<remove_cvr_t<Raw>>) {
                libk::forward<F>(f)(storage_.value_);
                return Ret::success();
            } else {
                return Ret::success(libk::forward<F>(f)(storage_.value_));
            }
        }
        return Ret::failure(storage_.error_);
    }

    template<typename F>
    requires(ConstructibleFrom<E, E&&>)
    constexpr auto transform(F&& f) &&
        -> expected_detail::TransformResultT<expected_detail::InvokeResultT<F&&, T&&>, E> {
        using Raw = expected_detail::InvokeResultT<F&&, T&&>;
        using Ret = expected_detail::TransformResultT<Raw, E>;
        if (has_value_) {
            if constexpr (Void<remove_cvr_t<Raw>>) {
                libk::forward<F>(f)(libk::move(storage_.value_));
                return Ret::success();
            } else {
                return Ret::success(libk::forward<F>(f)(libk::move(storage_.value_)));
            }
        }
        return Ret::failure(libk::move(storage_.error_));
    }

    template<typename F>
    requires(ConstructibleFrom<E, const E&&>)
    constexpr auto transform(F&& f) const &&
        -> expected_detail::TransformResultT<expected_detail::InvokeResultT<F&&, const T&&>, E> {
        using Raw = expected_detail::InvokeResultT<F&&, const T&&>;
        using Ret = expected_detail::TransformResultT<Raw, E>;
        if (has_value_) {
            if constexpr (Void<remove_cvr_t<Raw>>) {
                libk::forward<F>(f)(libk::move(storage_.value_));
                return Ret::success();
            } else {
                return Ret::success(libk::forward<F>(f)(libk::move(storage_.value_)));
            }
        }
        return Ret::failure(libk::move(storage_.error_));
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasValueV<
            expected_detail::InvokeResultT<F&&, E&>, T
        > && ConstructibleFrom<T, T&>
    )
    constexpr auto or_else(F&& f) &
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, E&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, E&>>;
        if (has_value_) {
            return Ret::success(storage_.value_);
        }
        return libk::forward<F>(f)(storage_.error_);
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasValueV<
            expected_detail::InvokeResultT<F&&, const E&>, T
        > && ConstructibleFrom<T, const T&>
    )
    constexpr auto or_else(F&& f) const &
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, const E&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, const E&>>;
        if (has_value_) {
            return Ret::success(storage_.value_);
        }
        return libk::forward<F>(f)(storage_.error_);
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasValueV<
            expected_detail::InvokeResultT<F&&, E&&>, T
        > && ConstructibleFrom<T, T&&>
    )
    constexpr auto or_else(F&& f) &&
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, E&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, E&&>>;
        if (has_value_) {
            return Ret::success(libk::move(storage_.value_));
        }
        return libk::forward<F>(f)(libk::move(storage_.error_));
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasValueV<
            expected_detail::InvokeResultT<F&&, const E&&>, T
        > && ConstructibleFrom<T, const T&&>
    )
    constexpr auto or_else(F&& f) const &&
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, const E&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, const E&&>>;
        if (has_value_) {
            return Ret::success(libk::move(storage_.value_));
        }
        return libk::forward<F>(f)(libk::move(storage_.error_));
    }

    template<typename U>
    requires(ConstructibleFrom<T, T&> && ConstructibleFrom<T, U&&>)
    constexpr auto value_or(U&& fallback) & -> T {
        if (has_value_) {
            return T(storage_.value_);
        }
        return T(libk::forward<U>(fallback));
    }

    template<typename U>
    requires(ConstructibleFrom<T, const T&> && ConstructibleFrom<T, U&&>)
    constexpr auto value_or(U&& fallback) const & -> T {
        if (has_value_) {
            return T(storage_.value_);
        }
        return T(libk::forward<U>(fallback));
    }

    template<typename U>
    requires(ConstructibleFrom<T, T&&> && ConstructibleFrom<T, U&&>)
    constexpr auto value_or(U&& fallback) && -> T {
        if (has_value_) {
            return T(libk::move(storage_.value_));
        }
        return T(libk::forward<U>(fallback));
    }

    template<typename U>
    requires(ConstructibleFrom<T, const T&&> && ConstructibleFrom<T, U&&>)
    constexpr auto value_or(U&& fallback) const && -> T {
        if (has_value_) {
            return T(libk::move(storage_.value_));
        }
        return T(libk::forward<U>(fallback));
    }

    constexpr ~Expected() {
        destroy_active();
    }

private:
    struct SuccessTag {};
    struct ErrorTag {};

    union Storage {
        constexpr Storage() : empty_{} {}
        constexpr ~Storage() {}

        char empty_;
        T value_;
        E error_;
    } storage_{};

    bool has_value_{false};

    template<typename... Args>
    requires(ConstructibleFrom<T, Args&&...>)
    constexpr Expected(SuccessTag, Args&&... args) {
        libk::construct_at(&storage_.value_, libk::forward<Args>(args)...);
        has_value_ = true;
    }

    template<typename... Args>
    requires(ConstructibleFrom<E, Args&&...>)
    constexpr Expected(ErrorTag, Args&&... args) {
        libk::construct_at(&storage_.error_, libk::forward<Args>(args)...);
        has_value_ = false;
    }

    constexpr void destroy_active() noexcept {
        if (has_value_) {
            libk::destroy_at(&storage_.value_);
        } else {
            libk::destroy_at(&storage_.error_);
        }
    }

    constexpr void move_construct_from(Expected&& other) {
        if (other.has_value_) {
            libk::construct_at(&storage_.value_, libk::move(other.storage_.value_));
            has_value_ = true;
        } else {
            libk::construct_at(&storage_.error_, libk::move(other.storage_.error_));
            has_value_ = false;
        }
    }

    constexpr void copy_construct_from(const Expected& other) {
        if (other.has_value_) {
            libk::construct_at(&storage_.value_, other.storage_.value_);
            has_value_ = true;
        } else {
            libk::construct_at(&storage_.error_, other.storage_.error_);
            has_value_ = false;
        }
    }
};

template<typename E>
class Expected<void, E> {
    static_assert(!Reference<E>, "Expected<void, E> cannot store a reference error");

public:
    using value_type = void;
    using error_type = E;

    constexpr Expected(ExpectedVoidValue)
        : Expected(SuccessTag{}) {}

    template<typename G>
    requires(ConstructibleFrom<E, G&&>)
    constexpr Expected(UnexpectedValue<G>&& error)
        : Expected(ErrorTag{}, libk::move(error).take()) {}

    template<typename... Args>
    requires(ConstructibleFrom<E, expected_detail::ForwardedArgT<Args>...>)
    constexpr Expected(UnexpectedInPlaceValue<Args...>&& error) {
        libk::move(error).template construct_into<E>(&storage_.error_);
        has_value_ = false;
    }

    constexpr Expected(const Expected& other)
    requires(ConstructibleFrom<E, const E&>) {
        copy_construct_from(other);
    }

    constexpr auto operator=(const Expected& other) -> Expected&
    requires(ConstructibleFrom<E, const E&>) {
        if (this == &other) {
            return *this;
        }
        destroy_active();
        copy_construct_from(other);
        return *this;
    }

    constexpr Expected(Expected&& other)
    requires(ConstructibleFrom<E, E&&>) {
        move_construct_from(libk::move(other));
    }

    constexpr auto operator=(Expected&& other) -> Expected&
    requires(ConstructibleFrom<E, E&&>) {
        if (this == &other) {
            return *this;
        }
        destroy_active();
        move_construct_from(libk::move(other));
        return *this;
    }

    static constexpr auto success() -> Expected {
        return Expected(SuccessTag{});
    }

    template<typename... Args>
    requires(ConstructibleFrom<E, Args&&...>)
    static constexpr auto failure(Args&&... args) -> Expected {
        return Expected(ErrorTag{}, libk::forward<Args>(args)...);
    }

    [[nodiscard]] constexpr auto has_value() const noexcept -> bool {
        return has_value_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return has_value();
    }

    constexpr void value() const noexcept {
        libk_assert(has_value_);
    }

    constexpr auto error() & noexcept -> E& {
        libk_assert(!has_value_);
        return storage_.error_;
    }

    constexpr auto error() const & noexcept -> const E& {
        libk_assert(!has_value_);
        return storage_.error_;
    }

    constexpr auto error() && noexcept -> E&& {
        libk_assert(!has_value_);
        return libk::move(storage_.error_);
    }

    constexpr auto error() const && noexcept -> const E&& {
        libk_assert(!has_value_);
        return libk::move(storage_.error_);
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasErrorV<
            expected_detail::InvokeResultT<F&&>, E
        > && ConstructibleFrom<E, E&>
    )
    constexpr auto and_then(F&& f) &
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&>>;
        if (has_value_) {
            return libk::forward<F>(f)();
        }
        return Ret::failure(storage_.error_);
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasErrorV<
            expected_detail::InvokeResultT<F&&>, E
        > && ConstructibleFrom<E, const E&>
    )
    constexpr auto and_then(F&& f) const &
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&>>;
        if (has_value_) {
            return libk::forward<F>(f)();
        }
        return Ret::failure(storage_.error_);
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasErrorV<
            expected_detail::InvokeResultT<F&&>, E
        > && ConstructibleFrom<E, E&&>
    )
    constexpr auto and_then(F&& f) &&
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&>>;
        if (has_value_) {
            return libk::forward<F>(f)();
        }
        return Ret::failure(libk::move(storage_.error_));
    }

    template<typename F>
    requires(
        expected_detail::ExpectedResultHasErrorV<
            expected_detail::InvokeResultT<F&&>, E
        > && ConstructibleFrom<E, const E&&>
    )
    constexpr auto and_then(F&& f) const &&
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&>>;
        if (has_value_) {
            return libk::forward<F>(f)();
        }
        return Ret::failure(libk::move(storage_.error_));
    }

    template<typename F>
    requires(ConstructibleFrom<E, E&>)
    constexpr auto transform(F&& f) &
        -> expected_detail::TransformResultT<expected_detail::InvokeResultT<F&&>, E> {
        using Raw = expected_detail::InvokeResultT<F&&>;
        using Ret = expected_detail::TransformResultT<Raw, E>;
        if (has_value_) {
            if constexpr (Void<remove_cvr_t<Raw>>) {
                libk::forward<F>(f)();
                return Ret::success();
            } else {
                return Ret::success(libk::forward<F>(f)());
            }
        }
        return Ret::failure(storage_.error_);
    }

    template<typename F>
    requires(ConstructibleFrom<E, const E&>)
    constexpr auto transform(F&& f) const &
        -> expected_detail::TransformResultT<expected_detail::InvokeResultT<F&&>, E> {
        using Raw = expected_detail::InvokeResultT<F&&>;
        using Ret = expected_detail::TransformResultT<Raw, E>;
        if (has_value_) {
            if constexpr (Void<remove_cvr_t<Raw>>) {
                libk::forward<F>(f)();
                return Ret::success();
            } else {
                return Ret::success(libk::forward<F>(f)());
            }
        }
        return Ret::failure(storage_.error_);
    }

    template<typename F>
    requires(ConstructibleFrom<E, E&&>)
    constexpr auto transform(F&& f) &&
        -> expected_detail::TransformResultT<expected_detail::InvokeResultT<F&&>, E> {
        using Raw = expected_detail::InvokeResultT<F&&>;
        using Ret = expected_detail::TransformResultT<Raw, E>;
        if (has_value_) {
            if constexpr (Void<remove_cvr_t<Raw>>) {
                libk::forward<F>(f)();
                return Ret::success();
            } else {
                return Ret::success(libk::forward<F>(f)());
            }
        }
        return Ret::failure(libk::move(storage_.error_));
    }

    template<typename F>
    requires(ConstructibleFrom<E, const E&&>)
    constexpr auto transform(F&& f) const &&
        -> expected_detail::TransformResultT<expected_detail::InvokeResultT<F&&>, E> {
        using Raw = expected_detail::InvokeResultT<F&&>;
        using Ret = expected_detail::TransformResultT<Raw, E>;
        if (has_value_) {
            if constexpr (Void<remove_cvr_t<Raw>>) {
                libk::forward<F>(f)();
                return Ret::success();
            } else {
                return Ret::success(libk::forward<F>(f)());
            }
        }
        return Ret::failure(libk::move(storage_.error_));
    }

    template<typename F>
    requires(expected_detail::ExpectedResultHasValueV<expected_detail::InvokeResultT<F&&, E&>, void>)
    constexpr auto or_else(F&& f) &
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, E&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, E&>>;
        if (has_value_) {
            return Ret::success();
        }
        return libk::forward<F>(f)(storage_.error_);
    }

    template<typename F>
    requires(expected_detail::ExpectedResultHasValueV<expected_detail::InvokeResultT<F&&, const E&>, void>)
    constexpr auto or_else(F&& f) const &
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, const E&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, const E&>>;
        if (has_value_) {
            return Ret::success();
        }
        return libk::forward<F>(f)(storage_.error_);
    }

    template<typename F>
    requires(expected_detail::ExpectedResultHasValueV<expected_detail::InvokeResultT<F&&, E&&>, void>)
    constexpr auto or_else(F&& f) &&
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, E&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, E&&>>;
        if (has_value_) {
            return Ret::success();
        }
        return libk::forward<F>(f)(libk::move(storage_.error_));
    }

    template<typename F>
    requires(expected_detail::ExpectedResultHasValueV<expected_detail::InvokeResultT<F&&, const E&&>, void>)
    constexpr auto or_else(F&& f) const &&
        -> remove_cvr_t<expected_detail::InvokeResultT<F&&, const E&&>> {
        using Ret = remove_cvr_t<expected_detail::InvokeResultT<F&&, const E&&>>;
        if (has_value_) {
            return Ret::success();
        }
        return libk::forward<F>(f)(libk::move(storage_.error_));
    }

    constexpr ~Expected() {
        destroy_active();
    }

private:
    struct SuccessTag {};
    struct ErrorTag {};

    union Storage {
        constexpr Storage() : empty_{} {}
        constexpr ~Storage() {}

        char empty_;
        E error_;
    } storage_{};

    bool has_value_{true};

    constexpr Expected(SuccessTag)
        : has_value_(true) {}

    template<typename... Args>
    requires(ConstructibleFrom<E, Args&&...>)
    constexpr Expected(ErrorTag, Args&&... args) {
        libk::construct_at(&storage_.error_, libk::forward<Args>(args)...);
        has_value_ = false;
    }

    constexpr void destroy_active() noexcept {
        if (!has_value_) {
            libk::destroy_at(&storage_.error_);
        }
    }

    constexpr void move_construct_from(Expected&& other) {
        if (other.has_value_) {
            has_value_ = true;
        } else {
            libk::construct_at(&storage_.error_, libk::move(other.storage_.error_));
            has_value_ = false;
        }
    }

    constexpr void copy_construct_from(const Expected& other) {
        if (other.has_value_) {
            has_value_ = true;
        } else {
            libk::construct_at(&storage_.error_, other.storage_.error_);
            has_value_ = false;
        }
    }
};

} // namespace libk
