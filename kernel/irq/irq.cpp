#include <expected>
#include <irq/irq.hpp>
#include <ipc/notification.hpp>
#include <limits>

namespace irq {

auto Route::connect_locked(bool armed) noexcept -> bool {
    if (hook_.is_linked() || line_.id == 0 || !handler_) return false;
    for (auto& r : line_.routes.routes_)
        if (r.line_.id == line_.id) return false;
    line_.routes.routes_.push_back(*this);
    arm_locked(armed);
    return true;
}

auto Route::connect() noexcept -> bool {
    sync::Lock guard{line_.routes.lock_};
    return connect_locked(true);
}

void Route::arm_locked(bool armed) noexcept {
    armed_ = armed;
    line_.routes.set(line_.id, armed);
}

void Route::reset_locked() noexcept {
    if (!hook_.is_linked()) return;
    arm_locked(false);
    line_.routes.routes_.erase(*this);
}

void Route::reset() noexcept {
    sync::Lock guard{line_.routes.lock_};
    reset_locked();
}

void Routes::dispatch(Take take, End end) noexcept {
    sync::Lock guard{lock_};
    const u32 id = take();
    if (id == 0) return;
    for (auto& r : routes_) {
        if (r.line_.id != id) continue;
        r.arm_locked(false);
        const bool rearm = r.handler_();
        if (end) end(id);
        if (rearm && r.hook_.is_linked()) r.arm_locked(true);
        return;
    }
    set(id, false);
    if (end) end(id);
}

void Routes::refresh(Setup setup) noexcept {
    sync::Lock guard{lock_};
    if (setup) setup();
    for (auto& r : routes_) set(r.line_.id, r.armed_);
}

Irq::Irq(Line line) noexcept
    : route_(line, Route::Handler::bind<&Irq::publish>(*this)) {
    libk_assert(line.id != 0);
}

Irq::~Irq() noexcept { (void)close(); }

auto Irq::bound() const noexcept -> bool {
    sync::Lock guard{route_.line_.routes.lock_};
    return notice_.attached();
}

auto Irq::pending() const noexcept -> bool {
    sync::Lock guard{route_.line_.routes.lock_};
    return pending_;
}

auto Irq::closed() const noexcept -> bool {
    sync::Lock guard{route_.line_.routes.lock_};
    return closed_;
}

auto Irq::bind(ipc::Notification& n, u64 badge) noexcept -> std::expected<void, Error> {
    sync::Lock guard{route_.line_.routes.lock_};
    if (closed_ || generation_ == std::numeric_limits<u64>::max())
        return std::unexpected(Error::Closed);
    if (badge == 0) return std::unexpected(Error::InvalidState);
    if (notice_.attached()) return std::unexpected(Error::Busy);
    // A closed Notification has removed its edge. It owes no callback into
    // this object; the route retains any already delivered event until rebind.
    route_.reset_locked();
    if (!n.bind(notice_, badge)) return std::unexpected(Error::Busy);
    if (!route_.connect_locked(!pending_)) {
        notice_.reset();
        return std::unexpected(Error::Busy);
    }
    ++generation_;
    if (pending_) (void)notice_.signal();
    return {};
}

auto Irq::unbind() noexcept -> bool {
    sync::Lock guard{route_.line_.routes.lock_};
    const bool bound = notice_.attached();
    route_.reset_locked();
    notice_.reset();
    return bound;
}

auto Irq::delivery() const noexcept -> std::expected<Delivery, Error> {
    sync::Lock guard{route_.line_.routes.lock_};
    if (closed_) return std::unexpected(Error::Closed);
    if (!pending_ || !notice_.attached()) return std::unexpected(Error::InvalidState);
    return (Delivery{sequence_, generation_});
}

auto Irq::publish() noexcept -> bool {
    // Routes holds a claimed, masked line and the ownership lock through
    // publication; the controller retains its own EOI timing.
    if (closed_) return false;
    if (sequence_ == std::numeric_limits<u64>::max()) {
        closed_ = true;
        route_.reset_locked();
        notice_.reset();
        return false;
    }
    ++sequence_;
    pending_ = true;
    (void)notice_.signal();
    return false;
}

auto Irq::ack(u64 generation, u64 sequence) noexcept -> std::expected<void, Error> {
    sync::Lock guard{route_.line_.routes.lock_};
    if (closed_) return std::unexpected(Error::Closed);
    if (!pending_ || !notice_.attached()) return std::unexpected(Error::InvalidState);
    if (generation != generation_)
        return std::unexpected(Error::StaleSequence);
    if (sequence == 0 || sequence > sequence_) return std::unexpected(Error::BadSequence);
    if (sequence < sequence_) return std::unexpected(Error::StaleSequence);
    pending_ = false;
    // The driver must first clear the device's interrupt condition. This
    // acknowledgement releases the software delivery, then enables the line.
    route_.arm_locked(true);
    return {};
}

auto Irq::close() noexcept -> bool {
    sync::Lock guard{route_.line_.routes.lock_};
    route_.reset_locked();
    notice_.reset();
    closed_ = true;
    return true;
}

void Irq::retire(object::cleanup&& cleanup) noexcept {
    (void)close();
    // Removing the route under the controller ownership lock has drained
    // every publication. No asynchronous callback retains an Irq pointer.
    cleanup.complete();
}

} // namespace irq
