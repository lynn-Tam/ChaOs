#pragma once

// Allocation-free intrusive AVL tree with a member hook.
// The container owns ordering and membership only; values own their storage.
// minimum() is O(1), insert()/erase() are O(log N). A hook may belong to at
// most one tree and must be unlinked before its value is destroyed.

#include <stddef.h>

#include <libk/assert.hpp>
#include <type_traits>
#include <utility>

namespace libk {

class intrusive_tree_hook;

template<typename T, intrusive_tree_hook T::* HookMember, typename Compare>
class intrusive_tree;

class intrusive_tree_hook {
    template<typename T, intrusive_tree_hook T::* HookMember, typename Compare>
    friend class intrusive_tree;

public:
    constexpr intrusive_tree_hook() noexcept = default;
    intrusive_tree_hook(const intrusive_tree_hook&) = delete;
    auto operator=(const intrusive_tree_hook&) -> intrusive_tree_hook& = delete;
    intrusive_tree_hook(intrusive_tree_hook&&) = delete;
    auto operator=(intrusive_tree_hook&&) -> intrusive_tree_hook& = delete;

    constexpr ~intrusive_tree_hook() { libk_assert(!linked_); }

    [[nodiscard]] constexpr auto is_linked() const noexcept -> bool {
        return linked_;
    }

private:
    intrusive_tree_hook* parent_{};
    intrusive_tree_hook* left_{};
    intrusive_tree_hook* right_{};
    unsigned height_{1};
    bool linked_{};
};

template<typename T, intrusive_tree_hook T::* HookMember, typename Compare>
class intrusive_tree {
    static_assert(std::is_object_v<T> && !std::is_const_v<T>);

public:
    constexpr intrusive_tree() noexcept = default;
    intrusive_tree(const intrusive_tree&) = delete;
    auto operator=(const intrusive_tree&) -> intrusive_tree& = delete;
    constexpr intrusive_tree(intrusive_tree&& other)
        noexcept(std::is_nothrow_move_constructible_v<Compare>)
        : root_(std::exchange(other.root_, nullptr)),
          minimum_(std::exchange(other.minimum_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          hook_offset_(std::exchange(other.hook_offset_, 0)),
          offset_known_(std::exchange(other.offset_known_, false)),
          compare_(std::move(other.compare_)) {}
    constexpr auto operator=(intrusive_tree&& other)
        noexcept(std::is_nothrow_move_assignable_v<Compare>) -> intrusive_tree& {
        if (this != &other) {
            clear();
            root_ = std::exchange(other.root_, nullptr);
            minimum_ = std::exchange(other.minimum_, nullptr);
            size_ = std::exchange(other.size_, 0);
            hook_offset_ = std::exchange(other.hook_offset_, 0);
            offset_known_ = std::exchange(other.offset_known_, false);
            compare_ = std::move(other.compare_);
        }
        return *this;
    }

    constexpr ~intrusive_tree() { clear(); }

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
            static_cast<const intrusive_tree*>(this)->maximum());
    }
    [[nodiscard]] constexpr auto maximum() const noexcept -> const T* {
        if (root_ == nullptr) {
            return nullptr;
        }
        const intrusive_tree_hook* node = root_;
        while (node->right_ != nullptr) {
            node = node->right_;
        }
        return value_of(*node);
    }

    template<typename Key>
    [[nodiscard]] constexpr auto find(const Key& key) noexcept -> T* {
        return const_cast<T*>(static_cast<const intrusive_tree*>(this)->find(key));
    }

    template<typename Key>
    [[nodiscard]] constexpr auto find(const Key& key) const noexcept
        -> const T* {
        const intrusive_tree_hook* node = root_;
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
            static_cast<const intrusive_tree*>(this)->lower_bound(key));
    }

    template<typename Key>
    [[nodiscard]] constexpr auto lower_bound(const Key& key) const noexcept
        -> const T* {
        const intrusive_tree_hook* node = root_;
        const intrusive_tree_hook* candidate{};
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
            static_cast<const intrusive_tree*>(this)->next(value));
    }

    [[nodiscard]] constexpr auto next(const T& value) const noexcept
        -> const T* {
        const intrusive_tree_hook* node = &hook_of(value);
        libk_assert(node->linked_);
        if (node->right_ != nullptr) {
            return value_of(*leftmost(*node->right_));
        }
        const intrusive_tree_hook* parent = node->parent_;
        while (parent != nullptr && node == parent->right_) {
            node = parent;
            parent = parent->parent_;
        }
        return parent == nullptr ? nullptr : value_of(*parent);
    }

    [[nodiscard]] constexpr auto previous(T& value) noexcept -> T* {
        return const_cast<T*>(
            static_cast<const intrusive_tree*>(this)->previous(value));
    }

    [[nodiscard]] constexpr auto previous(const T& value) const noexcept
        -> const T* {
        const intrusive_tree_hook* node = &hook_of(value);
        libk_assert(node->linked_);
        if (node->left_ != nullptr) {
            const intrusive_tree_hook* result = node->left_;
            while (result->right_ != nullptr) {
                result = result->right_;
            }
            return value_of(*result);
        }
        const intrusive_tree_hook* parent = node->parent_;
        while (parent != nullptr && node == parent->left_) {
            node = parent;
            parent = parent->parent_;
        }
        return parent == nullptr ? nullptr : value_of(*parent);
    }

    constexpr void insert(T& value) noexcept {
        intrusive_tree_hook& node = hook_of(value);
        libk_assert(!node.linked_);
        establish_offset(value);

        intrusive_tree_hook* parent{};
        intrusive_tree_hook** edge = &root_;
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
        intrusive_tree_hook& node = hook_of(value);
        libk_assert(node.linked_);

        intrusive_tree_hook* rebalance_from{};
        if (node.left_ == nullptr || node.right_ == nullptr) {
            intrusive_tree_hook* child = node.left_ != nullptr
                ? node.left_
                : node.right_;
            rebalance_from = node.parent_;
            replace(node, child);
        } else {
            intrusive_tree_hook* successor = leftmost(*node.right_);
            if (successor->parent_ != &node) {
                intrusive_tree_hook* const old_parent = successor->parent_;
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
    static constexpr auto hook_of(T& value) noexcept -> intrusive_tree_hook& {
        return value.*HookMember;
    }
    static constexpr auto hook_of(const T& value) noexcept
        -> const intrusive_tree_hook& {
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

    [[nodiscard]] constexpr auto value_of(intrusive_tree_hook& hook) noexcept
        -> T* {
        libk_assert(offset_known_);
        auto* bytes = reinterpret_cast<unsigned char*>(&hook);
        return reinterpret_cast<T*>(bytes - hook_offset_);
    }
    [[nodiscard]] constexpr auto value_of(
        const intrusive_tree_hook& hook) const noexcept -> const T* {
        libk_assert(offset_known_);
        auto* bytes = reinterpret_cast<const unsigned char*>(&hook);
        return reinterpret_cast<const T*>(bytes - hook_offset_);
    }

    [[nodiscard]] static constexpr auto height(
        const intrusive_tree_hook* node) noexcept -> int {
        return node == nullptr ? 0 : static_cast<int>(node->height_);
    }
    static constexpr void update_height(intrusive_tree_hook& node) noexcept {
        const int left = height(node.left_);
        const int right = height(node.right_);
        node.height_ = static_cast<unsigned>(
            1 + (left > right ? left : right));
    }
    [[nodiscard]] static constexpr auto balance(
        const intrusive_tree_hook& node) noexcept -> int {
        return height(node.left_) - height(node.right_);
    }
    [[nodiscard]] static constexpr auto leftmost(
        intrusive_tree_hook& node) noexcept -> intrusive_tree_hook* {
        intrusive_tree_hook* result = &node;
        while (result->left_ != nullptr) {
            result = result->left_;
        }
        return result;
    }

    constexpr void replace(
        intrusive_tree_hook& old_node,
        intrusive_tree_hook* replacement) noexcept {
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

    constexpr auto rotate_left(intrusive_tree_hook& root) noexcept
        -> intrusive_tree_hook* {
        intrusive_tree_hook* const pivot = root.right_;
        libk_assert(pivot != nullptr);
        intrusive_tree_hook* const parent = root.parent_;
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

    constexpr auto rotate_right(intrusive_tree_hook& root) noexcept
        -> intrusive_tree_hook* {
        intrusive_tree_hook* const pivot = root.left_;
        libk_assert(pivot != nullptr);
        intrusive_tree_hook* const parent = root.parent_;
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

    constexpr void rebalance(intrusive_tree_hook* node) noexcept {
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

    intrusive_tree_hook* root_{};
    intrusive_tree_hook* minimum_{};
    size_t size_{};
    ptrdiff_t hook_offset_{};
    bool offset_known_{};
    [[no_unique_address]] Compare compare_{};
};

} // namespace libk
