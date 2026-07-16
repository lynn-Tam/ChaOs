#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/concepts.hpp>
#include <libk/assert.hpp>

namespace libk {

template<typename T>
concept SpanElement = !libk::is_void_v<T>
    && !libk::is_reference_v<T>
    && !libk::is_function_v<T>;

template<SpanElement T>
class Span {
public:
    constexpr Span() noexcept = default;
    constexpr Span(T* pointer, size_t count) noexcept
        : pointer_(pointer), size_(count) {
        libk_assert(pointer_ != nullptr || size_ == 0);
    }

    template<typename U, size_t Size>
        requires libk::ConvertibleTo<U (*)[], T (*)[]>
    constexpr Span(U (&values)[Size]) noexcept
        : pointer_(values), size_(Size) {}

    template<typename U>
        requires libk::ConvertibleTo<U (*)[], T (*)[]>
    constexpr Span(const Span<U>& other) noexcept
        : pointer_(other.data()), size_(other.size()) {}

    [[nodiscard]] constexpr auto data() noexcept -> T* { return pointer_; }
    [[nodiscard]] constexpr auto data() const noexcept -> const T* {
        return pointer_;
    }
    [[nodiscard]] constexpr auto begin() noexcept -> T* { return pointer_; }
    [[nodiscard]] constexpr auto begin() const noexcept -> const T* {
        return pointer_;
    }
    [[nodiscard]] constexpr auto end() noexcept -> T* {
        return pointer_ == nullptr ? nullptr : pointer_ + size_;
    }
    [[nodiscard]] constexpr auto end() const noexcept -> const T* {
        return pointer_ == nullptr ? nullptr : pointer_ + size_;
    }

    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return size_;
    }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return size_ == 0;
    }

    [[nodiscard]] constexpr auto slice(
        size_t offset,
        size_t count) const noexcept -> Span {
        libk_assert(offset <= size_);
        libk_assert(count <= size_ - offset);
        if (pointer_ == nullptr) {
            return {};
        }
        return Span{pointer_ + offset, count};
    }

    [[nodiscard]] constexpr auto slice(size_t offset) const noexcept -> Span {
        libk_assert(offset <= size_);
        return slice(offset, size_ - offset);
    }

    [[nodiscard]] constexpr auto operator[](size_t index) noexcept -> T& {
        libk_assert(index < size_);
        return pointer_[index];
    }
    [[nodiscard]] constexpr auto operator[](size_t index) const noexcept
        -> const T& {
        libk_assert(index < size_);
        return pointer_[index];
    }

private:
    T* pointer_{};
    size_t size_{};
};

using ByteSpan = Span<const uint8_t>;

} // namespace libk
