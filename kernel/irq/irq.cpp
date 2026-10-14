#include <expected>
#include <irq/irq.hpp>
#include <ipc/notification.hpp>
#include <limits>
#include <cpu/cpu.hpp>

namespace irq {

auto Route::connect_locked(bool armed) noexcept -> bool {
    if (line_.routes.closed_ || hook_.is_linked() || line_.id == 0 || !handler_) return false;
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
    if (closed_) return;
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
    if (closed_) return;
    if (setup) setup();
    for (auto& r : routes_) set(r.line_.id, r.armed_);
}

void Routes::close() noexcept {
    sync::Lock guard{lock_};
    if (closed_) return;
    for (auto& route : routes_) {
        route.arm_locked(false);
        if (route.stop_) route.stop_();
    }
    closed_ = true;
}
void Irq::controller_closed() noexcept {
    closed_ = true;
    (void)notice_.signal();
    notice_.reset();
}
Irq::Irq(Line line, object::ref<Ctl> owner) noexcept
    : owner_(std::move(owner)), route_(line, Route::Handler::bind<&Irq::publish>(*this),
                                     Route::Stop::bind<&Irq::controller_closed>(*this)) {
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
    if (closed_ || route_.line_.routes.closed_ || generation_ == std::numeric_limits<u64>::max())
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
    if (closed_ || route_.line_.routes.closed_) return std::unexpected(Error::Closed);
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
    if (closed_ || route_.line_.routes.closed_) return std::unexpected(Error::Closed);
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

namespace irq {
Ctl::Ctl(mm::KSpace& vm, Cpus& cpus, WorkQueue& work, mm::Phys::Lease&& claim, IrqConfig config) noexcept
    : vm_(vm), cpus_(cpus), work_(work), claim_(std::move(claim)), config_(config),
      routes_(Routes::Enable::bind<&Ctl::set>(*this)) {}
Ctl::~Ctl() noexcept { libk_assert(!open_ && !cleanup_); }
auto Ctl::word(usize offset) const noexcept -> volatile u32* {
    return reinterpret_cast<volatile u32*>(mapping_.address() + offset);
}
auto Ctl::open() noexcept -> status_t {
    const auto bytes = (0x200008 + config_.context * 0x1000 + 4095) & ~usize{4095};
    const auto pages = mm::Pages::from_aligned_bytes(mm::pa_t{config_.address}, bytes);
    if (!pages || !claim_.covers(*pages)) return STATUS_BAD_ARGS;
    if (!vm_.map(mapping_, *pages)) return STATUS_NO_MEMORY;
    for (usize i = 0; i <= config_.sources / 32; ++i)
        *word(0x2000 + config_.context * 0x80 + i * 4) = 0;
    *word(0x200000 + config_.context * 0x1000) = 0;
    arch::io_fence();
    work_.open(job_, Work::Fn::bind<&Ctl::progress>(*this));
    open_ = true;
    cpus_.attach_irq(*this);
    return STATUS_OK;
}
auto Ctl::line(u32 id) noexcept -> std::expected<Line, status_t> {
    sync::Lock guard{routes_.lock_};
    if (routes_.closed_) return std::unexpected(STATUS_CLOSED);
    if (!id || id > config_.sources) return std::unexpected(STATUS_BAD_ARGS);
    return Line{routes_, id};
}
void Ctl::set(u32 id, bool enable) noexcept {
    arch::io_fence();
    auto* priority = word(id * 4);
    if (!*priority) *priority = 1;
    auto* bank = word(0x2000 + config_.context * 0x80 + (id / 32) * 4);
    if (enable) { *bank |= u32{1} << (id % 32); *priority = *priority; }
    else *bank &= ~(u32{1} << (id % 32));
    arch::io_fence();
}
auto Ctl::take() noexcept -> u32 {
    arch::io_fence();
    auto* claim = word(0x200004 + config_.context * 0x1000);
    const auto id = *claim;
    // PLIC completion must precede masking; driver ACK only rearms the source.
    if (id) *claim = id;
    arch::io_fence();
    return id;
}
void Ctl::dispatch() noexcept { routes_.dispatch(Routes::Take::bind<&Ctl::take>(*this)); }
void Ctl::retire(object::cleanup&& cleanup) noexcept {
    if (!open_) { cleanup.complete(); return; }
    cpus_.detach_irq(*this);
    routes_.close();
    cleanup_ = std::move(cleanup);
    flush_.emplace();
    vm_.unmap(mapping_, *flush_, cpus_, current_cpu().id);
    work_.post(job_);
}
void Ctl::progress() noexcept {
    if (!flush_->complete()) { (void)flush_->kick(cpus_); work_.post(job_); return; }
    vm_.release(mapping_, *flush_);
    flush_.reset();
    open_ = false;
    work_.close(job_);
    auto cleanup = std::move(cleanup_);
    cleanup.complete();
}
} // namespace irq

void Cpus::attach_irq(irq::Ctl& controller) noexcept {
    sync::Lock guard{irq_lock_};
    controller.next_ = interrupts_;
    interrupts_ = &controller;
}
void Cpus::detach_irq(irq::Ctl& controller) noexcept {
    sync::Lock guard{irq_lock_};
    for (auto** link = &interrupts_; *link; link = &(*link)->next_)
        if (*link == &controller) { *link = controller.next_; controller.next_ = nullptr; return; }
    libk_assert(false);
}
void Cpus::dispatch_irq(CpuId cpu) noexcept {
    sync::Lock guard{irq_lock_};
    for (auto* controller = interrupts_; controller; controller = controller->next_)
        if (controller->config_.cpu == cpu.raw) controller->dispatch();
}
