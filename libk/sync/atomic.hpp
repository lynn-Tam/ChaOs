#pragma once

#include <stdint.h>

#include <libk/typetraits.hpp>

namespace libk {

enum class MemoryOrder : uint8_t {
    Relaxed,
    Acquire,
    Release,
    AcqRel,
    SeqCst,
};

namespace atomic_detail {

template<typename T>
struct IsObjectPointer : false_type {};

template<typename T>
struct IsObjectPointer<T*> : bool_constant<is_object_v<T>> {};

template<typename T>
inline constexpr bool IsObjectPointerV = IsObjectPointer<T>::value;

template<typename T, bool = is_enum_v<T>>
struct StorageType {
    using type = T;
};

template<typename T>
struct StorageType<T, true> {
    using type = underlying_type_t<T>;
};

template<typename T>
using StorageTypeT = typename StorageType<T>::type;

template<typename T>
[[nodiscard]] constexpr auto to_storage(T value) noexcept -> StorageTypeT<T> {
    return static_cast<StorageTypeT<T>>(value);
}

template<typename T>
[[nodiscard]] constexpr auto from_storage(StorageTypeT<T> value) noexcept -> T {
    return static_cast<T>(value);
}

template<typename T>
inline constexpr bool IsRmwIntegral =
    is_integral_v<T> && !is_same_v<remove_cv_t<T>, bool>;

template<MemoryOrder order>
inline constexpr bool ValidLoadOrder =
    order == MemoryOrder::Relaxed
    || order == MemoryOrder::Acquire
    || order == MemoryOrder::SeqCst;

template<MemoryOrder order>
inline constexpr bool ValidStoreOrder =
    order == MemoryOrder::Relaxed
    || order == MemoryOrder::Release
    || order == MemoryOrder::SeqCst;

template<MemoryOrder order>
consteval auto builtin_order() noexcept -> int {
    if constexpr (order == MemoryOrder::Relaxed) {
        return __ATOMIC_RELAXED;
    } else if constexpr (order == MemoryOrder::Acquire) {
        return __ATOMIC_ACQUIRE;
    } else if constexpr (order == MemoryOrder::Release) {
        return __ATOMIC_RELEASE;
    } else if constexpr (order == MemoryOrder::AcqRel) {
        return __ATOMIC_ACQ_REL;
    } else {
        static_assert(order == MemoryOrder::SeqCst);
        return __ATOMIC_SEQ_CST;
    }
}

template<MemoryOrder success, MemoryOrder failure>
inline constexpr bool ValidCompareExchangeOrders = [] consteval {
    if constexpr (failure == MemoryOrder::Release
                  || failure == MemoryOrder::AcqRel) {
        return false;
    } else if constexpr (failure == MemoryOrder::Relaxed) {
        return true;
    } else if constexpr (failure == MemoryOrder::Acquire) {
        return success == MemoryOrder::Acquire
            || success == MemoryOrder::AcqRel
            || success == MemoryOrder::SeqCst;
    } else {
        return success == MemoryOrder::SeqCst;
    }
}();

} // namespace atomic_detail

template<typename T>
concept AtomicValue =
    !is_const_v<T>
    && !is_volatile_v<T>
    && (is_integral_v<T> || is_enum_v<T>
        || atomic_detail::IsObjectPointerV<T>)
    && (sizeof(T) == 1 || sizeof(T) == 2
        || sizeof(T) == 4 || sizeof(T) == 8)
    && __atomic_always_lock_free(sizeof(T), nullptr);

// Atomic owns exactly one naturally aligned, always-lock-free scalar. Memory
// order is a template argument so an invalid or runtime-selected order cannot
// silently become a stronger compiler operation.
template<AtomicValue T>
class Atomic final {
public:
    using value_type = T;
    static constexpr bool is_always_lock_free = true;

    constexpr Atomic() noexcept = default;
    constexpr explicit Atomic(T initial) noexcept
        : value_(atomic_detail::to_storage(initial)) {}

    Atomic(const Atomic&) = delete;
    auto operator=(const Atomic&) -> Atomic& = delete;
    Atomic(Atomic&&) = delete;
    auto operator=(Atomic&&) -> Atomic& = delete;
    ~Atomic() noexcept = default;

    template<MemoryOrder order>
        requires(atomic_detail::ValidLoadOrder<order>)
    [[nodiscard]] auto load() const noexcept -> T {
        return atomic_detail::from_storage<T>(
            __atomic_load_n(&value_, atomic_detail::builtin_order<order>()));
    }

    template<MemoryOrder order>
        requires(atomic_detail::ValidStoreOrder<order>)
    void store(T desired) noexcept {
        __atomic_store_n(
            &value_,
            atomic_detail::to_storage(desired),
            atomic_detail::builtin_order<order>());
    }

    template<MemoryOrder order>
    [[nodiscard]] auto exchange(T desired) noexcept -> T {
        return atomic_detail::from_storage<T>(__atomic_exchange_n(
            &value_,
            atomic_detail::to_storage(desired),
            atomic_detail::builtin_order<order>()));
    }

    template<MemoryOrder order>
        requires(atomic_detail::IsRmwIntegral<T>)
    [[nodiscard]] auto fetch_add(T operand) noexcept -> T {
        return atomic_detail::from_storage<T>(__atomic_fetch_add(
            &value_,
            atomic_detail::to_storage(operand),
            atomic_detail::builtin_order<order>()));
    }

    template<MemoryOrder order>
        requires(atomic_detail::IsRmwIntegral<T>)
    [[nodiscard]] auto fetch_sub(T operand) noexcept -> T {
        return atomic_detail::from_storage<T>(__atomic_fetch_sub(
            &value_,
            atomic_detail::to_storage(operand),
            atomic_detail::builtin_order<order>()));
    }

    template<MemoryOrder order>
        requires(atomic_detail::IsRmwIntegral<T>)
    [[nodiscard]] auto fetch_and(T operand) noexcept -> T {
        return atomic_detail::from_storage<T>(__atomic_fetch_and(
            &value_,
            atomic_detail::to_storage(operand),
            atomic_detail::builtin_order<order>()));
    }

    template<MemoryOrder order>
        requires(atomic_detail::IsRmwIntegral<T>)
    [[nodiscard]] auto fetch_or(T operand) noexcept -> T {
        return atomic_detail::from_storage<T>(__atomic_fetch_or(
            &value_,
            atomic_detail::to_storage(operand),
            atomic_detail::builtin_order<order>()));
    }

    template<MemoryOrder order>
        requires(atomic_detail::IsRmwIntegral<T>)
    [[nodiscard]] auto fetch_xor(T operand) noexcept -> T {
        return atomic_detail::from_storage<T>(__atomic_fetch_xor(
            &value_,
            atomic_detail::to_storage(operand),
            atomic_detail::builtin_order<order>()));
    }

    template<MemoryOrder success, MemoryOrder failure>
        requires(atomic_detail::ValidCompareExchangeOrders<success, failure>)
    [[nodiscard]] auto compare_exchange_strong(
        T& expected,
        T desired) noexcept -> bool {
        auto expected_storage = atomic_detail::to_storage(expected);
        const bool exchanged = __atomic_compare_exchange_n(
            &value_,
            &expected_storage,
            atomic_detail::to_storage(desired),
            false,
            atomic_detail::builtin_order<success>(),
            atomic_detail::builtin_order<failure>());
        if (!exchanged) {
            expected = atomic_detail::from_storage<T>(expected_storage);
        }
        return exchanged;
    }

    template<MemoryOrder success, MemoryOrder failure>
        requires(atomic_detail::ValidCompareExchangeOrders<success, failure>)
    [[nodiscard]] auto compare_exchange_weak(
        T& expected,
        T desired) noexcept -> bool {
        auto expected_storage = atomic_detail::to_storage(expected);
        const bool exchanged = __atomic_compare_exchange_n(
            &value_,
            &expected_storage,
            atomic_detail::to_storage(desired),
            true,
            atomic_detail::builtin_order<success>(),
            atomic_detail::builtin_order<failure>());
        if (!exchanged) {
            expected = atomic_detail::from_storage<T>(expected_storage);
        }
        return exchanged;
    }

private:
    using storage_type = atomic_detail::StorageTypeT<T>;
    alignas(sizeof(storage_type)) storage_type value_{};
};

template<MemoryOrder order>
inline void atomic_thread_fence() noexcept {
    __atomic_thread_fence(atomic_detail::builtin_order<order>());
}

template<MemoryOrder order>
inline void atomic_signal_fence() noexcept {
    __atomic_signal_fence(atomic_detail::builtin_order<order>());
}

template<AtomicValue T>
inline constexpr bool AtomicHasScalarLayout =
    sizeof(Atomic<T>) == sizeof(T)
    && alignof(Atomic<T>) >= alignof(T);

} // namespace libk
