#pragma once

#include <type_traits>
#include <utility>

namespace libk::detail {

template<typename FunctionPointer>
union CallableTarget {
    void* object;
    FunctionPointer function;

    constexpr CallableTarget() noexcept : object(nullptr) {}
    constexpr explicit CallableTarget(void* value) noexcept : object(value) {}
    constexpr explicit CallableTarget(FunctionPointer value) noexcept
        : function(value) {}
};

template<typename T>
auto declval() noexcept -> T&&;

template<typename F, typename... Args>
concept Invocable = requires(F&& function, Args&&... arguments) {
    std::forward<F>(function)(std::forward<Args>(arguments)...);
};

template<typename R, typename F, typename... Args>
concept InvocableR = Invocable<F, Args...>
    && (std::is_void_v<R>
        || requires(F&& function, Args&&... arguments) {
            static_cast<R>(std::forward<F>(function)(
                std::forward<Args>(arguments)...));
        });

template<typename R, typename F, typename... Args>
concept NothrowInvocableR = InvocableR<R, F, Args...>
    && noexcept(detail::declval<F>()(detail::declval<Args>()...));

template<typename R, typename F, typename... Args>
    requires InvocableR<R, F, Args...>
constexpr auto invoke_r(F&& function, Args&&... arguments)
    noexcept(noexcept(std::forward<F>(function)(
        std::forward<Args>(arguments)...))) -> R {
    if constexpr (std::is_void_v<R>) {
        std::forward<F>(function)(std::forward<Args>(arguments)...);
    } else {
        return static_cast<R>(std::forward<F>(function)(
            std::forward<Args>(arguments)...));
    }
}

template<typename SignatureArg>
constexpr decltype(auto) multicast_argument(
    std::remove_reference_t<SignatureArg>& value) noexcept {
    if constexpr (std::is_rvalue_reference_v<SignatureArg>) {
        return static_cast<SignatureArg>(value);
    } else {
        return (value);
    }
}

} // namespace libk::detail
