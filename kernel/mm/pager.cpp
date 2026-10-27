#include <mm/pager.hpp>
#include <mm/mem.hpp>
#include <uapi/ipc.h>
#include <limits>
#include <utility>

Pager::~Pager() noexcept {
    libk_assert((state_ == State::Open || state_ == State::Closed) && ready_.empty() &&
                claimed_.empty() && replies_ == 0 && !cleanup_ &&
                pins_.load(std::memory_order_relaxed) == 0);
}

auto Pager::state() const noexcept -> State {
    sync::Lock guard{lock_};
    return state_;
}

auto Pager::pending() const noexcept -> usize {
    sync::Lock guard{lock_};
    return ready_.size();
}

void Pager::settle() noexcept {
    if (state_ != State::Open && ready_.empty() && claimed_.empty() && !replies_)
        state_ = State::Closed;
}

void Pager::pin() noexcept {
    (void)pins_.fetch_add(1, std::memory_order_acq_rel);
}
void Pager::unpin() noexcept {
    object::cleanup cleanup;
    {
        sync::Lock guard{lock_};
        const auto n = pins_.fetch_sub(1, std::memory_order_acq_rel);
        libk_assert(n);
        if (n == 1 && state_ == State::Closed && events_.drained()) cleanup = std::move(cleanup_);
    }
    if (cleanup) { work_->close(job_); cleanup.complete(); }
}

void Pager::complete_cleanup() noexcept {
    object::cleanup cleanup;
    {
        sync::Lock guard{lock_};
        if (state_ != State::Closed || pins_.load(std::memory_order_acquire) || !events_.drained()) return;
        cleanup = std::move(cleanup_);
    }
    if (cleanup) { work_->close(job_); cleanup.complete(); }
}

void Pager::refresh(bool changed) noexcept {
    Source::Publication publication;
    {
        sync::Lock guard{lock_};
        const u64 bits = (ready_.empty() ? 0 : SOURCE_READY)
            | (state_ == State::Forced || state_ == State::Closed ? SOURCE_TERMINAL : 0);
        if (changed || events_.snapshot().value != bits) publication = events_.prepare(bits, true);
    }
}

auto Pager::enqueue(Request &r, Req info) noexcept -> bool {
    sync::Lock guard{lock_};
    if (state_ != State::Open || r.pager || !r.mem->work_open_.load(std::memory_order_acquire))
        return false;
    r.mem->request_pin();
    pin();
    info.key = r.mem->id();
    r.info = info;
    r.info.id = 0;
    r.pager = this;
    ready_.push_back(r);
    return true;
}

auto Pager::active(const Request &r) const noexcept -> bool {
    sync::Lock guard{lock_};
    return r.pager != nullptr;
}

void Pager::signal() noexcept {
    refresh();
}

auto Pager::take(Request &r) noexcept -> Reply {
    if (r.info.id)
        claimed_.erase(r);
    else
        ready_.erase(r);
    ++replies_;
    return Reply{r};
}

auto Pager::claim(cap::Graph::Admission admission, std::span<const object::ref<>*> gates) noexcept -> std::expected<Req, Error> {
    Pin pin{*this};
    sync::Lock guard{lock_};
    if (state_ == State::Forced || state_ == State::Closed) return std::unexpected(Error::Closed);
    if (ready_.empty()) return std::unexpected(Error::Busy);
    if (next_id_ == std::numeric_limits<u64>::max())
        return std::unexpected(Error::GenerationExhausted);
    auto &r = ready_.front();
    if (admission.publish(gates, [&] noexcept {
        ready_.erase(r);
        r.info.id = ++next_id_;
        claimed_.push_back(r);
    }) != STATUS_OK) return std::unexpected(Error::Closed);
    return r.info;
}

auto Pager::reply(u64 id, mm::Mem *mem) noexcept -> std::expected<Reply, Error> {
    Pin pin{*this};
    sync::Lock guard{lock_};
    for (auto &r : claimed_) {
        if (id && r.info.id == id && (!mem || r.mem == mem)) return take(r);
    }
    return std::unexpected(Error::Stale);
}

auto Pager::Reply::commit(cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates, Source::Action commit, bool fail) noexcept -> std::expected<void, Error> {
    if (!*this) return std::unexpected(Error::Stale);
    auto d = h_.get();
    sync::Lock guard{d.pager->lock_};
    libk_assert(d.request->pager == d.pager && !d.request->route.is_linked());
    if (!fail && (d.pager->state_ == State::Forced || d.pager->state_ == State::Closed))
        return std::unexpected(Error::Closed);
    if (admission.publish(gates, [&] noexcept {
        if (commit) commit();
        d.request->pager = nullptr;
        d.request = nullptr;
        libk_assert(d.pager->replies_);
        --d.pager->replies_;
        d.pager->settle();
    }) != STATUS_OK) return std::unexpected(Error::Closed);
    (void)h_.release();
    h_ = libk::unique_handle<Data, Drop, Data>{d};
    return {};
}

void Pager::Reply::Drop::operator()(Data &d) const noexcept {
    d.pager->dispose(d);
}

auto Pager::Reply::abort() noexcept -> std::expected<void, Error> {
    if (!*this) return std::unexpected(Error::Stale);
    h_.reset();
    return {};
}

void Pager::dispose(Reply::Data &d) noexcept {
    if (d.request) {
        bool requeued{};
        {
            sync::Lock guard{lock_};
            if (state_ != State::Forced && state_ != State::Closed &&
                d.mem->work_open_.load(std::memory_order_acquire)) {
                if (d.request->info.id) claimed_.push_back(*d.request);
                else ready_.push_back(*d.request);
                --replies_;
                requeued = true;
            }
        }
        if (requeued) {
            signal();
            return; // The queue adopts the original Mem and Pager pins.
        }
        Reply terminal;
        terminal.h_ = libk::unique_handle<Reply::Data, Reply::Drop, Reply::Data>{d};
        d = {};
        terminal.mem()->cancel_request(terminal);
        return;
    }
    auto *mem = d.mem;
    mem->request_drop();
    unpin(); // May free Pager; no member perms follows.
}

auto Pager::requeue(u64 id, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates) noexcept -> std::expected<void, Error> {
    Pin pin{*this};
    sync::Lock guard{lock_};
    for (auto& r : claimed_) {
        if (!id || r.info.id != id) continue;
        if (admission.publish(gates, [&] noexcept {
            claimed_.erase(r);
            r.info.id = 0;
            ready_.push_back(r);
        }) != STATUS_OK) return std::unexpected(Error::Closed);
        return {};
    }
    return std::unexpected(Error::Stale);
}

void Pager::cancel(Request& request) noexcept {
    Pin pin{*this};
    Reply reply;
    {
        sync::Lock guard{lock_};
        // A detached Reply already owns the request until its real exit.
        if (request.pager == this && request.route.is_linked()) reply = take(request);
    }
    if (reply) reply.mem()->cancel_request(reply);
}

auto Pager::close(bool force) noexcept -> bool {
    Pin pin{*this};
    std::array<Buffer, 8> buffers;
    {
        sync::Lock guard{lock_};
        if (force) buffers = std::move(buffers_);
        if (state_ == State::Closed) return true;
        if (force) {
            state_ = State::Forced;
        }
        else if (state_ == State::Open)
            state_ = State::Closing;
        settle();
    }
    for (usize step = 0; step < 8; ++step) {
        Reply r;
        {
            sync::Lock guard{lock_};
            if (state_ != State::Forced) return state_ == State::Closed;
            if (!ready_.empty())
                r = take(ready_.front());
            else if (!claimed_.empty())
                r = take(claimed_.front());
            else {
                settle();
                return state_ == State::Closed;
            }
        }
        r.mem()->cancel_request(r);
    }
    bool more{}, done{};
    {
        sync::Lock guard{lock_};
        settle();
        done = state_ == State::Closed;
        more = state_ == State::Forced && (!ready_.empty() || !claimed_.empty());
    }
    if (more) { libk_assert(work_); work_->post(job_); }
    return done;
}

void Pager::retire(object::cleanup &&cleanup) noexcept {
    Pin pin{*this};
    {
        sync::Lock guard{lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(cleanup);
    }
    events_.close();
    (void)close(true);
}

void Pager::attach(Binding& binding) noexcept {
    { sync::Lock guard{lock_}; bindings_.insert(binding); ++generation_; }
    refresh(true);
}
void Pager::detach(Binding& binding) noexcept {
    { sync::Lock guard{lock_}; if (binding.hook.is_linked()) { bindings_.erase(binding); ++generation_; } }
    refresh(true);
}
auto Pager::find(u64 key) noexcept -> object::ref<> {
    sync::Lock guard{lock_};
    auto* binding = bindings_.find(key);
    return binding ? object::ref<>::retain_alive(binding->storage) : object::ref<>{};
}
auto Pager::next(u64 key) noexcept -> object::ref<> {
    sync::Lock guard{lock_};
    auto* next = bindings_.lower_bound(key);
    if (next && next->key == key) next = bindings_.next(*next);
    return next ? object::ref<>::retain_alive(next->storage) : object::ref<>{};
}

auto Pager::memory(u64 id) noexcept -> object::ref<> {
    sync::Lock guard{lock_};
    for (auto& r : claimed_) if (r.info.id == id) return object::ref<>::retain_alive(r.mem->binding_.storage);
    return {};
}

auto Pager::buffer(object::ref<> memory, usize page) noexcept -> std::expected<usize, Error> {
    sync::Lock guard{lock_};
    if (state_ != State::Open) return std::unexpected(Error::Closed);
    for (auto& b : buffers_) if (b.memory && b.memory.id() == memory.id() && b.page == page) return std::unexpected(Error::Busy);
    for (usize i = 0; i < buffers_.size(); ++i) if (!buffers_[i].memory) {
        buffers_[i] = {std::move(memory), page}; return i + 1;
    }
    return std::unexpected(Error::Busy);
}
auto Pager::buffer(usize slot, bool remove) noexcept -> Buffer {
    sync::Lock guard{lock_};
    if (!slot || slot > buffers_.size()) return {};
    auto& b = buffers_[slot - 1];
    return remove ? std::move(b) : Buffer{b.memory, b.page};
}

void Pager::Reservation::Drop::operator()(Data& d) const noexcept {
    sync::Lock guard{d.pager->lock_}; libk_assert(d.bytes <= d.pager->logical_); d.pager->logical_ -= d.bytes;
}
auto Pager::reserve(usize bytes) noexcept -> std::expected<Reservation, Error> {
    sync::Lock guard{lock_};
    if (state_ != State::Open) return std::unexpected(Error::Closed);
    if (bytes > limit_ - logical_) return std::unexpected(Error::InvalidRange);
    logical_ += bytes; return Reservation{*this, bytes};
}
auto Pager::configure(usize limit) noexcept -> bool {
    sync::Lock guard{lock_}; if (limit < logical_) return false; limit_ = limit; return true;
}
auto Pager::info() const noexcept -> PagerInfo {
    sync::Lock guard{lock_}; return {1, sizeof(PagerInfo), logical_, limit_, generation_, bindings_.size()};
}
