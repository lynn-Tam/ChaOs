#pragma once

// Compact member-hook intrusive doubly-linked list.
// Each hook is two pointers by default; define LIBK_INTRUSIVE_LIST_DEBUG_OWNER=1
// consistently across translation units to add owner validation. Hooks never auto-unlink;
// copying/moving/destroying a linked hook is rejected. The list itself unlinks nodes on clear/destruction.

#include <stddef.h>

#include <libk/assert.hpp>
#include <libk/typetraits.hpp>
#include <libk/utility.hpp>

#ifndef LIBK_INTRUSIVE_LIST_DEBUG_OWNER
#define LIBK_INTRUSIVE_LIST_DEBUG_OWNER 0
#endif

namespace libk {

class IntrusiveListHook;

template<typename T, IntrusiveListHook T::* HookMember>
class IntrusiveList;

class IntrusiveListHook {
    template<typename T, IntrusiveListHook T::* HookMember>
    friend class IntrusiveList;

public:
    constexpr IntrusiveListHook() noexcept = default;

    constexpr IntrusiveListHook(const IntrusiveListHook& other) noexcept {
        libk_assert(!other.is_linked());
    }

    constexpr IntrusiveListHook(IntrusiveListHook&& other) noexcept {
        libk_assert(!other.is_linked());
    }

    constexpr IntrusiveListHook& operator=(const IntrusiveListHook& other) noexcept {
        libk_assert(!is_linked());
        libk_assert(!other.is_linked());
        return *this;
    }

    constexpr IntrusiveListHook& operator=(IntrusiveListHook&& other) noexcept {
        libk_assert(!is_linked());
        libk_assert(!other.is_linked());
        return *this;
    }

    constexpr ~IntrusiveListHook() {
        libk_assert(!is_linked());
    }

    [[nodiscard]] constexpr bool is_linked() const noexcept {
        return next_ != nullptr;
    }

private:
    constexpr void initialize_sentinel(const void* owner) noexcept {
        previous_ = this;
        next_ = this;
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        owner_ = owner;
#else
        (void)owner;
#endif
    }

    constexpr void reset_unlinked() noexcept {
        previous_ = nullptr;
        next_ = nullptr;
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        owner_ = nullptr;
#endif
    }

    IntrusiveListHook* previous_ = nullptr;
    IntrusiveListHook* next_ = nullptr;
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
    const void* owner_ = nullptr;
#endif
};

template<typename T, IntrusiveListHook T::* HookMember>
class IntrusiveList {
    static_assert(is_object_v<T> && !is_const_v<T>,
                  "IntrusiveList requires a non-const object type");

    static constexpr ptrdiff_t unknown_offset_ = static_cast<ptrdiff_t>(-1);

public:
    template<bool IsConst>
    class basic_iterator {
        friend class IntrusiveList;
        template<bool>
        friend class basic_iterator;

    public:
        using value_type = T;
        using difference_type = ptrdiff_t;
        using reference = conditional_t<IsConst, const T&, T&>;
        using pointer = conditional_t<IsConst, const T*, T*>;

        constexpr basic_iterator() noexcept = default;

        template<bool OtherConst>
            requires(IsConst && !OtherConst)
        constexpr basic_iterator(const basic_iterator<OtherConst>& other) noexcept
            : hook_(other.hook_), hook_offset_(other.hook_offset_) {}

        [[nodiscard]] constexpr reference operator*() const noexcept {
            libk_assert(hook_ != nullptr);
            using byte_pointer = conditional_t<IsConst, const unsigned char*, unsigned char*>;
            byte_pointer hook_bytes = reinterpret_cast<byte_pointer>(hook_);
            byte_pointer object_bytes = hook_bytes - hook_offset_;
            return *reinterpret_cast<pointer>(object_bytes);
        }

        [[nodiscard]] constexpr pointer operator->() const noexcept {
            return libk::addressof(operator*());
        }

        constexpr basic_iterator& operator++() noexcept {
            libk_assert(hook_ != nullptr);
            hook_ = hook_->next_;
            return *this;
        }

        constexpr basic_iterator operator++(int) noexcept {
            basic_iterator old = *this;
            ++*this;
            return old;
        }

        constexpr basic_iterator& operator--() noexcept {
            libk_assert(hook_ != nullptr);
            hook_ = hook_->previous_;
            return *this;
        }

        constexpr basic_iterator operator--(int) noexcept {
            basic_iterator old = *this;
            --*this;
            return old;
        }

        template<bool OtherConst>
        [[nodiscard]] friend constexpr bool operator==(
            const basic_iterator& lhs,
            const basic_iterator<OtherConst>& rhs) noexcept {
            return lhs.hook_ == rhs.hook_;
        }

    private:
        using hook_pointer = conditional_t<IsConst, const IntrusiveListHook*, IntrusiveListHook*>;

        constexpr basic_iterator(hook_pointer hook, ptrdiff_t hook_offset) noexcept
            : hook_(hook), hook_offset_(hook_offset) {}

        hook_pointer hook_ = nullptr;
        ptrdiff_t hook_offset_ = 0;
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    constexpr IntrusiveList() noexcept {
        sentinel_.initialize_sentinel(this);
    }

    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    constexpr IntrusiveList(IntrusiveList&& other) noexcept {
        sentinel_.initialize_sentinel(this);
        steal_from(other);
    }

    constexpr IntrusiveList& operator=(IntrusiveList&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        clear();
        steal_from(other);
        return *this;
    }

    constexpr ~IntrusiveList() {
        clear();
        sentinel_.reset_unlinked();
    }

    [[nodiscard]] constexpr size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr T& front() noexcept {
        libk_assert(!empty());
        return value_from_hook(*sentinel_.next_);
    }

    [[nodiscard]] constexpr const T& front() const noexcept {
        libk_assert(!empty());
        return value_from_hook(*sentinel_.next_);
    }

    [[nodiscard]] constexpr T& back() noexcept {
        libk_assert(!empty());
        return value_from_hook(*sentinel_.previous_);
    }

    [[nodiscard]] constexpr const T& back() const noexcept {
        libk_assert(!empty());
        return value_from_hook(*sentinel_.previous_);
    }

    [[nodiscard]] constexpr iterator begin() noexcept {
        return iterator(sentinel_.next_, iterator_offset());
    }

    [[nodiscard]] constexpr const_iterator begin() const noexcept {
        return const_iterator(sentinel_.next_, iterator_offset());
    }

    [[nodiscard]] constexpr const_iterator cbegin() const noexcept {
        return const_iterator(sentinel_.next_, iterator_offset());
    }

    [[nodiscard]] constexpr iterator end() noexcept {
        return iterator(&sentinel_, iterator_offset());
    }

    [[nodiscard]] constexpr const_iterator end() const noexcept {
        return const_iterator(&sentinel_, iterator_offset());
    }

    [[nodiscard]] constexpr const_iterator cend() const noexcept {
        return const_iterator(&sentinel_, iterator_offset());
    }

    [[nodiscard]] constexpr iterator iterator_to(T& value) noexcept {
        IntrusiveListHook& hook = hook_of(value);
        establish_offset(value);
        assert_owned_or_unlinked(hook, false);
        libk_assert(hook.is_linked());
        return iterator(&hook, hook_offset_);
    }

    [[nodiscard]] constexpr const_iterator iterator_to(const T& value) const noexcept {
        const IntrusiveListHook& hook = hook_of(value);
        libk_assert(hook_offset_ != unknown_offset_);
        assert_owned_or_unlinked(hook, false);
        libk_assert(hook.is_linked());
        return const_iterator(&hook, hook_offset_);
    }

    constexpr iterator insert(iterator position, T& value) noexcept {
        IntrusiveListHook& hook = hook_of(value);
        establish_offset(value);
        assert_position(position);
        libk_assert(!hook.is_linked());

        link_before(*position.hook_, hook);
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        hook.owner_ = this;
#endif
        ++size_;
        return iterator(&hook, hook_offset_);
    }

    constexpr void push_front(T& value) noexcept {
        (void)insert(begin(), value);
    }

    constexpr void push_back(T& value) noexcept {
        (void)insert(end(), value);
    }

    constexpr iterator erase(iterator position) noexcept {
        assert_erasable(position);
        IntrusiveListHook* next = position.hook_->next_;
        unlink(*position.hook_);
        --size_;
        return iterator(next, iterator_offset());
    }

    constexpr void erase(T& value) noexcept {
        (void)erase(iterator_to(value));
    }

    [[nodiscard]] constexpr T& pop_front() noexcept {
        libk_assert(!empty());
        T& value = front();
        erase(value);
        return value;
    }

    [[nodiscard]] constexpr T& pop_back() noexcept {
        libk_assert(!empty());
        T& value = back();
        erase(value);
        return value;
    }

    constexpr void clear() noexcept {
        IntrusiveListHook* current = sentinel_.next_;
        while (current != &sentinel_) {
            IntrusiveListHook* next = current->next_;
            current->reset_unlinked();
            current = next;
        }
        sentinel_.initialize_sentinel(this);
        size_ = 0;
    }

    constexpr void splice(iterator position, IntrusiveList& other) noexcept {
        assert_position(position);
        if (other.empty() || this == &other) {
            return;
        }

        adopt_offset(other);
        IntrusiveListHook* first = other.sentinel_.next_;
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        IntrusiveListHook* last = other.sentinel_.previous_;
#endif
        transfer_before(*position.hook_, *first, other.sentinel_);

        size_ += other.size_;
        other.sentinel_.initialize_sentinel(&other);
        other.size_ = 0;
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        update_owner_range(*first, *last, this);
#endif
    }

    constexpr void splice(
        iterator position,
        IntrusiveList& other,
        iterator element) noexcept {
        assert_position(position);
        other.assert_erasable(element);

        IntrusiveListHook* next = element.hook_->next_;
        if (this == &other
            && (position.hook_ == element.hook_ || position.hook_ == next)) {
            return;
        }

        adopt_offset(other);
        transfer_before(*position.hook_, *element.hook_, *next);
        if (this != &other) {
            ++size_;
            --other.size_;
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
            element.hook_->owner_ = this;
#endif
        }
    }

    constexpr void splice(
        iterator position,
        IntrusiveList& other,
        iterator first,
        iterator last) noexcept {
        size_t count = 0;
        for (iterator current = first; current != last; ++current) {
            ++count;
        }
        splice(position, other, first, last, count);
    }

    constexpr void splice(
        iterator position,
        IntrusiveList& other,
        iterator first,
        iterator last,
        size_t count) noexcept {
        assert_position(position);
        other.assert_position(first);
        other.assert_position(last);
        libk_assert(first.hook_ != nullptr && last.hook_ != nullptr);
        if (first == last) {
            libk_assert(count == 0);
            return;
        }
        libk_assert(count > 0);
        libk_assert(first.hook_ != &other.sentinel_);

#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        size_t actual_count = 0;
        for (IntrusiveListHook* current = first.hook_;
             current != last.hook_;
             current = current->next_) {
            libk_assert(current != &other.sentinel_);
            libk_assert(current->owner_ == &other);
            ++actual_count;
        }
        libk_assert(actual_count == count);
#endif

        adopt_offset(other);
        IntrusiveListHook* first_hook = first.hook_;
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        IntrusiveListHook* last_moved = last.hook_->previous_;
#endif

        if (this == &other) {
            if (position == first || position == last) {
                return;
            }
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
            for (IntrusiveListHook* current = first_hook;
                 current != last.hook_;
                 current = current->next_) {
                libk_assert(current != position.hook_);
            }
#endif
        } else {
            libk_assert(count <= other.size_);
        }

        transfer_before(*position.hook_, *first_hook, *last.hook_);
        if (this != &other) {
            size_ += count;
            other.size_ -= count;
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
            update_owner_range(*first_hook, *last_moved, this);
#endif
        }
    }

private:
    [[nodiscard]] static constexpr IntrusiveListHook& hook_of(T& value) noexcept {
        return value.*HookMember;
    }

    [[nodiscard]] static constexpr const IntrusiveListHook& hook_of(const T& value) noexcept {
        return value.*HookMember;
    }

    constexpr void establish_offset(T& value) noexcept {
        auto* object_bytes = reinterpret_cast<unsigned char*>(libk::addressof(value));
        auto* hook_bytes = reinterpret_cast<unsigned char*>(libk::addressof(hook_of(value)));
        const ptrdiff_t offset = hook_bytes - object_bytes;
        if (hook_offset_ == unknown_offset_) {
            hook_offset_ = offset;
        } else {
            libk_assert(hook_offset_ == offset);
        }
    }

    constexpr void adopt_offset(const IntrusiveList& other) noexcept {
        if (other.hook_offset_ == unknown_offset_) {
            return;
        }
        if (hook_offset_ == unknown_offset_) {
            hook_offset_ = other.hook_offset_;
        } else {
            libk_assert(hook_offset_ == other.hook_offset_);
        }
    }

    [[nodiscard]] constexpr ptrdiff_t iterator_offset() const noexcept {
        return hook_offset_ == unknown_offset_ ? 0 : hook_offset_;
    }

    [[nodiscard]] constexpr T& value_from_hook(IntrusiveListHook& hook) noexcept {
        libk_assert(hook_offset_ != unknown_offset_);
        auto* hook_bytes = reinterpret_cast<unsigned char*>(libk::addressof(hook));
        return *reinterpret_cast<T*>(hook_bytes - hook_offset_);
    }

    [[nodiscard]] constexpr const T& value_from_hook(
        const IntrusiveListHook& hook) const noexcept {
        libk_assert(hook_offset_ != unknown_offset_);
        auto* hook_bytes = reinterpret_cast<const unsigned char*>(libk::addressof(hook));
        return *reinterpret_cast<const T*>(hook_bytes - hook_offset_);
    }

    constexpr void assert_position(iterator position) const noexcept {
        libk_assert(position.hook_ != nullptr);
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        libk_assert(position.hook_ == &sentinel_ || position.hook_->owner_ == this);
#endif
    }

    constexpr void assert_erasable(iterator position) const noexcept {
        assert_position(position);
        libk_assert(position.hook_ != &sentinel_);
        libk_assert(position.hook_->is_linked());
    }

    constexpr void assert_owned_or_unlinked(
        const IntrusiveListHook& hook,
        bool allow_unlinked) const noexcept {
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        if (hook.is_linked()) {
            libk_assert(hook.owner_ == this);
        } else {
            libk_assert(allow_unlinked);
        }
#else
        (void)hook;
        (void)allow_unlinked;
#endif
    }

    static constexpr void link_before(
        IntrusiveListHook& position,
        IntrusiveListHook& node) noexcept {
        IntrusiveListHook* previous = position.previous_;
        node.previous_ = previous;
        node.next_ = &position;
        previous->next_ = &node;
        position.previous_ = &node;
    }

    static constexpr void unlink(IntrusiveListHook& node) noexcept {
        IntrusiveListHook* previous = node.previous_;
        IntrusiveListHook* next = node.next_;
        previous->next_ = next;
        next->previous_ = previous;
        node.reset_unlinked();
    }

    static constexpr void transfer_before(
        IntrusiveListHook& position,
        IntrusiveListHook& first,
        IntrusiveListHook& last_exclusive) noexcept {
        if (&first == &last_exclusive || &position == &last_exclusive) {
            return;
        }

        IntrusiveListHook* before_first = first.previous_;
        IntrusiveListHook* last = last_exclusive.previous_;
        before_first->next_ = &last_exclusive;
        last_exclusive.previous_ = before_first;

        IntrusiveListHook* before_position = position.previous_;
        before_position->next_ = &first;
        first.previous_ = before_position;
        last->next_ = &position;
        position.previous_ = last;
    }

#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
    static constexpr void update_owner_range(
        IntrusiveListHook& first,
        IntrusiveListHook& last,
        const void* owner) noexcept {
        IntrusiveListHook* current = &first;
        for (;;) {
            current->owner_ = owner;
            if (current == &last) {
                break;
            }
            current = current->next_;
        }
    }
#endif

    constexpr void steal_from(IntrusiveList& other) noexcept {
        hook_offset_ = other.hook_offset_;
        if (other.empty()) {
            size_ = 0;
            return;
        }

        sentinel_.next_ = other.sentinel_.next_;
        sentinel_.previous_ = other.sentinel_.previous_;
        sentinel_.next_->previous_ = &sentinel_;
        sentinel_.previous_->next_ = &sentinel_;
        size_ = other.size_;

        other.sentinel_.initialize_sentinel(&other);
        other.size_ = 0;
        other.hook_offset_ = unknown_offset_;
#if LIBK_INTRUSIVE_LIST_DEBUG_OWNER
        update_owner_range(*sentinel_.next_, *sentinel_.previous_, this);
#endif
    }

    IntrusiveListHook sentinel_{};
    size_t size_ = 0;
    ptrdiff_t hook_offset_ = unknown_offset_;
};

} // namespace libk
