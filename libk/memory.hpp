#pragma once
#include <libk/assert.hpp>
#include <libk/concepts.hpp>
#include <libk/new.hpp>
#include <libk/utility.hpp>

namespace libk {

template<typename T, typename... Args>
constexpr auto construct_at(T* location, Args&&... arguments)
    noexcept(is_nothrow_constructible_v<T, Args&&...>) -> T* {
    return ::new(static_cast<void*>(location))
        T(libk::forward<Args>(arguments)...);
}

template<typename T>
constexpr void destroy_at(T* location) noexcept {
    location->~T();
}

template<typename T>
class observer_ptr {
public:
    using element_type = T;
    using pointer = T*;

    constexpr observer_ptr() noexcept = default;
    constexpr observer_ptr(decltype(nullptr)) noexcept {}
    constexpr explicit observer_ptr(pointer ptr) noexcept : ptr_(ptr) {}

    template<typename U>
        requires ConvertibleTo<U*, pointer>
    constexpr observer_ptr(observer_ptr<U> other) noexcept : ptr_(other.get()) {}

    constexpr pointer release() noexcept {
        pointer old = ptr_;
        ptr_ = nullptr;
        return old;
    }

    constexpr void reset(pointer ptr = nullptr) noexcept {
        ptr_ = ptr;
    }

    constexpr void swap(observer_ptr& other) noexcept {
        pointer tmp = ptr_;
        ptr_ = other.ptr_;
        other.ptr_ = tmp;
    }

    [[nodiscard]] constexpr pointer get() const noexcept {
        return ptr_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }

    [[nodiscard]] constexpr T& operator*() const noexcept {
        libk_assert(ptr_ != nullptr);
        return *ptr_;
    }

    [[nodiscard]] constexpr pointer operator->() const noexcept {
        libk_assert(ptr_ != nullptr);
        return ptr_;
    }

private:
    pointer ptr_ = nullptr;
};

template<typename T>
observer_ptr(T*) -> observer_ptr<T>;

template<typename T, typename U>
[[nodiscard]] constexpr auto operator==(
    observer_ptr<T> lhs,
    observer_ptr<U> rhs) noexcept -> bool {
    return lhs.get() == rhs.get();
}

template<typename T>
[[nodiscard]] constexpr auto operator==(
    observer_ptr<T> lhs,
    decltype(nullptr)) noexcept -> bool {
    return lhs.get() == nullptr;
}

template<typename T>
[[nodiscard]] constexpr auto operator==(
    decltype(nullptr),
    observer_ptr<T> rhs) noexcept -> bool {
    return rhs == nullptr;
}

template<typename Ptr>
class not_null {
    static_assert(Pointer<Ptr>, "not_null currently supports raw pointer types");

public:
    using pointer = Ptr;
    using element_type = remove_extent_t<
        remove_ref_t<decltype(*static_cast<Ptr>(nullptr))>>;

    constexpr explicit not_null(pointer ptr) noexcept : ptr_(ptr) {
        libk_assert(ptr_ != nullptr);
    }

    template<typename U>
        requires ConvertibleTo<U, pointer>
    constexpr not_null(not_null<U> other) noexcept
        : ptr_(static_cast<pointer>(other.get())) {}

    not_null(decltype(nullptr)) = delete;
    not_null& operator=(decltype(nullptr)) = delete;

    constexpr not_null& operator=(pointer ptr) noexcept {
        libk_assert(ptr != nullptr);
        ptr_ = ptr;
        return *this;
    }

    [[nodiscard]] constexpr pointer get() const noexcept {
        return ptr_;
    }

    [[nodiscard]] constexpr operator pointer() const noexcept {
        return ptr_;
    }

    [[nodiscard]] constexpr element_type& operator*() const noexcept {
        return *ptr_;
    }

    [[nodiscard]] constexpr pointer operator->() const noexcept {
        return ptr_;
    }

private:
    pointer ptr_;
};

template<typename T>
not_null(T*) -> not_null<T*>;

template<typename T, typename U>
[[nodiscard]] constexpr auto operator==(
    not_null<T> lhs,
    not_null<U> rhs) noexcept -> bool {
    return lhs.get() == rhs.get();
}

template<typename T, typename U>
[[nodiscard]] constexpr auto operator==(
    not_null<T> lhs,
    U* rhs) noexcept -> bool {
    return lhs.get() == rhs;
}

template<typename T, typename U>
[[nodiscard]] constexpr auto operator==(
    T* lhs,
    not_null<U> rhs) noexcept -> bool {
    return lhs == rhs.get();
}

} // namespace libk
