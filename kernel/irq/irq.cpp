#include <io.hpp>
#include <expected>
#include <irq/irq.hpp>
#include <ipc/notification.hpp>
#include <uapi/ipc.h>
#include <limits>
#include <cpu/cpu.hpp>

namespace irq {
auto Ctl::open() noexcept -> status_t {
    const auto pages = mm::Pages::from_aligned_bytes(mm::pa_t{config_.address}, arch::plic_bytes(config_.context));
    if (!pages || !claim_.covers(*pages)) return STATUS_BAD_ARGS;
    if (!vm_.map(mapping_, *pages, cpus_, current_cpu().id)) return STATUS_NO_MEMORY;
    mapped_ = true; work_.open(job_, Work::Fn::bind<&Ctl::progress>(*this)); return STATUS_OK;
}
void Ctl::activate() noexcept {
    auto commit = [&]() noexcept {
        arch::plic_init(mapping_.address(), config_.context, config_.sources);
        { sync::Lock guard{routes_.lock_}; open_ = true; }
        cpus_.attach_irq(*this);
    };
    static_cast<void>(bindings_.publish(Source::Action::bind(commit)));
}
void Ctl::set(u32 id, bool enable) noexcept { arch::plic_set(mapping_.address(), config_.context, id, enable); }
auto Ctl::take() noexcept -> u32 { return arch::plic_take(mapping_.address(), config_.context); }


auto Route::connect_locked(bool armed) noexcept -> bool {
    if (line_.routes.closed_ || hook_.is_linked() || line_.id == 0 || !handler_) return false;
    for (auto& r : line_.routes.routes_)
        if (r.line_.id == line_.id) return false;
    stopped_ = false;
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
    Source::Publication publication;
    sync::Lock guard{lock_};
    if (closed_) return;
    const u32 id = take();
    if (id == 0) return;
    for (auto& r : routes_) {
        if (r.line_.id != id) continue;
        r.arm_locked(false);
        auto delivered = r.handler_();
        publication = std::move(delivered.second);
        const bool rearm = delivered.first;
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
    {
        sync::Lock guard{lock_};
        if (closed_) return;
        for (auto& route : routes_) route.arm_locked(false);
        closed_ = true;
    }
    for (;;) {
        Source::Publication publication;
        bool stopped{};
        {
            sync::Lock guard{lock_};
            for (auto& route : routes_) {
                if (route.stopped_) continue;
                route.stopped_ = stopped = true;
                if (route.stop_) publication = route.stop_();
                break;
            }
        }
        if (!stopped) return;
    }
}
auto Irq::controller_closed() noexcept -> Source::Publication {
    Source::Publication publication;
    closed_ = true;
    publication = events_.prepare(SOURCE_TERMINAL, true);
    return publication;
}
Irq::Irq(Line line, object::ref<Ctl> owner) noexcept
    : owner_(std::move(owner)), route_(line, Route::Handler::bind<&Irq::publish>(*this),
                                     Route::Stop::bind<&Irq::controller_closed>(*this)) {
    libk_assert(line.id != 0);
}

Irq::~Irq() noexcept { (void)close(); }

void Irq::links(u64 cursor, LinkRecords& out) const noexcept {
    bindings_.links(out, LINK_ROUTE, cursor);
    for (auto& row : std::span{out.records, out.count}) { row.first = route_.line_.id; row.bytes = 1; }
}

auto Irq::bound() const noexcept -> bool {
    sync::Lock guard{route_.line_.routes.lock_};
    return route_.hook_.is_linked();
}

auto Irq::pending() const noexcept -> bool {
    sync::Lock guard{route_.line_.routes.lock_};
    return pending_;
}

auto Irq::closed() const noexcept -> bool {
    sync::Lock guard{route_.line_.routes.lock_};
    return closed_;
}

auto Irq::enable(bool enabled) noexcept -> std::expected<u64, Error> {
    Source::Publication publication;
    auto result = std::expected<u64, Error>{std::unexpected(Error::Closed)};
    auto apply = [&]() noexcept -> std::expected<u64, Error> {
        sync::Lock guard{route_.line_.routes.lock_};
        if (closed_ || route_.line_.routes.closed_) return std::unexpected(Error::Closed);
        if (route_.hook_.is_linked() == enabled) return generation_;
        if (!enabled) { route_.reset_locked(); return generation_; }
        if (generation_ == std::numeric_limits<u64>::max()) return std::unexpected(Error::Closed);
        if (!route_.connect_locked(!pending_)) return std::unexpected(Error::Busy);
        ++generation_;
        publication = events_.prepare(pending_ ? SOURCE_READY : 0, true);
        return generation_;
    };
    auto commit = [&]() noexcept { result = apply(); };
    if (!bindings_.publish(Source::Action::bind(commit))) return std::unexpected(Error::Closed);
    if (result) object::changed(key_);
    return result;
}

auto Irq::delivery() const noexcept -> std::expected<Delivery, Error> {
    sync::Lock guard{route_.line_.routes.lock_};
    if (closed_ || route_.line_.routes.closed_) return std::unexpected(Error::Closed);
    if (!pending_ || !route_.hook_.is_linked()) return std::unexpected(Error::InvalidState);
    return (Delivery{sequence_, generation_});
}

auto Irq::publish() noexcept -> Route::Delivery {
    // Routes holds a claimed, masked line and the ownership lock through
    // publication; the controller retains its own EOI timing.
    if (closed_) return {false, {}};
    if (sequence_ == std::numeric_limits<u64>::max()) {
        closed_ = true;
        route_.reset_locked();
            return {false, events_.prepare(SOURCE_TERMINAL, true)};
    }
    ++sequence_;
    pending_ = true;
    return {false, events_.prepare(SOURCE_READY, true)};
}

auto Irq::ack(u64 generation, u64 sequence) noexcept -> std::expected<void, Error> {
    Source::Publication publication;
    auto result = std::expected<void, Error>{std::unexpected(Error::Closed)};
    auto apply = [&]() noexcept -> std::expected<void, Error> {
        sync::Lock guard{route_.line_.routes.lock_};
        if (closed_ || route_.line_.routes.closed_) return std::unexpected(Error::Closed);
        if (!pending_ || !route_.hook_.is_linked()) return std::unexpected(Error::InvalidState);
        if (generation != generation_)
        return std::unexpected(Error::StaleSequence);
        if (sequence == 0 || sequence > sequence_) return std::unexpected(Error::BadSequence);
        if (sequence < sequence_) return std::unexpected(Error::StaleSequence);
        pending_ = false;
        publication = events_.prepare(0, true);
        // The driver must first clear the device's interrupt condition. This
        // acknowledgement releases the software delivery, then enables the line.
        route_.arm_locked(true);
        return {};
    };
    auto commit = [&]() noexcept { result = apply(); };
    if (!bindings_.publish(Source::Action::bind(commit))) return std::unexpected(Error::Closed);
    return result;
}

auto Irq::close() noexcept -> bool {
    Source::Publication publication;
    bool changed;
    {
        sync::Lock guard{route_.line_.routes.lock_};
        route_.reset_locked(); changed = !closed_; closed_ = true;
        publication = events_.prepare(SOURCE_TERMINAL, true);
    }
    if (changed) object::changed(key_);
    static_cast<void>(bindings_.close());
    return true;
}

void Irq::retire(object::cleanup&& cleanup) noexcept {
    Source::Publication publication;
    {
        sync::Lock guard{route_.line_.routes.lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(cleanup);
        route_.reset_locked();
            closed_ = true;
        publication = events_.prepare(SOURCE_TERMINAL, true);
    }
    static_cast<void>(bindings_.close());
    // The terminal publication keeps cleanup blocked through this call.
    events_.close();
}

void Irq::complete_cleanup() noexcept {
    object::cleanup cleanup;
    if (!events_.drained() || !bindings_.drained()) return;
    {
        sync::Lock guard{route_.line_.routes.lock_};
        cleanup = std::move(cleanup_);
    }
    if (cleanup) cleanup.complete();
}

Ctl::Ctl(mm::KSpace& vm, Cpus& cpus, WorkQueue& work, mm::Phys::Claim&& claim, IrqConfig config) noexcept
    : vm_(vm), cpus_(cpus), work_(work), claim_(std::move(claim)), config_(config),
      routes_(Routes::Enable::bind<&Ctl::set>(*this)) {}
Ctl::~Ctl() noexcept { libk_assert(!open_ && !mapped_ && !cleanup_); }
void Ctl::links(u64 cursor, LinkRecords& out) const noexcept {
    bindings_.links(out, LINK_PHYS, cursor);
    for (auto& row : std::span{out.records, out.count}) { row.first = config_.address; row.bytes = config_.bytes; }
}
auto Ctl::line(u32 id) noexcept -> std::expected<Line, status_t> {
    sync::Lock guard{routes_.lock_};
    if (!open_ || routes_.closed_) return std::unexpected(STATUS_CLOSED);
    if (!id || id > config_.sources) return std::unexpected(STATUS_BAD_ARGS);
    return Line{routes_, id};
}
void Ctl::dispatch() noexcept { routes_.dispatch(Routes::Take::bind<&Ctl::take>(*this)); }
void Ctl::retire(object::cleanup&& cleanup) noexcept {
    cleanup_ = std::move(cleanup);
    if (open_) cpus_.detach_irq(*this);
    routes_.close();
    if (mapped_) {
        flush_.emplace();
        vm_.unmap(mapping_, *flush_, cpus_, current_cpu().id);
        work_.post(job_);
    } else progress();
}
void Ctl::progress() noexcept {
    if (!cleanup_) return;
    if (mapped_) {
        if (!flush_->complete()) { (void)flush_->kick(cpus_); work_.post(job_); return; }
        vm_.release(mapping_, *flush_);
        flush_.reset(); mapped_ = false; open_ = false;
    }
    claim_.reset();
    if (!bindings_.close()) return;
    work_.close(job_);
    auto cleanup = std::move(cleanup_);
    cleanup.complete();
}
} // namespace irq
