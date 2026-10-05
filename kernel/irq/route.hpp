#pragma once

#include <sync.hpp>
#include <libk/intrusive_list.hpp>

namespace irq {

class Routes;

// Controller lifetime exceeds every route and capability using this line.
struct Line final {
    Routes& routes;
    u32 id;
    bool level{true};
};

// Caller-owned route: one kernel handler or one capability owns a source.
// reset drains dispatch before returning; handlers cannot reenter Routes.
class Route final : private libk::noncopyable_nonmovable {
public:
    using Handler = libk::delegate<bool() noexcept>;
    Route(Line line, Handler handler) noexcept : line_(line), handler_(handler) {}
    ~Route() noexcept { reset(); }
    [[nodiscard]] auto connect() noexcept -> bool;
    void reset() noexcept;
private:
    friend class Routes;
    friend class Irq;
    [[nodiscard]] auto connect_locked(bool armed) noexcept -> bool;
    void reset_locked() noexcept;
    void arm_locked(bool armed) noexcept;
    Line line_;
    Handler handler_;
    libk::IntrusiveListHook hook_{};
    bool armed_{};
};

class Routes final : private libk::noncopyable_nonmovable {
public:
    using Enable = libk::delegate<void(u32, bool enabled) noexcept>;
    using Take = libk::delegate<u32() noexcept>;
    using End = libk::delegate<void(u32) noexcept>;
    explicit Routes(Enable enable = {}) noexcept : enable_(enable) {}
    ~Routes() noexcept { libk_assert(routes_.empty()); }
    // Controllers choose EOI timing: take may finish before masking (PLIC),
    // or end may finish after masked publication, before rearming. User ack
    // only rearms a route and never clears the device's interrupt condition.
    void dispatch(Take take, End end = {}) noexcept;
    using Setup = libk::delegate<void() noexcept>;
    void refresh(Setup setup = {}) noexcept;
private:
    friend class Route;
    friend class Irq;
    void set(u32 id, bool enabled) noexcept { if (enable_) enable_(id, enabled); }
    mutable sync::Spin lock_{};
    Enable enable_{};
    libk::IntrusiveList<Route, &Route::hook_> routes_{};
};

} // namespace irq
