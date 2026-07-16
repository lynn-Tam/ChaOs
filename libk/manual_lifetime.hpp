#pragma once

#include <stdint.h>

#include <libk/concepts.hpp>
#include <libk/assert.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>

namespace libk {

template<typename T>
requires(Object<T> && !is_const_v<T>)
class ManualLifetime{
public:
    constexpr ManualLifetime() noexcept = default;

    ManualLifetime(const ManualLifetime&) = delete;
    auto operator=(const ManualLifetime&) -> ManualLifetime& = delete;

    ManualLifetime(ManualLifetime&&) = delete;
    auto operator=(ManualLifetime&&) -> ManualLifetime& = delete;

    template<typename... Args>
    requires ConstructibleFrom<T, Args&&...>
    [[nodiscard]] auto emplace(Args&&... args) -> T& {
        libk_assert(!engaged_);
        T* value = libk::construct_at(
            stor_ptr(),
            libk::forward<Args>(args)...);
        engaged_ = true;
        return *value;
    }

    auto reset() noexcept -> void{
        if (!engaged_) {
            return;
        }
        libk::destroy_at(stor_ptr());
        engaged_ = false;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept{
        return engaged_;
    }

    [[nodiscard]] auto operator*() noexcept -> T& {
        libk_assert(engaged_);
        return *stor_ptr();
    }

    [[nodiscard]] auto operator*() const noexcept -> const  T& {
        libk_assert(engaged_);
        return *stor_ptr();
    }

    [[nodiscard]] auto operator->() noexcept -> T* {
        libk_assert(engaged_);
        return stor_ptr();
    }

    [[nodiscard]] auto operator->() const noexcept -> const T* {
        libk_assert(engaged_);
        return stor_ptr();
    }

private:
    [[nodiscard]] auto stor_ptr() noexcept -> T* {
        return reinterpret_cast<T*>(storage_);
    }
    [[nodiscard]] auto stor_ptr() const noexcept -> const T*{
        return reinterpret_cast<const T*>(storage_);
    }
    alignas(T) unsigned char storage_[sizeof(T)]{};
    bool engaged_{};
};

} // namespace libk
