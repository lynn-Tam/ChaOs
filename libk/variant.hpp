// libk/variant.hpp
// Freestanding tagged union for values with exactly one active typed payload.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "libk/assert.hpp"
#include "libk/concepts.hpp"
#include "libk/memory.hpp"
#include "libk/typetraits.hpp"
#include "libk/utility.hpp"

namespace libk {

struct monostate {};

[[nodiscard]] constexpr auto operator==(monostate, monostate) noexcept -> bool {
    return true;
}

template<size_t I>
struct in_place_index_t {
    explicit constexpr in_place_index_t() = default;
};

template<size_t I>
inline constexpr in_place_index_t<I> in_place_index{};

template<typename T>
struct in_place_type_t {
    explicit constexpr in_place_type_t() = default;
};

template<typename T>
inline constexpr in_place_type_t<T> in_place_type{};

inline constexpr size_t variant_npos = static_cast<size_t>(-1);

template<typename... Ts>
class variant;

template<typename T>
struct variant_size;

template<typename... Ts>
struct variant_size<variant<Ts...>> {
    static constexpr size_t value = sizeof...(Ts);
};

template<typename T>
struct variant_size<const T> : variant_size<T> {};

template<typename T>
struct variant_size<volatile T> : variant_size<T> {};

template<typename T>
struct variant_size<const volatile T> : variant_size<T> {};

template<typename T>
inline constexpr size_t variant_size_v = variant_size<T>::value;

template<size_t I, typename T>
struct variant_alternative;

namespace variant_detail {

template<size_t I>
struct IndexConstant {
    static constexpr size_t value = I;
};

template<typename T>
T&& declval() noexcept;

[[noreturn]] inline void unreachable() noexcept {
    libk_assert(false);
#if defined(__GNUC__) || defined(__clang__)
    __builtin_unreachable();
#else
    for (;;) {
    }
#endif
}

template<size_t I, typename... Ts>
struct TypeAt;

template<size_t I, typename T, typename... Rest>
struct TypeAt<I, T, Rest...> : TypeAt<I - 1, Rest...> {};

template<typename T, typename... Rest>
struct TypeAt<0, T, Rest...> {
    using type = T;
};

template<size_t I, typename... Ts>
using TypeAtT = typename TypeAt<I, Ts...>::type;

template<typename T, typename... Ts>
struct CountType;

template<typename T>
struct CountType<T> {
    static constexpr size_t value = 0;
};

template<typename T, typename Head, typename... Rest>
struct CountType<T, Head, Rest...> {
    static constexpr size_t value = (SameAs<T, Head> ? 1u : 0u) + CountType<T, Rest...>::value;
};

template<typename T, typename... Ts>
inline constexpr size_t CountTypeV = CountType<T, Ts...>::value;

template<typename T, size_t I, typename... Ts>
struct FirstIndexOf;

template<typename T, size_t I, typename Head, typename... Rest>
struct FirstIndexOf<T, I, Head, Rest...> {
    static constexpr size_t value = SameAs<T, Head>
        ? I
        : FirstIndexOf<T, I + 1, Rest...>::value;
};

template<typename T, size_t I>
struct FirstIndexOf<T, I> {
    static constexpr size_t value = variant_npos;
};

template<typename T, typename... Ts>
inline constexpr size_t FirstIndexOfV = FirstIndexOf<T, 0, Ts...>::value;

template<typename... Ts>
inline constexpr bool ValidAlternativeListV =
    (sizeof...(Ts) > 0)
    && (... && Object<Ts>)
    && (... && !BuiltinArray<Ts>)
    && (... && !is_const_v<Ts>)
    && (... && !is_volatile_v<Ts>);

template<size_t Count>
struct SmallestIndex {
    using type = conditional_t<
        (Count <= 0xFFu),
        uint8_t,
        conditional_t<(Count <= 0xFFFFu), uint16_t, uint32_t>
    >;
};

template<size_t Count>
using SmallestIndexT = typename SmallestIndex<Count>::type;

template<size_t I, typename... Ts>
union VariantUnion;

template<size_t I, typename T>
union VariantUnion<I, T> {
    char dummy_;
    T value_;

    constexpr VariantUnion() noexcept : dummy_{} {}

    template<typename... Args>
    requires ConstructibleFrom<T, Args&&...>
    constexpr explicit VariantUnion(IndexConstant<I>, Args&&... args)
        : value_(libk::forward<Args>(args)...) {}

    constexpr ~VariantUnion() {}
};

template<size_t I, typename T, typename... Rest>
union VariantUnion<I, T, Rest...> {
    char dummy_;
    T value_;
    VariantUnion<I + 1, Rest...> tail_;

    constexpr VariantUnion() noexcept : dummy_{} {}

    template<typename... Args>
    requires ConstructibleFrom<T, Args&&...>
    constexpr explicit VariantUnion(IndexConstant<I>, Args&&... args)
        : value_(libk::forward<Args>(args)...) {}

    template<size_t Target, typename... Args>
    requires(Target != I)
    constexpr explicit VariantUnion(IndexConstant<Target>, Args&&... args)
        : tail_(IndexConstant<Target>{}, libk::forward<Args>(args)...) {}

    constexpr ~VariantUnion() {}
};

template<size_t Target, size_t I, typename T>
[[nodiscard]] constexpr auto get_union(VariantUnion<I, T>& storage) noexcept -> T& {
    static_assert(Target == I, "variant index is out of range");
    return storage.value_;
}

template<size_t Target, size_t I, typename T>
[[nodiscard]] constexpr auto get_union(const VariantUnion<I, T>& storage) noexcept -> const T& {
    static_assert(Target == I, "variant index is out of range");
    return storage.value_;
}

template<size_t Target, size_t I, typename T>
[[nodiscard]] constexpr auto get_union(VariantUnion<I, T>&& storage) noexcept -> T&& {
    static_assert(Target == I, "variant index is out of range");
    return libk::move(storage.value_);
}

template<size_t Target, size_t I, typename T>
[[nodiscard]] constexpr auto get_union(const VariantUnion<I, T>&& storage) noexcept -> const T&& {
    static_assert(Target == I, "variant index is out of range");
    return libk::move(storage.value_);
}

template<size_t Target, size_t I, typename T, typename... Rest>
    requires(sizeof...(Rest) > 0)
[[nodiscard]] constexpr auto get_union(VariantUnion<I, T, Rest...>& storage) noexcept
    -> TypeAtT<Target - I, T, Rest...>& {
    if constexpr (Target == I) {
        return storage.value_;
    } else {
        return get_union<Target>(storage.tail_);
    }
}

template<size_t Target, size_t I, typename T, typename... Rest>
    requires(sizeof...(Rest) > 0)
[[nodiscard]] constexpr auto get_union(const VariantUnion<I, T, Rest...>& storage) noexcept
    -> const TypeAtT<Target - I, T, Rest...>& {
    if constexpr (Target == I) {
        return storage.value_;
    } else {
        return get_union<Target>(storage.tail_);
    }
}

template<size_t Target, size_t I, typename T, typename... Rest>
    requires(sizeof...(Rest) > 0)
[[nodiscard]] constexpr auto get_union(VariantUnion<I, T, Rest...>&& storage) noexcept
    -> TypeAtT<Target - I, T, Rest...>&& {
    if constexpr (Target == I) {
        return libk::move(storage.value_);
    } else {
        return get_union<Target>(libk::move(storage.tail_));
    }
}

template<size_t Target, size_t I, typename T, typename... Rest>
    requires(sizeof...(Rest) > 0)
[[nodiscard]] constexpr auto get_union(const VariantUnion<I, T, Rest...>&& storage) noexcept
    -> const TypeAtT<Target - I, T, Rest...>&& {
    if constexpr (Target == I) {
        return libk::move(storage.value_);
    } else {
        return get_union<Target>(libk::move(storage.tail_));
    }
}

template<size_t Target, size_t I, typename T, typename... Rest, typename... Args>
constexpr void construct_union(VariantUnion<I, T, Rest...>* storage, Args&&... args) {
    if constexpr (Target == I) {
        libk::construct_at(&storage->value_, libk::forward<Args>(args)...);
    } else {
        libk::construct_at(&storage->tail_, IndexConstant<Target>{}, libk::forward<Args>(args)...);
    }
}

template<size_t I, typename T, typename... Rest>
constexpr void destroy_union(size_t index, VariantUnion<I, T, Rest...>* storage) noexcept {
    if constexpr (is_trivially_destructible_v<T> && (... && is_trivially_destructible_v<Rest>)) {
        (void)index;
        (void)storage;
    } else {
        if (index == I) {
            if constexpr (!is_trivially_destructible_v<T>) {
                libk::destroy_at(&storage->value_);
            }
        } else {
            if constexpr (sizeof...(Rest) > 0) {
                destroy_union(index, &storage->tail_);
            } else {
                unreachable();
            }
        }
    }
}

} // namespace variant_detail

template<size_t I, typename... Ts>
struct variant_alternative<I, variant<Ts...>> {
    static_assert(I < sizeof...(Ts), "variant alternative index is out of range");
    using type = variant_detail::TypeAtT<I, Ts...>;
};

template<size_t I, typename T>
struct variant_alternative<I, const T> {
    using type = const typename variant_alternative<I, T>::type;
};

template<size_t I, typename T>
struct variant_alternative<I, volatile T> {
    using type = volatile typename variant_alternative<I, T>::type;
};

template<size_t I, typename T>
struct variant_alternative<I, const volatile T> {
    using type = const volatile typename variant_alternative<I, T>::type;
};

template<size_t I, typename T>
using variant_alternative_t = typename variant_alternative<I, T>::type;

template<typename... Ts>
class variant {
    static_assert(variant_detail::ValidAlternativeListV<Ts...>, "variant alternatives must be non-cv object types and must not be arrays");

    using Storage = variant_detail::VariantUnion<0, Ts...>;
    using IndexStorage = variant_detail::SmallestIndexT<sizeof...(Ts)>;

    static constexpr IndexStorage storage_npos_ = static_cast<IndexStorage>(-1);

public:
    using self_type = variant;

    constexpr variant()
    requires ConstructibleFrom<variant_alternative_t<0, variant>>
        : storage_(variant_detail::IndexConstant<0>{}), index_(0) {}

    constexpr variant(const variant& other)
    requires((ConstructibleFrom<Ts, const Ts&> && ...))
        : storage_(), index_(storage_npos_) {
        copy_construct_active(other);
    }

    constexpr variant(variant&& other)
    requires((ConstructibleFrom<Ts, Ts&&> && ...))
        : storage_(), index_(storage_npos_) {
        move_construct_active(libk::move(other));
    }

    template<size_t I, typename... Args>
    requires(I < sizeof...(Ts) && ConstructibleFrom<variant_alternative_t<I, variant>, Args&&...>)
    constexpr explicit variant(in_place_index_t<I>, Args&&... args)
        : storage_(variant_detail::IndexConstant<I>{}, libk::forward<Args>(args)...),
          index_(static_cast<IndexStorage>(I)) {}

    template<typename T, typename... Args>
    requires(
        variant_detail::CountTypeV<T, Ts...> == 1
        && ConstructibleFrom<T, Args&&...>
    )
    constexpr explicit variant(in_place_type_t<T>, Args&&... args)
        : variant(in_place_index<variant_detail::FirstIndexOfV<T, Ts...>>, libk::forward<Args>(args)...) {}

    template<typename T>
    requires(
        !SameAs<remove_cvr_t<T>, variant>
        && variant_detail::CountTypeV<remove_cvr_t<T>, Ts...> == 1
        && ConstructibleFrom<remove_cvr_t<T>, T&&>
    )
    constexpr variant(T&& value)
        : variant(in_place_index<variant_detail::FirstIndexOfV<remove_cvr_t<T>, Ts...>>, libk::forward<T>(value)) {}

    constexpr ~variant() {
        destroy_active();
    }

    constexpr auto operator=(const variant& other) -> variant&
    requires((ConstructibleFrom<Ts, const Ts&> && ...)) {
        if (this == &other) {
            return *this;
        }
        assign_copy(other);
        return *this;
    }

    constexpr auto operator=(variant&& other) -> variant&
    requires((ConstructibleFrom<Ts, Ts&&> && ...)) {
        if (this == &other) {
            return *this;
        }
        assign_move(libk::move(other));
        return *this;
    }

    template<typename T>
    requires(
        !SameAs<remove_cvr_t<T>, variant>
        && variant_detail::CountTypeV<remove_cvr_t<T>, Ts...> == 1
        && ConstructibleFrom<remove_cvr_t<T>, T&&>
    )
    constexpr auto operator=(T&& value) -> variant& {
        constexpr size_t I = variant_detail::FirstIndexOfV<remove_cvr_t<T>, Ts...>;
        assign_value<I>(libk::forward<T>(value));
        return *this;
    }

    [[nodiscard]] constexpr auto index() const noexcept -> size_t {
        if (index_ == storage_npos_) {
            return variant_npos;
        }
        return static_cast<size_t>(index_);
    }

    [[nodiscard]] constexpr auto valueless_by_exception() const noexcept -> bool {
        return index_ == storage_npos_;
    }

    template<size_t I>
    [[nodiscard]] constexpr auto holds() const noexcept -> bool {
        static_assert(I < sizeof...(Ts), "variant alternative index is out of range");
        return index_ == I;
    }

    template<size_t I, typename... Args>
    requires(I < sizeof...(Ts) && ConstructibleFrom<variant_alternative_t<I, variant>, Args&&...>)
    constexpr auto emplace(Args&&... args) -> variant_alternative_t<I, variant>& {
        destroy_active();
        variant_detail::construct_union<I>(&storage_, libk::forward<Args>(args)...);
        index_ = static_cast<IndexStorage>(I);
        return variant_detail::get_union<I>(storage_);
    }

    template<typename T, typename... Args>
    requires(
        variant_detail::CountTypeV<T, Ts...> == 1
        && ConstructibleFrom<T, Args&&...>
    )
    constexpr auto emplace(Args&&... args) -> T& {
        constexpr size_t I = variant_detail::FirstIndexOfV<T, Ts...>;
        return emplace<I>(libk::forward<Args>(args)...);
    }

private:
    template<size_t I>
    using Alternative = variant_alternative_t<I, variant>;

    constexpr void destroy_active() noexcept {
        if (index_ == storage_npos_) {
            return;
        }
        variant_detail::destroy_union(index(), &storage_);
        index_ = storage_npos_;
    }

    constexpr void copy_construct_active(const variant& other)
    requires((ConstructibleFrom<Ts, const Ts&> && ...)) {
        copy_construct_active_from_index<0>(other.index(), other.storage_);
        index_ = other.index_;
    }

    constexpr void move_construct_active(variant&& other)
    requires((ConstructibleFrom<Ts, Ts&&> && ...)) {
        move_construct_active_from_index<0>(other.index(), libk::move(other.storage_));
        index_ = other.index_;
    }

    template<size_t I>
    constexpr void copy_construct_active_from_index(size_t active, const Storage& other_storage) {
        if constexpr (I < sizeof...(Ts)) {
            if (active == I) {
                variant_detail::construct_union<I>(&storage_, variant_detail::get_union<I>(other_storage));
            } else {
                copy_construct_active_from_index<I + 1>(active, other_storage);
            }
        } else {
            variant_detail::unreachable();
        }
    }

    template<size_t I>
    constexpr void move_construct_active_from_index(size_t active, Storage&& other_storage) {
        if constexpr (I < sizeof...(Ts)) {
            if (active == I) {
                variant_detail::construct_union<I>(&storage_, variant_detail::get_union<I>(libk::move(other_storage)));
            } else {
                move_construct_active_from_index<I + 1>(active, libk::move(other_storage));
            }
        } else {
            variant_detail::unreachable();
        }
    }

    constexpr void assign_copy(const variant& other)
    requires((ConstructibleFrom<Ts, const Ts&> && ...)) {
        if (index_ == other.index_) {
            assign_same_copy<0>(other.index_, other.storage_);
            return;
        }
        destroy_active();
        copy_construct_active(other);
    }

    constexpr void assign_move(variant&& other)
    requires((ConstructibleFrom<Ts, Ts&&> && ...)) {
        if (index_ == other.index_) {
            assign_same_move<0>(other.index_, libk::move(other.storage_));
            return;
        }
        destroy_active();
        move_construct_active(libk::move(other));
    }

    template<size_t I>
    constexpr void assign_same_copy(size_t active, const Storage& other_storage) {
        if constexpr (I < sizeof...(Ts)) {
            if (active == I) {
                if constexpr (AssignableFrom<Alternative<I>&, const Alternative<I>&>) {
                    variant_detail::get_union<I>(storage_) = variant_detail::get_union<I>(other_storage);
                } else {
                    destroy_active();
                    variant_detail::construct_union<I>(&storage_, variant_detail::get_union<I>(other_storage));
                    index_ = static_cast<IndexStorage>(I);
                }
            } else {
                assign_same_copy<I + 1>(active, other_storage);
            }
        } else {
            variant_detail::unreachable();
        }
    }

    template<size_t I>
    constexpr void assign_same_move(size_t active, Storage&& other_storage) {
        if constexpr (I < sizeof...(Ts)) {
            if (active == I) {
                if constexpr (AssignableFrom<Alternative<I>&, Alternative<I>&&>) {
                    variant_detail::get_union<I>(storage_) = variant_detail::get_union<I>(libk::move(other_storage));
                } else {
                    destroy_active();
                    variant_detail::construct_union<I>(&storage_, variant_detail::get_union<I>(libk::move(other_storage)));
                    index_ = static_cast<IndexStorage>(I);
                }
            } else {
                assign_same_move<I + 1>(active, libk::move(other_storage));
            }
        } else {
            variant_detail::unreachable();
        }
    }

    template<size_t I, typename U>
    constexpr void assign_value(U&& value) {
        if (index_ == I) {
            if constexpr (AssignableFrom<Alternative<I>&, U&&>) {
                variant_detail::get_union<I>(storage_) = libk::forward<U>(value);
            } else {
                destroy_active();
                variant_detail::construct_union<I>(&storage_, libk::forward<U>(value));
                index_ = static_cast<IndexStorage>(I);
            }
        } else {
            destroy_active();
            variant_detail::construct_union<I>(&storage_, libk::forward<U>(value));
            index_ = static_cast<IndexStorage>(I);
        }
    }

    Storage storage_{};
    IndexStorage index_{storage_npos_};

    template<size_t I, typename... Us>
    friend constexpr auto get(variant<Us...>& value) noexcept -> variant_alternative_t<I, variant<Us...>>&;

    template<size_t I, typename... Us>
    friend constexpr auto get(const variant<Us...>& value) noexcept -> const variant_alternative_t<I, variant<Us...>>&;

    template<size_t I, typename... Us>
    friend constexpr auto get(variant<Us...>&& value) noexcept -> variant_alternative_t<I, variant<Us...>>&&;

    template<size_t I, typename... Us>
    friend constexpr auto get(const variant<Us...>&& value) noexcept -> const variant_alternative_t<I, variant<Us...>>&&;

    template<size_t I, typename... Us>
    friend constexpr auto get_if(variant<Us...>* value) noexcept -> variant_alternative_t<I, variant<Us...>>*;

    template<size_t I, typename... Us>
    friend constexpr auto get_if(const variant<Us...>* value) noexcept -> const variant_alternative_t<I, variant<Us...>>*;
};

template<size_t I, typename... Ts>
[[nodiscard]] constexpr auto get(variant<Ts...>& value) noexcept -> variant_alternative_t<I, variant<Ts...>>& {
    static_assert(I < sizeof...(Ts), "variant alternative index is out of range");
    libk_assert(value.index_ == I);
    return variant_detail::get_union<I>(value.storage_);
}

template<size_t I, typename... Ts>
[[nodiscard]] constexpr auto get(const variant<Ts...>& value) noexcept -> const variant_alternative_t<I, variant<Ts...>>& {
    static_assert(I < sizeof...(Ts), "variant alternative index is out of range");
    libk_assert(value.index_ == I);
    return variant_detail::get_union<I>(value.storage_);
}

template<size_t I, typename... Ts>
[[nodiscard]] constexpr auto get(variant<Ts...>&& value) noexcept -> variant_alternative_t<I, variant<Ts...>>&& {
    static_assert(I < sizeof...(Ts), "variant alternative index is out of range");
    libk_assert(value.index_ == I);
    return variant_detail::get_union<I>(libk::move(value.storage_));
}

template<size_t I, typename... Ts>
[[nodiscard]] constexpr auto get(const variant<Ts...>&& value) noexcept -> const variant_alternative_t<I, variant<Ts...>>&& {
    static_assert(I < sizeof...(Ts), "variant alternative index is out of range");
    libk_assert(value.index_ == I);
    return variant_detail::get_union<I>(libk::move(value.storage_));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr auto get(variant<Ts...>& value) noexcept -> T&
requires(variant_detail::CountTypeV<T, Ts...> == 1) {
    constexpr size_t I = variant_detail::FirstIndexOfV<T, Ts...>;
    return get<I>(value);
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr auto get(const variant<Ts...>& value) noexcept -> const T&
requires(variant_detail::CountTypeV<T, Ts...> == 1) {
    constexpr size_t I = variant_detail::FirstIndexOfV<T, Ts...>;
    return get<I>(value);
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr auto get(variant<Ts...>&& value) noexcept -> T&&
requires(variant_detail::CountTypeV<T, Ts...> == 1) {
    constexpr size_t I = variant_detail::FirstIndexOfV<T, Ts...>;
    return get<I>(libk::move(value));
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr auto get(const variant<Ts...>&& value) noexcept -> const T&&
requires(variant_detail::CountTypeV<T, Ts...> == 1) {
    constexpr size_t I = variant_detail::FirstIndexOfV<T, Ts...>;
    return get<I>(libk::move(value));
}

template<size_t I, typename... Ts>
[[nodiscard]] constexpr auto get_if(variant<Ts...>* value) noexcept -> variant_alternative_t<I, variant<Ts...>>* {
    static_assert(I < sizeof...(Ts), "variant alternative index is out of range");
    if (value == nullptr || value->index_ != I) {
        return nullptr;
    }
    return &variant_detail::get_union<I>(value->storage_);
}

template<size_t I, typename... Ts>
[[nodiscard]] constexpr auto get_if(const variant<Ts...>* value) noexcept -> const variant_alternative_t<I, variant<Ts...>>* {
    static_assert(I < sizeof...(Ts), "variant alternative index is out of range");
    if (value == nullptr || value->index_ != I) {
        return nullptr;
    }
    return &variant_detail::get_union<I>(value->storage_);
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr auto get_if(variant<Ts...>* value) noexcept -> T*
requires(variant_detail::CountTypeV<T, Ts...> == 1) {
    constexpr size_t I = variant_detail::FirstIndexOfV<T, Ts...>;
    return get_if<I>(value);
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr auto get_if(const variant<Ts...>* value) noexcept -> const T*
requires(variant_detail::CountTypeV<T, Ts...> == 1) {
    constexpr size_t I = variant_detail::FirstIndexOfV<T, Ts...>;
    return get_if<I>(value);
}

template<size_t I, typename... Ts>
[[nodiscard]] constexpr auto holds_alternative(const variant<Ts...>& value) noexcept -> bool {
    static_assert(I < sizeof...(Ts), "variant alternative index is out of range");
    return value.index() == I;
}

template<typename T, typename... Ts>
[[nodiscard]] constexpr auto holds_alternative(const variant<Ts...>& value) noexcept -> bool
requires(variant_detail::CountTypeV<T, Ts...> == 1) {
    constexpr size_t I = variant_detail::FirstIndexOfV<T, Ts...>;
    return value.index() == I;
}

namespace variant_detail {

template<typename Visitor, typename Variant, size_t I>
using VisitAlternativeT = decltype(
    variant_detail::declval<Visitor>()(
        get<I>(variant_detail::declval<Variant>())
    )
);

template<typename Expected, typename Visitor, typename Variant, size_t I, size_t N>
struct VisitReturnsSameImpl {
    static constexpr bool value = SameAs<Expected, VisitAlternativeT<Visitor, Variant, I>>
        && VisitReturnsSameImpl<Expected, Visitor, Variant, I + 1, N>::value;
};

template<typename Expected, typename Visitor, typename Variant, size_t N>
struct VisitReturnsSameImpl<Expected, Visitor, Variant, N, N> {
    static constexpr bool value = true;
};

template<typename Visitor, typename Variant>
inline constexpr bool VisitReturnsSameV = VisitReturnsSameImpl<
    VisitAlternativeT<Visitor, Variant, 0>,
    Visitor,
    Variant,
    1,
    variant_size_v<remove_ref_t<Variant>>
>::value;

template<size_t I, typename R, typename Visitor, typename Variant>
constexpr auto visit_impl(Visitor&& visitor, Variant&& value) -> R {
    using RawVariant = remove_ref_t<Variant>;
    if constexpr (I < variant_size_v<RawVariant>) {
        if (value.index() == I) {
            if constexpr (Void<R>) {
                libk::forward<Visitor>(visitor)(get<I>(libk::forward<Variant>(value)));
                return;
            } else {
                return libk::forward<Visitor>(visitor)(get<I>(libk::forward<Variant>(value)));
            }
        }
        return visit_impl<I + 1, R>(libk::forward<Visitor>(visitor), libk::forward<Variant>(value));
    } else {
        unreachable();
    }
}

} // namespace variant_detail

template<typename Visitor, typename Variant>
requires(variant_detail::VisitReturnsSameV<Visitor&&, Variant&&>)
constexpr auto visit(Visitor&& visitor, Variant&& value)
    -> variant_detail::VisitAlternativeT<Visitor&&, Variant&&, 0> {
    using R = variant_detail::VisitAlternativeT<Visitor&&, Variant&&, 0>;
    return variant_detail::visit_impl<0, R>(libk::forward<Visitor>(visitor), libk::forward<Variant>(value));
}

namespace variant_detail {

template<size_t I, typename... Ts>
[[nodiscard]] constexpr auto equal_by_index(const variant<Ts...>& lhs, const variant<Ts...>& rhs) -> bool {
    if constexpr (I < sizeof...(Ts)) {
        if (lhs.index() == I) {
            return get<I>(lhs) == get<I>(rhs);
        }
        return equal_by_index<I + 1>(lhs, rhs);
    } else {
        unreachable();
    }
}

} // namespace variant_detail

template<typename... Ts>
[[nodiscard]] constexpr auto operator==(const variant<Ts...>& lhs, const variant<Ts...>& rhs) -> bool
requires((requires(const Ts& a, const Ts& b) { { a == b } -> ConvertibleTo<bool>; } && ...)) {
    if (lhs.index() != rhs.index()) {
        return false;
    }
    return variant_detail::equal_by_index<0>(lhs, rhs);
}

} // namespace libk
