#pragma once

#include <stddef.h>

#include <libk/assert.hpp>

namespace libk {

template<typename T, size_t N>
struct Array final {
    T elements[N];

    [[nodiscard]] static constexpr auto size() noexcept -> size_t { return N; }
    [[nodiscard]] static constexpr auto empty() noexcept -> bool { return false; }
    [[nodiscard]] constexpr auto data() noexcept -> T* { return elements; }
    [[nodiscard]] constexpr auto data() const noexcept -> const T* {
        return elements;
    }
    [[nodiscard]] constexpr auto begin() noexcept -> T* { return elements; }
    [[nodiscard]] constexpr auto begin() const noexcept -> const T* {
        return elements;
    }
    [[nodiscard]] constexpr auto end() noexcept -> T* { return elements + N; }
    [[nodiscard]] constexpr auto end() const noexcept -> const T* {
        return elements + N;
    }
    [[nodiscard]] constexpr auto operator[](size_t index) noexcept -> T& {
        libk_assert(index < N);
        return elements[index];
    }
    [[nodiscard]] constexpr auto operator[](size_t index) const noexcept
        -> const T& {
        libk_assert(index < N);
        return elements[index];
    }
};

template<typename T>
struct Array<T, 0> final {
    [[nodiscard]] static constexpr auto size() noexcept -> size_t { return 0; }
    [[nodiscard]] static constexpr auto empty() noexcept -> bool { return true; }
    [[nodiscard]] constexpr auto data() noexcept -> T* { return nullptr; }
    [[nodiscard]] constexpr auto data() const noexcept -> const T* {
        return nullptr;
    }
    [[nodiscard]] constexpr auto begin() noexcept -> T* { return nullptr; }
    [[nodiscard]] constexpr auto begin() const noexcept -> const T* {
        return nullptr;
    }
    [[nodiscard]] constexpr auto end() noexcept -> T* { return nullptr; }
    [[nodiscard]] constexpr auto end() const noexcept -> const T* {
        return nullptr;
    }
    [[nodiscard]] constexpr auto operator[]([[maybe_unused]] size_t index)
        noexcept -> T& {
        libk_assert(false);
        __builtin_unreachable();
    }
    [[nodiscard]] constexpr auto operator[]([[maybe_unused]] size_t index) const
        noexcept -> const T& {
        libk_assert(false);
        __builtin_unreachable();
    }
};

} // namespace libk
