#pragma once

// Allocation-free delegate and fixed-capacity multicast event.
// Runtime free functions and compile-time free/member bindings use two-word delegates.
// Every binding is non-owning; bound objects and callable targets must outlive the delegate.
// multicast_delegate intentionally supports void event signatures only; mutation while
// dispatching is rejected. Value parameters are copied for each subscriber; rvalue-reference
// event parameters are rejected because one object cannot be moved into multiple subscribers safely.

#include <stddef.h>
#include <stdint.h>

#include <libk/assert.hpp>
#include <libk/detail/callable.hpp>
#include <libk/typetraits.hpp>
#include <libk/utility.hpp>

namespace libk {

template<typename Signature>
class delegate;

template<typename R, typename... Args>
class delegate<R(Args...)> {
private:
    using function_pointer = R (*)(Args...);
    using target_type = detail::CallableTarget<function_pointer>;
    using stub_type = R (*)(target_type, Args&&...);

public:
    constexpr delegate() noexcept = default;
    constexpr delegate(decltype(nullptr)) noexcept {}

    constexpr delegate(function_pointer function) noexcept
        : target_(function),
          stub_(function == nullptr ? nullptr : &invoke_runtime_function) {}

    template<auto Function>
        requires(is_pointer_v<decltype(Function)>
                 && is_function_v<remove_pointer_t<decltype(Function)>>
                 && detail::InvocableR<R, decltype(Function), Args...>)
    [[nodiscard]] static constexpr delegate bind() noexcept {
        static_assert(Function != nullptr, "delegate cannot bind a null function");
        delegate result;
        result.stub_ = &invoke_free<Function>;
        return result;
    }

    template<auto Method, typename C>
        requires(is_member_function_pointer_v<decltype(Method)>
                 && requires(C& object, Args&&... args) {
            (object.*Method)(libk::forward<Args>(args)...);
            requires is_void_v<R> || requires {
                static_cast<R>((object.*Method)(libk::forward<Args>(args)...));
            };
        })
    [[nodiscard]] static constexpr delegate bind(C& object) noexcept {
        delegate result;
        result.target_ = target_type(const_cast<void*>(
            static_cast<const void*>(libk::addressof(object))));
        result.stub_ = &invoke_member<Method, C>;
        return result;
    }

    template<typename F>
        requires (!is_same_v<remove_cvr_t<F>, delegate>
                  && !is_function_v<remove_ref_t<F>>
                  && detail::InvocableR<R, F&, Args...>)
    [[nodiscard]] static constexpr delegate bind(F& function) noexcept {
        delegate result;
        result.target_ = target_type(const_cast<void*>(
            static_cast<const void*>(libk::addressof(function))));
        result.stub_ = &invoke_object<F>;
        return result;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return stub_ != nullptr;
    }

    constexpr void reset() noexcept {
        target_ = target_type{};
        stub_ = nullptr;
    }

    constexpr R operator()(Args... args) const
        noexcept(noexcept(stub_(target_, libk::forward<Args>(args)...))) {
        libk_assert(stub_ != nullptr);
        return stub_(target_, libk::forward<Args>(args)...);
    }

private:
    static constexpr R invoke_runtime_function(target_type target, Args&&... args)
        noexcept(noexcept(detail::invoke_r<R>(
            target.function, libk::forward<Args>(args)...))) {
        return detail::invoke_r<R>(
            target.function, libk::forward<Args>(args)...);
    }

    template<auto Function>
    static constexpr R invoke_free(target_type, Args&&... args)
        noexcept(noexcept(detail::invoke_r<R>(
            Function, libk::forward<Args>(args)...))) {
        return detail::invoke_r<R>(
            Function, libk::forward<Args>(args)...);
    }

    template<auto Method, typename C>
    static constexpr R invoke_member(target_type target, Args&&... args)
        noexcept(noexcept(((*static_cast<C*>(target.object)).*Method)(
            libk::forward<Args>(args)...))) {
        C& object = *static_cast<C*>(target.object);
        if constexpr (is_void_v<R>) {
            (object.*Method)(libk::forward<Args>(args)...);
        } else {
            return static_cast<R>(
                (object.*Method)(libk::forward<Args>(args)...));
        }
    }

    template<typename F>
    static constexpr R invoke_object(target_type target, Args&&... args)
        noexcept(noexcept(detail::invoke_r<R>(
            *static_cast<F*>(target.object),
            libk::forward<Args>(args)...))) {
        return detail::invoke_r<R>(
            *static_cast<F*>(target.object),
            libk::forward<Args>(args)...);
    }

    target_type target_{};
    stub_type stub_ = nullptr;
};

template<typename R, typename... Args>
class delegate<R(Args...) noexcept> {
private:
    using function_pointer = R (*)(Args...) noexcept;
    using target_type = detail::CallableTarget<function_pointer>;
    using stub_type = R (*)(target_type, Args&&...) noexcept;

public:
    constexpr delegate() noexcept = default;
    constexpr delegate(decltype(nullptr)) noexcept {}

    constexpr delegate(function_pointer function) noexcept
        : target_(function),
          stub_(function == nullptr ? nullptr : &invoke_runtime_function) {}

    template<auto Function>
        requires(is_pointer_v<decltype(Function)>
                 && is_function_v<remove_pointer_t<decltype(Function)>>
                 && detail::NothrowInvocableR<R, decltype(Function), Args...>)
    [[nodiscard]] static constexpr delegate bind() noexcept {
        static_assert(Function != nullptr, "delegate cannot bind a null function");
        delegate result;
        result.stub_ = &invoke_free<Function>;
        return result;
    }

    template<auto Method, typename C>
        requires(is_member_function_pointer_v<decltype(Method)>
                 && requires(C& object, Args&&... args) {
            requires noexcept((object.*Method)(libk::forward<Args>(args)...));
            (object.*Method)(libk::forward<Args>(args)...);
            requires is_void_v<R> || requires {
                static_cast<R>((object.*Method)(libk::forward<Args>(args)...));
            };
        })
    [[nodiscard]] static constexpr delegate bind(C& object) noexcept {
        delegate result;
        result.target_ = target_type(const_cast<void*>(
            static_cast<const void*>(libk::addressof(object))));
        result.stub_ = &invoke_member<Method, C>;
        return result;
    }

    template<typename F>
        requires (!is_same_v<remove_cvr_t<F>, delegate>
                  && !is_function_v<remove_ref_t<F>>
                  && detail::NothrowInvocableR<R, F&, Args...>)
    [[nodiscard]] static constexpr delegate bind(F& function) noexcept {
        delegate result;
        result.target_ = target_type(const_cast<void*>(
            static_cast<const void*>(libk::addressof(function))));
        result.stub_ = &invoke_object<F>;
        return result;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return stub_ != nullptr;
    }

    constexpr void reset() noexcept {
        target_ = target_type{};
        stub_ = nullptr;
    }

    constexpr R operator()(Args... args) const noexcept {
        libk_assert(stub_ != nullptr);
        return stub_(target_, libk::forward<Args>(args)...);
    }

private:
    static constexpr R invoke_runtime_function(target_type target, Args&&... args) noexcept {
        return detail::invoke_r<R>(
            target.function, libk::forward<Args>(args)...);
    }

    template<auto Function>
    static constexpr R invoke_free(target_type, Args&&... args) noexcept {
        return detail::invoke_r<R>(
            Function, libk::forward<Args>(args)...);
    }

    template<auto Method, typename C>
    static constexpr R invoke_member(target_type target, Args&&... args) noexcept {
        C& object = *static_cast<C*>(target.object);
        if constexpr (is_void_v<R>) {
            (object.*Method)(libk::forward<Args>(args)...);
        } else {
            return static_cast<R>(
                (object.*Method)(libk::forward<Args>(args)...));
        }
    }

    template<typename F>
    static constexpr R invoke_object(target_type target, Args&&... args) noexcept {
        return detail::invoke_r<R>(
            *static_cast<F*>(target.object),
            libk::forward<Args>(args)...);
    }

    target_type target_{};
    stub_type stub_ = nullptr;
};

template<typename Signature, size_t Capacity>
class multicast_delegate;

template<typename... Args, size_t Capacity>
class multicast_delegate<void(Args...), Capacity> {
    static_assert(Capacity > 0, "multicast_delegate capacity must be non-zero");
    static_assert((!is_rvalue_reference_v<Args> && ...),
                  "multicast_delegate does not support rvalue-reference event arguments");

public:
    using callback_type = delegate<void(Args...)>;

    class connection {
        friend class multicast_delegate;

    public:
        constexpr connection() noexcept = default;

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return owner_ != nullptr && id_ != 0;
        }

    private:
        constexpr connection(const multicast_delegate* owner, uint64_t id) noexcept
            : owner_(owner), id_(id) {}

        const multicast_delegate* owner_ = nullptr;
        uint64_t id_ = 0;
    };

    constexpr multicast_delegate() noexcept = default;
    multicast_delegate(const multicast_delegate&) = delete;
    multicast_delegate& operator=(const multicast_delegate&) = delete;
    multicast_delegate(multicast_delegate&&) = delete;
    multicast_delegate& operator=(multicast_delegate&&) = delete;

    [[nodiscard]] constexpr size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ == Capacity; }

    [[nodiscard]] constexpr connection try_connect(callback_type callback) noexcept {
        libk_assert(dispatch_depth_ == 0);
        if (!callback || full()) {
            return {};
        }

        const uint64_t id = allocate_id();
        entries_[size_] = entry{id, callback};
        ++size_;
        return connection(this, id);
    }

    [[nodiscard]] constexpr connection try_connect(void (*function)(Args...)) noexcept {
        return try_connect(callback_type(function));
    }

    template<auto Function>
    [[nodiscard]] constexpr connection try_connect() noexcept {
        return try_connect(callback_type::template bind<Function>());
    }

    template<auto Method, typename C>
    [[nodiscard]] constexpr connection try_connect(C& object) noexcept {
        return try_connect(callback_type::template bind<Method>(object));
    }

    template<typename F>
        requires (!is_same_v<remove_cvr_t<F>, callback_type>)
    [[nodiscard]] constexpr connection try_connect(F& function) noexcept {
        return try_connect(callback_type::bind(function));
    }

    [[nodiscard]] constexpr bool disconnect(connection value) noexcept {
        libk_assert(dispatch_depth_ == 0);
        if (value.owner_ != this || value.id_ == 0) {
            return false;
        }

        for (size_t index = 0; index < size_; ++index) {
            if (entries_[index].id == value.id_) {
                erase_stable(index);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr bool disconnect_unordered(connection value) noexcept {
        libk_assert(dispatch_depth_ == 0);
        if (value.owner_ != this || value.id_ == 0) {
            return false;
        }

        for (size_t index = 0; index < size_; ++index) {
            if (entries_[index].id == value.id_) {
                --size_;
                if (index != size_) {
                    entries_[index] = entries_[size_];
                }
                entries_[size_] = entry{};
                return true;
            }
        }
        return false;
    }

    constexpr void clear() noexcept {
        libk_assert(dispatch_depth_ == 0);
        while (size_ != 0) {
            --size_;
            entries_[size_] = entry{};
        }
    }

    constexpr void operator()(Args... args) const {
        dispatch_guard guard(*this);
        for (size_t index = 0; index < size_; ++index) {
            entries_[index].callback(detail::multicast_argument<Args>(args)...);
        }
    }

private:
    struct entry {
        uint64_t id = 0;
        callback_type callback{};
    };

    class dispatch_guard {
    public:
        explicit constexpr dispatch_guard(const multicast_delegate& owner) noexcept
            : owner_(owner) {
            ++owner_.dispatch_depth_;
        }

        constexpr ~dispatch_guard() {
            --owner_.dispatch_depth_;
        }

    private:
        const multicast_delegate& owner_;
    };

    [[nodiscard]] constexpr uint64_t allocate_id() noexcept {
        uint64_t id = next_id_++;
        if (id == 0) {
            id = next_id_++;
        }
        libk_assert(id != 0);
        return id;
    }

    constexpr void erase_stable(size_t index) noexcept {
        for (size_t current = index; current + 1 < size_; ++current) {
            entries_[current] = entries_[current + 1];
        }
        --size_;
        entries_[size_] = entry{};
    }

    entry entries_[Capacity]{};
    size_t size_ = 0;
    uint64_t next_id_ = 1;
    mutable size_t dispatch_depth_ = 0;
};

template<typename... Args, size_t Capacity>
class multicast_delegate<void(Args...) noexcept, Capacity> {
    static_assert(Capacity > 0, "multicast_delegate capacity must be non-zero");
    static_assert((!is_rvalue_reference_v<Args> && ...),
                  "multicast_delegate does not support rvalue-reference event arguments");

public:
    using callback_type = delegate<void(Args...) noexcept>;

    class connection {
        friend class multicast_delegate;

    public:
        constexpr connection() noexcept = default;
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return owner_ != nullptr && id_ != 0;
        }

    private:
        constexpr connection(const multicast_delegate* owner, uint64_t id) noexcept
            : owner_(owner), id_(id) {}
        const multicast_delegate* owner_ = nullptr;
        uint64_t id_ = 0;
    };

    constexpr multicast_delegate() noexcept = default;
    multicast_delegate(const multicast_delegate&) = delete;
    multicast_delegate& operator=(const multicast_delegate&) = delete;
    multicast_delegate(multicast_delegate&&) = delete;
    multicast_delegate& operator=(multicast_delegate&&) = delete;

    [[nodiscard]] constexpr size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ == Capacity; }

    [[nodiscard]] constexpr connection try_connect(callback_type callback) noexcept {
        libk_assert(dispatch_depth_ == 0);
        if (!callback || full()) {
            return {};
        }
        const uint64_t id = allocate_id();
        entries_[size_] = entry{id, callback};
        ++size_;
        return connection(this, id);
    }

    [[nodiscard]] constexpr connection try_connect(
        void (*function)(Args...) noexcept) noexcept {
        return try_connect(callback_type(function));
    }

    template<auto Function>
    [[nodiscard]] constexpr connection try_connect() noexcept {
        return try_connect(callback_type::template bind<Function>());
    }

    template<auto Method, typename C>
    [[nodiscard]] constexpr connection try_connect(C& object) noexcept {
        return try_connect(callback_type::template bind<Method>(object));
    }

    template<typename F>
        requires (!is_same_v<remove_cvr_t<F>, callback_type>)
    [[nodiscard]] constexpr connection try_connect(F& function) noexcept {
        return try_connect(callback_type::bind(function));
    }

    [[nodiscard]] constexpr bool disconnect(connection value) noexcept {
        libk_assert(dispatch_depth_ == 0);
        if (value.owner_ != this || value.id_ == 0) {
            return false;
        }
        for (size_t index = 0; index < size_; ++index) {
            if (entries_[index].id == value.id_) {
                erase_stable(index);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr bool disconnect_unordered(connection value) noexcept {
        libk_assert(dispatch_depth_ == 0);
        if (value.owner_ != this || value.id_ == 0) {
            return false;
        }
        for (size_t index = 0; index < size_; ++index) {
            if (entries_[index].id == value.id_) {
                --size_;
                if (index != size_) {
                    entries_[index] = entries_[size_];
                }
                entries_[size_] = entry{};
                return true;
            }
        }
        return false;
    }

    constexpr void clear() noexcept {
        libk_assert(dispatch_depth_ == 0);
        while (size_ != 0) {
            --size_;
            entries_[size_] = entry{};
        }
    }

    constexpr void operator()(Args... args) const noexcept {
        dispatch_guard guard(*this);
        for (size_t index = 0; index < size_; ++index) {
            entries_[index].callback(detail::multicast_argument<Args>(args)...);
        }
    }

private:
    struct entry {
        uint64_t id = 0;
        callback_type callback{};
    };

    class dispatch_guard {
    public:
        explicit constexpr dispatch_guard(const multicast_delegate& owner) noexcept
            : owner_(owner) { ++owner_.dispatch_depth_; }
        constexpr ~dispatch_guard() { --owner_.dispatch_depth_; }
    private:
        const multicast_delegate& owner_;
    };

    [[nodiscard]] constexpr uint64_t allocate_id() noexcept {
        uint64_t id = next_id_++;
        if (id == 0) {
            id = next_id_++;
        }
        libk_assert(id != 0);
        return id;
    }

    constexpr void erase_stable(size_t index) noexcept {
        for (size_t current = index; current + 1 < size_; ++current) {
            entries_[current] = entries_[current + 1];
        }
        --size_;
        entries_[size_] = entry{};
    }

    entry entries_[Capacity]{};
    size_t size_ = 0;
    uint64_t next_id_ = 1;
    mutable size_t dispatch_depth_ = 0;
};

template<typename Signature>
using fast_delegate = delegate<Signature>;

template<typename Signature, size_t Capacity>
using MulticastDelegate = multicast_delegate<Signature, Capacity>;

} // namespace libk
