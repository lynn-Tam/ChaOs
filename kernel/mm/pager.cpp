#include <mm/pager.hpp>
#include <mm/mem.hpp>
#include <ipc/notification.hpp>
#include <limits>
#include <utility>

Pager::~Pager() noexcept {
    libk_assert((state_ == State::Open || state_ == State::Closed) && ready_.empty() &&
                claimed_.empty() && replies_ == 0 && !source_.attached() && !cleanup_ &&
                pins_.load<libk::MemoryOrder::Relaxed>() == 0);
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
    (void)pins_.fetch_add<libk::MemoryOrder::AcqRel>(1);
}
void Pager::unpin() noexcept {
    object::cleanup cleanup;
    {
        sync::Lock guard{lock_};
        const auto n = pins_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
        libk_assert(n);
        if (n == 1 && state_ == State::Closed) cleanup = std::move(cleanup_);
    }
    if (cleanup) cleanup.complete();
}

auto Pager::enqueue(Request &r, Req info) noexcept -> bool {
    sync::Lock guard{lock_};
    if (state_ != State::Open || r.pager || !r.mem->work_open_.load<libk::MemoryOrder::Acquire>())
        return false;
    r.mem->request_pin();
    pin();
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
    (void)source_.signal();
}

void Pager::unlink_worker(Request &r) noexcept {
    if (auto *w = r.worker) {
        sync::Lock guard{w->lock_};
        w->list_.erase(r);
        r.worker = nullptr;
    }
}

auto Pager::take(Request &r) noexcept -> Reply {
    if (r.info.id)
        claimed_.erase(r);
    else
        ready_.erase(r);
    unlink_worker(r);
    ++replies_;
    return Reply{r};
}

auto Pager::claim(Claims *worker) noexcept -> std::expected<Req, Error> {
    Pin pin{*this};
    sync::Lock guard{lock_};
    if (state_ == State::Forced || state_ == State::Closed) return std::unexpected(Error::Closed);
    if (ready_.empty()) return std::unexpected(Error::Busy);
    if (next_id_ == std::numeric_limits<u64>::max())
        return std::unexpected(Error::GenerationExhausted);
    auto &r = ready_.front();
    ready_.erase(r);
    r.info.id = ++next_id_;
    claimed_.push_back(r);
    if (worker) {
        sync::Lock claims{worker->lock_};
        r.worker = worker;
        worker->list_.push_back(r);
    }
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

auto Pager::Reply::commit() noexcept -> std::expected<void, Error> {
    if (!*this) return std::unexpected(Error::Stale);
    auto d = h_.release();
    {
        sync::Lock guard{d.pager->lock_};
        libk_assert(d.request->pager == d.pager && !d.request->route.is_linked());
        d.request->pager = nullptr;
        d.request = nullptr;
        libk_assert(d.pager->replies_);
        --d.pager->replies_;
        d.pager->settle();
    }
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
                d.mem->work_open_.load<libk::MemoryOrder::Acquire>()) {
                d.request->info.id = 0;
                ready_.push_back(*d.request);
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
    unpin(); // May free Pager; no member access follows.
}

auto Pager::requeue(u64 id) noexcept -> std::expected<void, Error> {
    auto r = reply(id);
    if (!r) return std::unexpected(r.error());
    return r->abort();
}

void Pager::abandon(Request &r, u64 id, Claims &worker) noexcept {
    bool queued{};
    {
        sync::Lock guard{lock_};
        if (r.pager != this || r.info.id != id || r.worker != &worker) return;
        claimed_.erase(r);
        unlink_worker(r);
        r.info.id = 0;
        ready_.push_back(r);
        queued = true;
    }
    if (queued) signal();
}

auto Pager::Claims::empty() const noexcept -> bool {
    sync::Lock guard{lock_};
    return list_.empty();
}

void Pager::Claims::release() noexcept {
    for (;;) {
        Request *r;
        Pager *p;
        mm::Mem *m;
        u64 id;
        {
            sync::Lock guard{lock_};
            if (list_.empty()) return;
            r = &list_.front();
            p = r->pager;
            m = r->mem;
            id = r->info.id;
            // Completion needs this list lock to unlink, so both storage and
            // transport are still pinned while we acquire the exit-walk borrow.
            m->request_pin();
            p->pin();
        }
        p->abandon(*r, id, *this);
        m->request_drop();
        p->unpin();
    }
}

void Pager::cancel(mm::Mem &mem) noexcept {
    Pin pin{*this};
    for (;;) {
        Reply r;
        {
            sync::Lock guard{lock_};
            for (auto *list : {&ready_, &claimed_}) {
                for (auto &candidate : *list) {
                    if (candidate.mem == &mem) {
                        r = take(candidate);
                        break;
                    }
                }
                if (r) break;
            }
        }
        if (!r) return;
        mem.cancel_request(r);
    }
}

auto Pager::close(bool force) noexcept -> bool {
    Pin pin{*this};
    {
        sync::Lock guard{lock_};
        if (state_ == State::Closed) return true;
        if (force)
            state_ = State::Forced;
        else if (state_ == State::Open)
            state_ = State::Closing;
        source_.reset();
        settle();
    }
    for (;;) {
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
}

void Pager::retire(object::cleanup &&cleanup) noexcept {
    Pin pin{*this};
    bool force;
    {
        sync::Lock guard{lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(cleanup);
        force = state_ == State::Forced;
    }
    (void)close(force);
}

auto Pager::bind(ipc::Notification &n, u64 badge) noexcept -> std::expected<void, Error> {
    Pin pin{*this};
    if (!badge) return std::unexpected(Error::InvalidRange);
    bool ready;
    {
        sync::Lock guard{lock_};
        if (state_ != State::Open || !n.bind(source_, badge)) return std::unexpected(Error::Busy);
        ready = !ready_.empty();
    }
    if (ready) signal();
    return {};
}

auto Pager::unbind() noexcept -> bool {
    Pin pin{*this};
    sync::Lock guard{lock_};
    const bool attached = source_.attached();
    source_.reset();
    return attached;
}
