#include <mm/slab.hpp>
#include <ipc/port.hpp>
#include <object/pool.hpp>
#include <libk/checked_arithmetic.hpp>

namespace ipc {

const cap::AttachOps Port::attach_ops_{
    .invalidate = [](void* p, object::LinkWork&& work) noexcept {
        auto& watch = *static_cast<Watch*>(p);
        watch.owner.close(watch, false, std::move(work));
    },
    .released = [](void* p, object::LinkWork&& done) noexcept {
        auto& watch = *static_cast<Watch*>(p);
        auto& owner = watch.owner;
        Flight flight{owner};
        {
            sync::Lock guard{owner.lock_};
            ++watch.calls;
            done.reset();
            watch.grant_done = true;
        }
        { sync::Lock guard{owner.lock_}; --watch.calls; }
        owner.finish(watch);
    },
    .retain = false,
    .observe = true,
};

Port::Watch::Watch(Port& port) noexcept
    : owner(port), attachment(this, attach_ops_),
      source(libk::delegate<void(WaitResult) noexcept>::bind<&Watch::deliver>(*this),
             Source::Drained::bind<&Watch::drained>(*this)) {}
void Port::Watch::deliver(WaitResult result) noexcept {
    Flight flight{owner};
    if (owner.emit(*this, result)) owner.close(*this, false);
}
void Port::Watch::drained() noexcept {
    Flight flight{owner};
    { sync::Lock guard{owner.lock_}; source_done = true; }
    owner.finish(*this);
}

Port::Port(mm::Pmm& pmm, Config config) noexcept
    : config_(config), storage_(pmm, mm::Slab<Watch, false>::quota_for(config.watches)) {}
Port::~Port() noexcept {
    libk_assert(!cleanup_ && queue_.empty() && index_.empty());
    while (!watches_.empty()) {
        auto& watch = watches_.pop_front();
        libk_assert(watch.state == Watch::State::Free);
        if (watch.queue.is_linked()) free_.erase(watch);
        storage_.destroy(watch);
    }
}
auto Port::storage_bytes(Config config) noexcept -> std::optional<usize> {
    if (!config.watches || !config.queue || config.queue > config.watches || config.watches > UINT32_MAX)
        return {};
    return libk::checked_multiply(mm::Slab<Watch, false>::quota_for(config.watches).pages, mm::page_size);
}
auto Port::open() noexcept -> status_t {
    if (!storage_bytes(config_) || opened_) return STATUS_BAD_ARGS;
    usize budget = 64;
    while (watches_.size() < config_.watches) {
        if (!budget) return STATUS_PENDING;
        --budget;
        auto made = storage_.create(quota_, *this);
        if (!made) return STATUS_NO_MEMORY;
        watches_.push_back(**made); free_.push_back(**made);
    }
    if (!initialization_.query().sequence) { opened_ = true; initialization_.accept(); initialization_.finish(); }
    return STATUS_OK;
}
auto Port::find(u64 id) noexcept -> Watch* {
    libk_assert(lock_.held());
    return index_.find(id);
}
auto Port::update() noexcept -> Source::Publication {
    libk_assert(lock_.held());
    const u64 bits = (queue_.empty() ? 0 : PORT_READABLE) | (overflow_ ? PORT_OVERFLOW : 0);
    if (std::exchange(readiness_, bits) == bits) return {};
    return ready_.prepare(bits, true);
}

auto Port::watch(cap::Resolved<Port>& port, const cap::GrantPin& grant, Source& source,
    u32 facet, u64 key, Source::Condition condition, Bound bound) noexcept -> std::expected<u64, status_t> {
    Flight flight{*this};
    auto target = grant.clone_target(), receiver = port.reference();
    if (!target || !receiver) return std::unexpected(STATUS_CLOSED);
    const auto id = object::new_id<Watch>().value;
    if (!id) return std::unexpected(STATUS_NO_MEMORY);
    Watch* cell{};
    {
        sync::Lock guard{lock_};
        if (!opened_ || closing_) return std::unexpected(STATUS_CLOSED);
        if (!free_.empty()) cell = &free_.pop_front();
        if (!cell) return std::unexpected(STATUS_NO_MEMORY);
        cell->state = Watch::State::Prepared;
        cell->condition = condition; cell->bound = bound;
        cell->event = {.version = 1, .size = sizeof(PortEvent), .source_id = grant.target_id().value,
            .key = key, .watch_gen = id, .sequence = 0, .condition = 0, .facet = facet, .flags = 0};
        index_.insert(*cell);
    }
    std::array<const cap::GrantPin*, 2> grants{&grant, &port.pin()};
    std::array<const object::ref<>*, 2> gates{&*target, &*receiver};
    const auto admission = grant.graph().admission(grants);
    const auto status = source.watch(cell->source, condition, [&](auto&& register_source) noexcept {
        std::optional<sync::Lock<sync::Spin>> structure;
        if (bound.scope) {
            structure.emplace(object::Control::lock);
            if (!bound.scope->find(bound.object, bound.view.kinds, bound.view.depth)) return STATUS_STALE;
        }
        sync::Lock guard{lock_};
        if (closing_) return STATUS_CLOSED;
        auto commit = [&]() noexcept { register_source(); cell->state = Watch::State::Live; changed(); };
        auto attached = admission.attach(grant, cell->attachment, gates, libk::delegate<void() noexcept>::bind(commit));
        return attached ? STATUS_OK : STATUS_CLOSED;
    });
    if (status != STATUS_OK) {
        sync::Lock guard{lock_};
        cell->state = Watch::State::Free; index_.erase(*cell); free_.push_back(*cell);
        return std::unexpected(status);
    }
    return id;
}

auto Port::arm(u64 id, u64 seen) noexcept -> WaitResult {
    std::expected<cap::GrantPin, cap::GrantError> grant = std::unexpected(cap::GrantError::InvalidState);
    Watch* watch;
    Source* source;
    Source::Pin pin;
    libk::scope_exit release{[&]() noexcept { if (pin) pin(false); }};
    Source::Condition condition;
    Bound bound;
    {
        sync::Lock guard{lock_};
        watch = find(id);
        if (!watch) return {STATUS_STALE};
        if (watch->state != Watch::State::Live) return {STATUS_CLOSED};
        grant = watch->attachment.acquire();
        source = watch->source.source();
        pin = source->retain();
        condition = watch->condition;
        bound = watch->bound;
        condition.seen = seen;
    }
    if (!grant) return {STATUS_CLOSED};
    WaitResult observed;
    const auto status = source->arm(watch->source, condition, [&](auto&& commit) noexcept {
        std::optional<sync::Lock<sync::Spin>> structure;
        if (bound.scope) {
            structure.emplace(object::Control::lock);
            if (!bound.scope->find(bound.object, bound.view.kinds, bound.view.depth)) return STATUS_STALE;
        }
        sync::Lock guard{lock_};
        if (closing_ || watch->event.watch_gen != id || watch->erase || watch->state != Watch::State::Live) return STATUS_STALE;
        commit(); return STATUS_OK;
    }, &observed);
    return status == STATUS_OK ? observed : WaitResult{status};
}

auto Port::emit(Watch& watch, WaitResult result) noexcept -> bool {
    Source::Publication publication;
    std::optional<sync::Lock<sync::Spin>> structure;
    if (watch.bound.scope) {
        structure.emplace(object::Control::lock);
        if (!watch.bound.scope->find(watch.bound.object, watch.bound.view.kinds, watch.bound.view.depth))
            result.status = STATUS_CLOSED;
    }
    {
        sync::Lock guard{lock_};
        if (closing_ || watch.erase || watch.state == Watch::State::Free) return false;
        const bool terminal = result.status == STATUS_CLOSED || watch.state == Watch::State::Closing
            || (watch.event.flags & PORT_EVENT_CLOSED);
        if (!terminal && (watch.state != Watch::State::Live || watch.event.sequence > result.sequence)) return false;
        watch.event.sequence = std::max(watch.event.sequence, result.sequence);
        // A Source flight already owns this hint, even if attachment closes
        // before its callback reaches the Port. Keep closure and real readiness.
        watch.event.condition = result.value | (terminal
            && (watch.queue.is_linked() || watch.pending > acknowledged_) ? watch.event.condition : 0);
        if (terminal) watch.event.flags |= PORT_EVENT_CLOSED;
        changed(); watch.pending = version_;
        if (!watch.queue.is_linked()) {
            if (queue_.size() < config_.queue) queue_.push_back(watch);
            else { overflow_ = true; lost_ = version_; }
        }
        publication = update();
    }
    return result.status == STATUS_CLOSED;
}

void Port::close(Watch& watch, bool erase, object::LinkWork work) noexcept {
    Flight flight{*this};
    bool detach{};
    {
        sync::Lock guard{lock_};
        if (watch.state == Watch::State::Free) return;
        ++watch.calls;
        watch.erase |= erase;
        if (work) watch.work = std::move(work);
        if (watch.state == Watch::State::Live) { watch.state = Watch::State::Closing; detach = true; }
    }
    if (detach) {
        emit(watch, {STATUS_CLOSED});
        watch.source.detach();
    }
    { sync::Lock guard{lock_}; --watch.calls; }
    finish(watch);
}

void Port::finish(Watch& watch) noexcept {
    Flight flight{*this};
    for (;;) {
        bool detach;
        object::LinkWork work;
        {
            sync::Lock guard{lock_};
            if (watch.state != Watch::State::Closing && watch.state != Watch::State::Terminal) return;
            if (!watch.source_done || watch.calls || watch.finishing) return;
            watch.finishing = true;
            ++watch.calls;
            detach = !std::exchange(watch.detach_started, true);
            work = std::move(watch.work);
        }
        const auto done = detach && watch.attachment.detach();
        work.reset();
        Source::Publication publication;
        bool again;
        {
            sync::Lock guard{lock_};
            watch.grant_done |= done;
            --watch.calls;
            watch.finishing = false;
            again = bool(watch.work);
            if (!again) {
                if (watch.calls || !watch.grant_done || watch.attachment.busy()) return;
                watch.state = Watch::State::Terminal;
                if (watch.erase || closing_) {
                    if (watch.queue.is_linked()) queue_.erase(watch);
                    watch.attachment.reset();
                    watch.state = Watch::State::Free; index_.erase(watch); free_.push_back(watch);
                    watch.source_done = watch.detach_started = watch.grant_done = watch.erase = false;
                    watch.pending = 0;
                    changed();
                    if (!closing_) publication = update();
                }
            }
        }
        if (!again) return;
    }
}

auto Port::unwatch(u64 id) noexcept -> status_t {
    Watch* watch;
    Source::Publication publication;
    {
        sync::Lock guard{lock_};
        watch = find(id);
        if (!watch || watch->erase) return STATUS_STALE;
        ++watch->calls; watch->erase = true;
        // Logical removal owns all queued hints immediately; external Grant
        // detach may finish later without exposing an unregistered event.
        if (watch->queue.is_linked()) queue_.erase(*watch);
        changed(); publication = update();
    }
    close(*watch, true);
    { sync::Lock guard{lock_}; --watch->calls; }
    finish(*watch);
    return STATUS_OK;
}
auto Port::take(PortEvent& event) noexcept -> status_t {
    Source::Publication publication;
    {
        sync::Lock guard{lock_};
        if (closing_) return STATUS_CLOSED;
        if (queue_.empty()) return STATUS_RETRY;
        auto& watch = queue_.pop_front();
        event = watch.event;
        watch.pending = 0;
        changed();
        publication = update();
    }
    return STATUS_OK;
}
auto Port::records(u64 epoch, u64 last, u64 limit, PortRecords& result) const noexcept -> status_t {
    sync::Lock guard{lock_};
    if (version_ == UINT64_MAX || epoch > version_) return STATUS_STALE;
    if (!epoch) { epoch = version_; const auto* end = index_.maximum(); limit = end ? end->event.watch_gen : 0; }
    result = {.version = 2, .size = sizeof(PortRecords), .epoch = epoch, .next = last, .limit = limit, .count = 0, .more = 0, .events = {}};
    auto* watch = last < limit ? index_.lower_bound(last + 1) : nullptr;
    for (usize step = 0; watch && watch->event.watch_gen <= limit && step < PORT_RECORDS; ++step) {
        result.next = watch->event.watch_gen;
        if (!watch->erase && watch->state != Watch::State::Prepared) {
            auto event = watch->event;
            if (watch->queue.is_linked() || watch->pending > acknowledged_) event.flags |= PORT_EVENT_PENDING;
            result.events[result.count++] = event;
        }
        watch = index_.next(*watch);
    }
    result.more = watch && watch->event.watch_gen <= limit;
    return STATUS_OK;
}
auto Port::resync(u64 epoch) noexcept -> status_t {
    Source::Publication publication;
    {
        sync::Lock guard{lock_};
        if (!epoch || version_ == UINT64_MAX || epoch > version_) return STATUS_STALE;
        acknowledged_ = std::max(acknowledged_, epoch);
        overflow_ = lost_ > acknowledged_;
        changed();
        publication = update();
    }
    return STATUS_OK;
}

void Port::retire(object::cleanup&& cleanup) noexcept {
    Flight flight{*this};
    { sync::Lock guard{lock_}; closing_ = true; cleanup_ = std::move(cleanup); }
    ready_.close();
    work_->post(job_);
}
void Port::finish_call(bool release) noexcept {
    auto execution = object::ref<>::retain_alive(job_.key());
    bool post;
    {
        sync::Lock guard{lock_};
        if (release) { libk_assert(calls_); --calls_; }
        post = closing_ && !calls_ && bool(cleanup_);
    }
    if (post) work_->post(job_);
}
void Port::run_work() noexcept {
    const auto phase = initialization_.query().sequence;
    if (phase && phase != OP_DONE && initialization_.schedule()) return;
    advance();
}

void Port::advance() noexcept {
    bool retiring;
    {
        sync::Lock guard{lock_};
        if (advancing_) { post(); return; }
        advancing_ = true; retiring = closing_;
    }
    libk::scope_exit advanced{[&]() noexcept { sync::Lock guard{lock_}; advancing_ = false; }};
    const auto phase = initialization_.query().sequence;
    if (phase && phase != OP_DONE) {
        if (retiring) (void)initialization_.cancel();
        if (init_status_ == STATUS_PENDING) {
            init_status_ = initialization_.canceled() ? STATUS_CANCELED : open();
            if (init_status_ == STATUS_PENDING) { initialization_.post(); return; }
            if (init_status_ == STATUS_OK) {
                if (initialization_.commit()) { sync::Lock guard{lock_}; opened_ = true; }
                else init_status_ = STATUS_CANCELED;
            }
        }
        if (!initialization_.detach()) { initialization_.post(); return; }
        initialization_.finish(init_status_);
        if (retiring) post();
        return;
    }
    if (!retiring) return;
    for (usize step = 0; step < 8; ++step) {
        Watch* watch;
        {
            sync::Lock guard{lock_};
            watch = index_.lower_bound(scan_);
            if (!watch) { scanned_ = true; break; }
            scan_ = watch->event.watch_gen + 1; ++watch->calls;
        }
        close(*watch, true);
        { sync::Lock guard{lock_}; --watch->calls; }
        finish(*watch);
    }
    {
        sync::Lock guard{lock_};
        if (!scanned_) { guard.restore(); work_->post(job_); return; }
        if (calls_ || !index_.empty() || !ready_.drained()) return;
    }
    for (usize step = 0; step < 8 && !watches_.empty(); ++step) {
        auto& watch = watches_.pop_front();
        libk_assert(watch.state == Watch::State::Free);
        free_.erase(watch); storage_.destroy(watch);
    }
    if (!watches_.empty() || !storage_.drain(8)) { work_->post(job_); return; }
    object::cleanup cleanup;
    { sync::Lock guard{lock_}; cleanup = std::move(cleanup_); }
    work_->close(job_); initialization_.retire(std::move(cleanup));
}

} // namespace ipc
