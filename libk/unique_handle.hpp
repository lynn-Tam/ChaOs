#pragma once

// Move-only RAII wrapper for non-pointer resource handles.
// EmptyPolicy supplies static empty() and is_empty(handle); DefaultEmptyValue uses T{}.

#include <concepts>
#include <libk/concepts.hpp>
#include <type_traits>
#include <utility>

namespace libk {

template<typename HandleT>
struct DefaultEmptyValue {
    [[nodiscard]] static constexpr HandleT empty() noexcept(
        std::is_nothrow_default_constructible_v<HandleT>) {
        return HandleT{};
    }

    [[nodiscard]] static constexpr bool is_empty(const HandleT& handle)
        noexcept(noexcept(handle == empty())) {
        return handle == empty();
    }
};

template<typename HandleT, HandleT EmptyValue>
struct StaticEmptyValue {
    [[nodiscard]] static constexpr HandleT empty() noexcept {
        return EmptyValue;
    }

    [[nodiscard]] static constexpr bool is_empty(const HandleT& handle)
        noexcept(noexcept(handle == EmptyValue)) {
        return handle == EmptyValue;
    }
};

template<
    typename HandleT,
    typename DeleterT,
    typename EmptyPolicy = DefaultEmptyValue<HandleT>>
class unique_handle {
    static_assert(std::is_move_constructible_v<HandleT>
                  && std::is_move_assignable_v<HandleT>,
                  "unique_handle requires a movable handle type");
    static_assert(requires(const HandleT& handle) {
        { EmptyPolicy::empty() } -> std::convertible_to<HandleT>;
        { EmptyPolicy::is_empty(handle) } -> std::convertible_to<bool>;
    }, "EmptyPolicy must provide static empty() and is_empty(handle)");
    static_assert(requires(DeleterT& deleter, HandleT& handle) {
        deleter(handle);
    }, "DeleterT must be callable with a handle lvalue");

    static constexpr bool has_equality_ =
        requires(const HandleT& lhs, const HandleT& rhs) {
            lhs == rhs;
        };
    static constexpr bool equality_is_nothrow_ =
        !has_equality_
        || requires(const HandleT& lhs, const HandleT& rhs) {
            { lhs == rhs } noexcept;
        };

public:
    using handle_type = HandleT;
    using deleter_type = DeleterT;
    using empty_policy = EmptyPolicy;

    constexpr unique_handle()
        noexcept(noexcept(empty_value())
                 && std::is_nothrow_default_constructible_v<deleter_type>)
        : handle_(empty_value()), deleter_{} {}

    constexpr explicit unique_handle(handle_type handle)
        noexcept(std::is_nothrow_move_constructible_v<handle_type>
                 && std::is_nothrow_default_constructible_v<deleter_type>)
        : handle_(std::move(handle)), deleter_{} {}

    constexpr unique_handle(handle_type handle, const deleter_type& deleter)
        noexcept(std::is_nothrow_move_constructible_v<handle_type>
                 && std::is_nothrow_copy_constructible_v<deleter_type>)
        : handle_(std::move(handle)), deleter_(deleter) {}

    constexpr unique_handle(handle_type handle, deleter_type&& deleter)
        noexcept(std::is_nothrow_move_constructible_v<handle_type>
                 && std::is_nothrow_move_constructible_v<deleter_type>)
        : handle_(std::move(handle)), deleter_(std::move(deleter)) {}

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    constexpr unique_handle(unique_handle&& other)
        noexcept(noexcept(other.release())
                 && std::is_nothrow_move_constructible_v<deleter_type>)
        : handle_(other.release()), deleter_(std::move(other.deleter_)) {}

    constexpr unique_handle& operator=(unique_handle&& other)
        noexcept(noexcept(reset(other.release()))
                 && std::is_nothrow_move_assignable_v<deleter_type>) {
        if (this == &other) {
            return *this;
        }
        reset(other.release());
        deleter_ = std::move(other.deleter_);
        return *this;
    }

    constexpr ~unique_handle() noexcept {
        reset();
    }

    [[nodiscard]] constexpr const handle_type& get() const noexcept {
        return handle_;
    }

    [[nodiscard]] constexpr deleter_type& get_deleter() noexcept {
        return deleter_;
    }

    [[nodiscard]] constexpr const deleter_type& get_deleter() const noexcept {
        return deleter_;
    }

    [[nodiscard]] constexpr explicit operator bool() const
        noexcept(noexcept(EmptyPolicy::is_empty(handle_))) {
        return !EmptyPolicy::is_empty(handle_);
    }

    [[nodiscard]] constexpr handle_type release()
        noexcept(std::is_nothrow_move_constructible_v<handle_type>
                 && std::is_nothrow_assignable_v<handle_type&, handle_type>
                 && noexcept(empty_value())) {
        handle_type old = std::move(handle_);
        handle_ = empty_value();
        return old;
    }

    constexpr void reset(handle_type replacement = empty_value())
        noexcept(noexcept(EmptyPolicy::is_empty(handle_))
                 && noexcept(deleter_(handle_))
                 && std::is_nothrow_move_constructible_v<handle_type>
                 && std::is_nothrow_move_assignable_v<handle_type>
                 && equality_is_nothrow_) {
        if constexpr (has_equality_) {
            if (handle_ == replacement) {
                return;
            }
        }

        handle_type old = std::move(handle_);
        handle_ = std::move(replacement);
        if (!EmptyPolicy::is_empty(old)) {
            deleter_(old);
        }
    }

    constexpr void swap(unique_handle& other)
        noexcept(noexcept(std::swap(handle_, other.handle_))
                 && noexcept(std::swap(deleter_, other.deleter_))) {
        std::swap(handle_, other.handle_);
        std::swap(deleter_, other.deleter_);
    }

    [[nodiscard]] static constexpr handle_type empty_value()
        noexcept(noexcept(EmptyPolicy::empty())) {
        return static_cast<handle_type>(EmptyPolicy::empty());
    }

private:
    handle_type handle_;
    [[no_unique_address]] deleter_type deleter_;
};

template<typename H, typename D, typename P>
[[nodiscard]] constexpr bool operator==(
    const unique_handle<H, D, P>& lhs,
    const unique_handle<H, D, P>& rhs)
    noexcept(noexcept(lhs.get() == rhs.get())) {
    return lhs.get() == rhs.get();
}

} // namespace libk
