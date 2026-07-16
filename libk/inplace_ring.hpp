#pragma once

// Fixed-capacity allocation-free ring deque.
// Supports front/back insertion and removal with logical-order iteration.

#include <stddef.h>

#include <libk/concepts.hpp>
#include <libk/assert.hpp>
#include <libk/memory.hpp>
#include <libk/typetraits.hpp>
#include <libk/utility.hpp>

namespace libk {

template<typename T, size_t Capacity>
    requires(Object<T> && !is_const_v<T> && Capacity > 0)
class InplaceRing {
private:
    union slot {
        unsigned char empty_;
        T value_;

        constexpr slot() noexcept : empty_{} {}
        constexpr ~slot() {}
    };

    static constexpr bool power_of_two_ = (Capacity & (Capacity - 1)) == 0;

public:
    template<bool IsConst>
    class basic_iterator {
        friend class InplaceRing;
        template<bool>
        friend class basic_iterator;

        using ring_type = conditional_t<IsConst, const InplaceRing, InplaceRing>;

    public:
        using value_type = T;
        using difference_type = ptrdiff_t;
        using reference = conditional_t<IsConst, const T&, T&>;
        using pointer = conditional_t<IsConst, const T*, T*>;

        constexpr basic_iterator() noexcept = default;

        template<bool OtherConst>
            requires(IsConst && !OtherConst)
        constexpr basic_iterator(const basic_iterator<OtherConst>& other) noexcept
            : ring_(other.ring_), logical_index_(other.logical_index_) {}

        [[nodiscard]] constexpr reference operator*() const noexcept {
            libk_assert(ring_ != nullptr);
            return (*ring_)[logical_index_];
        }

        [[nodiscard]] constexpr pointer operator->() const noexcept {
            return libk::addressof(operator*());
        }

        constexpr basic_iterator& operator++() noexcept {
            ++logical_index_;
            return *this;
        }

        constexpr basic_iterator operator++(int) noexcept {
            basic_iterator old = *this;
            ++*this;
            return old;
        }

        template<bool OtherConst>
        [[nodiscard]] friend constexpr bool operator==(
            const basic_iterator& lhs,
            const basic_iterator<OtherConst>& rhs) noexcept {
            return lhs.ring_ == rhs.ring_
                && lhs.logical_index_ == rhs.logical_index_;
        }

    private:
        constexpr basic_iterator(ring_type* ring, size_t logical_index) noexcept
            : ring_(ring), logical_index_(logical_index) {}

        ring_type* ring_ = nullptr;
        size_t logical_index_ = 0;
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    constexpr InplaceRing() noexcept = default;

    constexpr InplaceRing(const InplaceRing& other)
        requires is_copy_constructible_v<T> {
        for (const T& value : other) {
            emplace_back(value);
        }
    }

    constexpr InplaceRing(const InplaceRing&)
        requires (!is_copy_constructible_v<T>) = delete;

    constexpr InplaceRing(InplaceRing&& other)
        noexcept(is_nothrow_move_constructible_v<T>)
        requires is_move_constructible_v<T> {
        while (!other.empty()) {
            emplace_back(libk::move(other.front()));
            other.pop_front();
        }
    }

    constexpr InplaceRing(InplaceRing&&)
        requires (!is_move_constructible_v<T>) = delete;

    constexpr InplaceRing& operator=(const InplaceRing& other)
        requires is_copy_constructible_v<T> {
        if (this == &other) {
            return *this;
        }
        clear();
        for (const T& value : other) {
            emplace_back(value);
        }
        return *this;
    }

    constexpr InplaceRing& operator=(const InplaceRing&)
        requires (!is_copy_constructible_v<T>) = delete;

    constexpr InplaceRing& operator=(InplaceRing&& other)
        noexcept(is_nothrow_move_constructible_v<T>)
        requires is_move_constructible_v<T> {
        if (this == &other) {
            return *this;
        }
        clear();
        while (!other.empty()) {
            emplace_back(libk::move(other.front()));
            other.pop_front();
        }
        return *this;
    }

    constexpr InplaceRing& operator=(InplaceRing&&)
        requires (!is_move_constructible_v<T>) = delete;

    constexpr ~InplaceRing() {
        clear();
    }

    [[nodiscard]] constexpr size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ == Capacity; }

    [[nodiscard]] constexpr T& front() noexcept {
        libk_assert(!empty());
        return slot_at(head_).value_;
    }

    [[nodiscard]] constexpr const T& front() const noexcept {
        libk_assert(!empty());
        return slot_at(head_).value_;
    }

    [[nodiscard]] constexpr T& back() noexcept {
        libk_assert(!empty());
        return slot_at(physical_index(size_ - 1)).value_;
    }

    [[nodiscard]] constexpr const T& back() const noexcept {
        libk_assert(!empty());
        return slot_at(physical_index(size_ - 1)).value_;
    }

    [[nodiscard]] constexpr T& operator[](size_t logical_index) noexcept {
        libk_assert(logical_index < size_);
        return slot_at(physical_index(logical_index)).value_;
    }

    [[nodiscard]] constexpr const T& operator[](size_t logical_index) const noexcept {
        libk_assert(logical_index < size_);
        return slot_at(physical_index(logical_index)).value_;
    }

    template<typename... Args>
        requires ConstructibleFrom<T, Args&&...>
    [[nodiscard]] constexpr T* try_emplace_back(Args&&... args)
        noexcept(is_nothrow_constructible_v<T, Args&&...>) {
        if (full()) {
            return nullptr;
        }
        const size_t index = physical_index(size_);
        T* value = libk::construct_at(
            libk::addressof(slot_at(index).value_),
            libk::forward<Args>(args)...);
        ++size_;
        return value;
    }

    template<typename... Args>
        requires ConstructibleFrom<T, Args&&...>
    constexpr T& emplace_back(Args&&... args)
        noexcept(is_nothrow_constructible_v<T, Args&&...>) {
        T* value = try_emplace_back(libk::forward<Args>(args)...);
        libk_assert(value != nullptr);
        return *value;
    }

    template<typename U>
        requires ConstructibleFrom<T, U&&>
    [[nodiscard]] constexpr bool try_push_back(U&& value)
        noexcept(is_nothrow_constructible_v<T, U&&>) {
        return try_emplace_back(libk::forward<U>(value)) != nullptr;
    }

    template<typename... Args>
        requires ConstructibleFrom<T, Args&&...>
    [[nodiscard]] constexpr T* try_emplace_front(Args&&... args)
        noexcept(is_nothrow_constructible_v<T, Args&&...>) {
        if (full()) {
            return nullptr;
        }
        const size_t new_head = decrement(head_);
        T* value = libk::construct_at(
            libk::addressof(slot_at(new_head).value_),
            libk::forward<Args>(args)...);
        head_ = new_head;
        ++size_;
        return value;
    }

    template<typename... Args>
        requires ConstructibleFrom<T, Args&&...>
    constexpr T& emplace_front(Args&&... args)
        noexcept(is_nothrow_constructible_v<T, Args&&...>) {
        T* value = try_emplace_front(libk::forward<Args>(args)...);
        libk_assert(value != nullptr);
        return *value;
    }

    template<typename U>
        requires ConstructibleFrom<T, U&&>
    [[nodiscard]] constexpr bool try_push_front(U&& value)
        noexcept(is_nothrow_constructible_v<T, U&&>) {
        return try_emplace_front(libk::forward<U>(value)) != nullptr;
    }

    constexpr void pop_front() noexcept {
        libk_assert(!empty());
        libk::destroy_at(libk::addressof(slot_at(head_).value_));
        head_ = increment(head_);
        --size_;
        if (size_ == 0) {
            head_ = 0;
        }
    }

    [[nodiscard]] constexpr bool try_pop_front() noexcept {
        if (empty()) {
            return false;
        }
        pop_front();
        return true;
    }

    template<typename U>
        requires AssignableFrom<U&, T&&>
    [[nodiscard]] constexpr bool try_pop_front(U& output)
        noexcept(is_nothrow_assignable_v<U&, T&&>) {
        if (empty()) {
            return false;
        }
        output = libk::move(front());
        pop_front();
        return true;
    }

    constexpr void pop_back() noexcept {
        libk_assert(!empty());
        const size_t index = physical_index(size_ - 1);
        libk::destroy_at(libk::addressof(slot_at(index).value_));
        --size_;
        if (size_ == 0) {
            head_ = 0;
        }
    }

    [[nodiscard]] constexpr bool try_pop_back() noexcept {
        if (empty()) {
            return false;
        }
        pop_back();
        return true;
    }

    template<typename U>
        requires AssignableFrom<U&, T&&>
    [[nodiscard]] constexpr bool try_pop_back(U& output)
        noexcept(is_nothrow_assignable_v<U&, T&&>) {
        if (empty()) {
            return false;
        }
        output = libk::move(back());
        pop_back();
        return true;
    }

    constexpr void clear() noexcept {
        while (!empty()) {
            pop_back();
        }
    }

    [[nodiscard]] constexpr iterator begin() noexcept {
        return iterator(this, 0);
    }

    [[nodiscard]] constexpr const_iterator begin() const noexcept {
        return const_iterator(this, 0);
    }

    [[nodiscard]] constexpr const_iterator cbegin() const noexcept {
        return const_iterator(this, 0);
    }

    [[nodiscard]] constexpr iterator end() noexcept {
        return iterator(this, size_);
    }

    [[nodiscard]] constexpr const_iterator end() const noexcept {
        return const_iterator(this, size_);
    }

    [[nodiscard]] constexpr const_iterator cend() const noexcept {
        return const_iterator(this, size_);
    }

private:
    [[nodiscard]] static constexpr size_t wrap(size_t value) noexcept {
        if constexpr (power_of_two_) {
            return value & (Capacity - 1);
        } else {
            return value < Capacity ? value : value % Capacity;
        }
    }

    [[nodiscard]] static constexpr size_t increment(size_t value) noexcept {
        return value + 1 == Capacity ? 0 : value + 1;
    }

    [[nodiscard]] static constexpr size_t decrement(size_t value) noexcept {
        return value == 0 ? Capacity - 1 : value - 1;
    }

    [[nodiscard]] constexpr size_t physical_index(size_t logical_index) const noexcept {
        return wrap(head_ + logical_index);
    }

    [[nodiscard]] constexpr slot& slot_at(size_t index) noexcept {
        return slots_[index];
    }

    [[nodiscard]] constexpr const slot& slot_at(size_t index) const noexcept {
        return slots_[index];
    }

    slot slots_[Capacity]{};
    size_t head_ = 0;
    size_t size_ = 0;
};

} // namespace libk
