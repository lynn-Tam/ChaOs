#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/assert.hpp>
#include <libk/concepts.hpp>
#include <libk/memory.hpp>
#include <libk/span.hpp>
#include <libk/typetraits.hpp>
#include <libk/utility.hpp>

namespace libk {

template<typename T, size_t Capacity>
    requires(Object<T> && !is_const_v<T>)
class InplaceVector final {
public:
    InplaceVector() noexcept = default;

    template<typename... Args>
        requires(
            sizeof...(Args) <= Capacity
            && (... && ConstructibleFrom<T, Args&&>))
    explicit InplaceVector(Args&&... arguments)
        noexcept((is_nothrow_constructible_v<T, Args&&> && ...)) {
        (static_cast<void>(try_emplace_back(
            libk::forward<Args>(arguments))), ...);
    }

    InplaceVector(const InplaceVector&) = delete;
    auto operator=(const InplaceVector&) -> InplaceVector& = delete;

    InplaceVector(InplaceVector&& other)
        noexcept(is_nothrow_move_constructible_v<T>) {
        move_from(other);
    }

    auto operator=(InplaceVector&& other)
        noexcept(is_nothrow_move_constructible_v<T>) -> InplaceVector& {
        if (this != &other) {
            clear();
            move_from(other);
        }
        return *this;
    }

    ~InplaceVector() noexcept { clear(); }

    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return size_;
    }
    [[nodiscard]] static constexpr auto capacity() noexcept -> size_t {
        return Capacity;
    }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return size_ == 0;
    }

    [[nodiscard]] auto data() noexcept -> T* { return ptr_at(0); }
    [[nodiscard]] auto data() const noexcept -> const T* { return ptr_at(0); }
    [[nodiscard]] auto begin() noexcept -> T* { return data(); }
    [[nodiscard]] auto begin() const noexcept -> const T* { return data(); }
    [[nodiscard]] auto end() noexcept -> T* { return ptr_at(size_); }
    [[nodiscard]] auto end() const noexcept -> const T* { return ptr_at(size_); }

    [[nodiscard]] auto operator[](size_t index) noexcept -> T& {
        libk_assert(index < size_);
        return *ptr_at(index);
    }
    [[nodiscard]] auto operator[](size_t index) const noexcept -> const T& {
        libk_assert(index < size_);
        return *ptr_at(index);
    }
    [[nodiscard]] auto front() noexcept -> T& {
        libk_assert(size_ != 0);
        return (*this)[0];
    }
    [[nodiscard]] auto front() const noexcept -> const T& {
        libk_assert(size_ != 0);
        return (*this)[0];
    }
    [[nodiscard]] auto back() noexcept -> T& {
        libk_assert(size_ != 0);
        return (*this)[size_ - 1];
    }
    [[nodiscard]] auto back() const noexcept -> const T& {
        libk_assert(size_ != 0);
        return (*this)[size_ - 1];
    }

    template<typename U>
        requires ConstructibleFrom<T, U&&>
    [[nodiscard]] auto try_push_back(U&& value)
        noexcept(is_nothrow_constructible_v<T, U&&>) -> bool {
        return try_emplace_back(libk::forward<U>(value));
    }

    template<typename... Args>
        requires ConstructibleFrom<T, Args&&...>
    [[nodiscard]] auto try_emplace_back(Args&&... arguments)
        noexcept(is_nothrow_constructible_v<T, Args&&...>) -> bool {
        if (size_ == Capacity) {
            return false;
        }
        libk::construct_at(
            ptr_at(size_), libk::forward<Args>(arguments)...);
        ++size_;
        return true;
    }

    [[nodiscard]] auto try_pop_back() noexcept -> bool {
        if (size_ == 0) {
            return false;
        }
        --size_;
        libk::destroy_at(ptr_at(size_));
        return true;
    }

    template<typename U>
        requires(
            ConstructibleFrom<T, U&&>
            && ConstructibleFrom<T, T&&>
            && AssignableFrom<T&, T&&>)
    auto insert(T* position, U&& value)
        noexcept(
            is_nothrow_constructible_v<T, U&&>
            && is_nothrow_move_constructible_v<T>
            && is_nothrow_move_assignable_v<T>) -> T* {
        libk_assert(size_ < Capacity);
        size_t index{};
        libk_assert(position_index(position, true, index));

        // Stage first so inserting an element already owned by this vector is
        // well-defined even when the subsequent shift moves that element.
        T staged(libk::forward<U>(value));
        if (index == size_) {
            libk::construct_at(ptr_at(size_), libk::move(staged));
        } else {
            libk::construct_at(ptr_at(size_), libk::move(back()));
            for (size_t cursor = size_ - 1; cursor > index; --cursor) {
                *ptr_at(cursor) = libk::move(*ptr_at(cursor - 1));
            }
            *ptr_at(index) = libk::move(staged);
        }
        ++size_;
        return ptr_at(index);
    }

    auto erase(T* position) noexcept(is_nothrow_move_assignable_v<T>) -> T*
        requires AssignableFrom<T&, T&&> {
        size_t index{};
        libk_assert(position_index(position, false, index));
        for (size_t cursor = index; cursor + 1 < size_; ++cursor) {
            *ptr_at(cursor) = libk::move(*ptr_at(cursor + 1));
        }
        --size_;
        libk::destroy_at(ptr_at(size_));
        return ptr_at(index);
    }

    template<typename U>
        requires(
            ConstructibleFrom<T, U&&>
            && ConstructibleFrom<T, T&&>)
    void replace(T* position, U&& value)
        noexcept(
            is_nothrow_constructible_v<T, U&&>
            && is_nothrow_move_constructible_v<T>) {
        size_t index{};
        libk_assert(position_index(position, false, index));
        T staged(libk::forward<U>(value));
        libk::destroy_at(ptr_at(index));
        libk::construct_at(ptr_at(index), libk::move(staged));
    }

    void clear() noexcept {
        while (size_ != 0) {
            --size_;
            libk::destroy_at(ptr_at(size_));
        }
    }

    [[nodiscard]] auto span() noexcept -> Span<T> {
        return Span<T>{data(), size_};
    }
    [[nodiscard]] auto span() const noexcept -> Span<const T> {
        return Span<const T>{data(), size_};
    }

private:
    static constexpr size_t storage_capacity = Capacity == 0 ? 1 : Capacity;

    [[nodiscard]] auto ptr_at(size_t index) noexcept -> T* {
        if constexpr (Capacity == 0) {
            return nullptr;
        }
        return reinterpret_cast<T*>(storage_ + index * sizeof(T));
    }
    [[nodiscard]] auto ptr_at(size_t index) const noexcept -> const T* {
        if constexpr (Capacity == 0) {
            return nullptr;
        }
        return reinterpret_cast<const T*>(storage_ + index * sizeof(T));
    }

    [[nodiscard]] auto position_index(
        const T* position,
        bool allow_end,
        size_t& index) const noexcept -> bool {
        if constexpr (Capacity == 0) {
            return false;
        }
        const uintptr_t first = reinterpret_cast<uintptr_t>(data());
        const uintptr_t candidate = reinterpret_cast<uintptr_t>(position);
        const size_t limit = allow_end ? size_ : size_ - (size_ != 0);
        if (candidate < first) {
            return false;
        }
        const uintptr_t displacement = candidate - first;
        if (displacement % sizeof(T) != 0) {
            return false;
        }
        index = displacement / sizeof(T);
        return index <= limit && (allow_end || index < size_);
    }

    void move_from(InplaceVector& other)
        noexcept(is_nothrow_move_constructible_v<T>) {
        for (size_t index = 0; index < other.size_; ++index) {
            libk::construct_at(ptr_at(index), libk::move(other[index]));
            ++size_;
        }
        other.clear();
    }

    alignas(T) uint8_t storage_[sizeof(T) * storage_capacity]{};
    size_t size_{};
};

} // namespace libk
