#pragma once

#include <libk/concepts.hpp>
#include <libk/noncopyable.hpp>
#include <libk/utility.hpp>

namespace libk {

// C++23-style scope guard for freestanding transactions. The callable must be
// noexcept: rollback is part of stack unwinding-free kernel control flow and
// cannot expose a second failure channel.
template<typename F>
requires requires(F& function) {
    { function() } noexcept;
}
class scope_exit final : private noncopyable {
public:
    constexpr explicit scope_exit(F function) noexcept
        : function_(libk::move(function)) {}

    constexpr scope_exit(scope_exit&& other) noexcept
        : function_(libk::move(other.function_)), active_(other.release()) {}

    auto operator=(scope_exit&&) -> scope_exit& = delete;

    constexpr ~scope_exit() noexcept {
        if (active_) {
            function_();
        }
    }

    constexpr auto release() noexcept -> bool {
        return libk::exchange(active_, false);
    }

private:
    [[no_unique_address]] F function_;
    bool active_{true};
};

template<typename F>
scope_exit(F) -> scope_exit<F>;

template<typename F>
[[nodiscard]] constexpr auto on_scope_exit(F&& function) noexcept
    -> scope_exit<remove_cvr_t<F>> {
    return scope_exit<remove_cvr_t<F>>{libk::forward<F>(function)};
}

} // namespace libk
