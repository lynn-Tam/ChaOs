#pragma once

// Allocation-free intrusive AVL tree with a member hook.
// The container owns ordering and membership only; values own their storage.
// minimum() is O(1), insert()/erase() are O(log N). A hook may belong to at
// most one tree and must be unlinked before its value is destroyed.

#include <stddef.h>

#include <libk/assert.hpp>
#include <libk/typetraits.hpp>

namespace libk {

class IntrusiveTreeHook;

template<typename T, IntrusiveTreeHook T::* HookMember, typename Compare>
class IntrusiveTree;

class IntrusiveTreeHook {
    template<typename T, IntrusiveTreeHook T::* HookMember, typename Compare>
    friend class IntrusiveTree;

public:
    constexpr IntrusiveTreeHook() noexcept = default;
    IntrusiveTreeHook(const IntrusiveTreeHook&) = delete;
    auto operator=(const IntrusiveTreeHook&) -> IntrusiveTreeHook& = delete;
    IntrusiveTreeHook(IntrusiveTreeHook&&) = delete;
    auto operator=(IntrusiveTreeHook&&) -> IntrusiveTreeHook& = delete;

    constexpr ~IntrusiveTreeHook() { libk_assert(!linked_); }

    [[nodiscard]] constexpr auto is_linked() const noexcept -> bool {
        return linked_;
    }

private:
    IntrusiveTreeHook* parent_{};
    IntrusiveTreeHook* left_{};
    IntrusiveTreeHook* right_{};
    unsigned height_{1};
    bool linked_{};
};

template<typename T, IntrusiveTreeHook T::* HookMember, typename Compare>
class IntrusiveTree {
    static_assert(is_object_v<T> && !is_const_v<T>);

public:
    constexpr IntrusiveTree() noexcept = default;
    IntrusiveTree(const IntrusiveTree&) = delete;
    auto operator=(const IntrusiveTree&) -> IntrusiveTree& = delete;
    IntrusiveTree(IntrusiveTree&&) = delete;
    auto operator=(IntrusiveTree&&) -> IntrusiveTree& = delete;

    constexpr ~IntrusiveTree() { clear(); }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return root_ == nullptr;
    }
    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return size_;
    }
    [[nodiscard]] constexpr auto minimum() noexcept -> T* {
        return minimum_ == nullptr ? nullptr : value_of(*minimum_);
    }
    [[nodiscard]] constexpr auto minimum() const noexcept -> const T* {
        return minimum_ == nullptr ? nullptr : value_of(*minimum_);
    }
    [[nodiscard]] constexpr auto maximum() noexcept -> T* {
        return const_cast<T*>(
            static_cast<const IntrusiveTree*>(this)->maximum());
    }
    [[nodiscard]] constexpr auto maximum() const noexcept -> const T* {
        if (root_ == nullptr) {
            return nullptr;
        }
        const IntrusiveTreeHook* node = root_;
        while (node->right_ != nullptr) {
            node = node->right_;
        }
        return value_of(*node);
    }

    template<typename Key>
    [[nodiscard]] constexpr auto find(const Key& key) noexcept -> T* {
        return const_cast<T*>(static_cast<const IntrusiveTree*>(this)->find(key));
    }

    template<typename Key>
    [[nodiscard]] constexpr auto find(const Key& key) const noexcept
        -> const T* {
        const IntrusiveTreeHook* node = root_;
        while (node != nullptr) {
            const T& value = *value_of(*node);
            if (compare_(key, value)) {
                node = node->left_;
            } else if (compare_(value, key)) {
                node = node->right_;
            } else {
                return &value;
            }
        }
        return nullptr;
    }

    template<typename Key>
    [[nodiscard]] constexpr auto lower_bound(const Key& key) noexcept -> T* {
        return const_cast<T*>(
            static_cast<const IntrusiveTree*>(this)->lower_bound(key));
    }

    template<typename Key>
    [[nodiscard]] constexpr auto lower_bound(const Key& key) const noexcept
        -> const T* {
        const IntrusiveTreeHook* node = root_;
        const IntrusiveTreeHook* candidate{};
        while (node != nullptr) {
            const T& value = *value_of(*node);
            if (!compare_(value, key)) {
                candidate = node;
                node = node->left_;
            } else {
                node = node->right_;
            }
        }
        return candidate == nullptr ? nullptr : value_of(*candidate);
    }

    [[nodiscard]] constexpr auto next(T& value) noexcept -> T* {
        return const_cast<T*>(
            static_cast<const IntrusiveTree*>(this)->next(value));
    }

    [[nodiscard]] constexpr auto next(const T& value) const noexcept
        -> const T* {
        const IntrusiveTreeHook* node = &hook_of(value);
        libk_assert(node->linked_);
        if (node->right_ != nullptr) {
            return value_of(*leftmost(*node->right_));
        }
        const IntrusiveTreeHook* parent = node->parent_;
        while (parent != nullptr && node == parent->right_) {
            node = parent;
            parent = parent->parent_;
        }
        return parent == nullptr ? nullptr : value_of(*parent);
    }

    [[nodiscard]] constexpr auto previous(T& value) noexcept -> T* {
        return const_cast<T*>(
            static_cast<const IntrusiveTree*>(this)->previous(value));
    }

    [[nodiscard]] constexpr auto previous(const T& value) const noexcept
        -> const T* {
        const IntrusiveTreeHook* node = &hook_of(value);
        libk_assert(node->linked_);
        if (node->left_ != nullptr) {
            const IntrusiveTreeHook* result = node->left_;
            while (result->right_ != nullptr) {
                result = result->right_;
            }
            return value_of(*result);
        }
        const IntrusiveTreeHook* parent = node->parent_;
        while (parent != nullptr && node == parent->left_) {
            node = parent;
            parent = parent->parent_;
        }
        return parent == nullptr ? nullptr : value_of(*parent);
    }

    constexpr void insert(T& value) noexcept {
        IntrusiveTreeHook& node = hook_of(value);
        libk_assert(!node.linked_);
        establish_offset(value);

        IntrusiveTreeHook* parent{};
        IntrusiveTreeHook** edge = &root_;
        while (*edge != nullptr) {
            parent = *edge;
            T& existing = *value_of(**edge);
            if (compare_(value, existing)) {
                edge = &parent->left_;
            } else {
                libk_assert(compare_(existing, value));
                edge = &parent->right_;
            }
        }

        node.parent_ = parent;
        node.left_ = nullptr;
        node.right_ = nullptr;
        node.height_ = 1;
        node.linked_ = true;
        *edge = &node;
        if (minimum_ == nullptr || compare_(value, *value_of(*minimum_))) {
            minimum_ = &node;
        }
        ++size_;
        rebalance(parent);
    }

    constexpr void erase(T& value) noexcept {
        IntrusiveTreeHook& node = hook_of(value);
        libk_assert(node.linked_);

        IntrusiveTreeHook* rebalance_from{};
        if (node.left_ == nullptr || node.right_ == nullptr) {
            IntrusiveTreeHook* child = node.left_ != nullptr
                ? node.left_
                : node.right_;
            rebalance_from = node.parent_;
            replace(node, child);
        } else {
            IntrusiveTreeHook* successor = leftmost(*node.right_);
            if (successor->parent_ != &node) {
                IntrusiveTreeHook* const old_parent = successor->parent_;
                replace(*successor, successor->right_);
                successor->right_ = node.right_;
                successor->right_->parent_ = successor;
                rebalance_from = old_parent;
            } else {
                rebalance_from = successor;
            }
            replace(node, successor);
            successor->left_ = node.left_;
            successor->left_->parent_ = successor;
            update_height(*successor);
        }

        node.parent_ = nullptr;
        node.left_ = nullptr;
        node.right_ = nullptr;
        node.height_ = 1;
        node.linked_ = false;
        libk_assert(size_ != 0);
        --size_;
        minimum_ = root_ == nullptr ? nullptr : leftmost(*root_);
        rebalance(rebalance_from);
    }

    constexpr void clear() noexcept {
        while (minimum_ != nullptr) {
            erase(*value_of(*minimum_));
        }
    }

private:
    static constexpr auto hook_of(T& value) noexcept -> IntrusiveTreeHook& {
        return value.*HookMember;
    }
    static constexpr auto hook_of(const T& value) noexcept
        -> const IntrusiveTreeHook& {
        return value.*HookMember;
    }

    constexpr void establish_offset(T& value) noexcept {
        const auto object = reinterpret_cast<const unsigned char*>(&value);
        const auto hook = reinterpret_cast<const unsigned char*>(&hook_of(value));
        const ptrdiff_t offset = hook - object;
        if (!offset_known_) {
            hook_offset_ = offset;
            offset_known_ = true;
        } else {
            libk_assert(hook_offset_ == offset);
        }
    }

    [[nodiscard]] constexpr auto value_of(IntrusiveTreeHook& hook) noexcept
        -> T* {
        libk_assert(offset_known_);
        auto* bytes = reinterpret_cast<unsigned char*>(&hook);
        return reinterpret_cast<T*>(bytes - hook_offset_);
    }
    [[nodiscard]] constexpr auto value_of(
        const IntrusiveTreeHook& hook) const noexcept -> const T* {
        libk_assert(offset_known_);
        auto* bytes = reinterpret_cast<const unsigned char*>(&hook);
        return reinterpret_cast<const T*>(bytes - hook_offset_);
    }

    [[nodiscard]] static constexpr auto height(
        const IntrusiveTreeHook* node) noexcept -> int {
        return node == nullptr ? 0 : static_cast<int>(node->height_);
    }
    static constexpr void update_height(IntrusiveTreeHook& node) noexcept {
        const int left = height(node.left_);
        const int right = height(node.right_);
        node.height_ = static_cast<unsigned>(
            1 + (left > right ? left : right));
    }
    [[nodiscard]] static constexpr auto balance(
        const IntrusiveTreeHook& node) noexcept -> int {
        return height(node.left_) - height(node.right_);
    }
    [[nodiscard]] static constexpr auto leftmost(
        IntrusiveTreeHook& node) noexcept -> IntrusiveTreeHook* {
        IntrusiveTreeHook* result = &node;
        while (result->left_ != nullptr) {
            result = result->left_;
        }
        return result;
    }

    constexpr void replace(
        IntrusiveTreeHook& old_node,
        IntrusiveTreeHook* replacement) noexcept {
        if (old_node.parent_ == nullptr) {
            root_ = replacement;
        } else if (old_node.parent_->left_ == &old_node) {
            old_node.parent_->left_ = replacement;
        } else {
            libk_assert(old_node.parent_->right_ == &old_node);
            old_node.parent_->right_ = replacement;
        }
        if (replacement != nullptr) {
            replacement->parent_ = old_node.parent_;
        }
    }

    constexpr auto rotate_left(IntrusiveTreeHook& root) noexcept
        -> IntrusiveTreeHook* {
        IntrusiveTreeHook* const pivot = root.right_;
        libk_assert(pivot != nullptr);
        IntrusiveTreeHook* const parent = root.parent_;
        root.right_ = pivot->left_;
        if (root.right_ != nullptr) {
            root.right_->parent_ = &root;
        }
        pivot->left_ = &root;
        root.parent_ = pivot;
        pivot->parent_ = parent;
        if (parent == nullptr) {
            root_ = pivot;
        } else if (parent->left_ == &root) {
            parent->left_ = pivot;
        } else {
            parent->right_ = pivot;
        }
        update_height(root);
        update_height(*pivot);
        return pivot;
    }

    constexpr auto rotate_right(IntrusiveTreeHook& root) noexcept
        -> IntrusiveTreeHook* {
        IntrusiveTreeHook* const pivot = root.left_;
        libk_assert(pivot != nullptr);
        IntrusiveTreeHook* const parent = root.parent_;
        root.left_ = pivot->right_;
        if (root.left_ != nullptr) {
            root.left_->parent_ = &root;
        }
        pivot->right_ = &root;
        root.parent_ = pivot;
        pivot->parent_ = parent;
        if (parent == nullptr) {
            root_ = pivot;
        } else if (parent->left_ == &root) {
            parent->left_ = pivot;
        } else {
            parent->right_ = pivot;
        }
        update_height(root);
        update_height(*pivot);
        return pivot;
    }

    constexpr void rebalance(IntrusiveTreeHook* node) noexcept {
        while (node != nullptr) {
            update_height(*node);
            const int factor = balance(*node);
            if (factor > 1) {
                if (balance(*node->left_) < 0) {
                    (void)rotate_left(*node->left_);
                }
                node = rotate_right(*node);
            } else if (factor < -1) {
                if (balance(*node->right_) > 0) {
                    (void)rotate_right(*node->right_);
                }
                node = rotate_left(*node);
            }
            node = node->parent_;
        }
    }

    IntrusiveTreeHook* root_{};
    IntrusiveTreeHook* minimum_{};
    size_t size_{};
    ptrdiff_t hook_offset_{};
    bool offset_known_{};
    [[no_unique_address]] Compare compare_{};
};

} // namespace libk
