#pragma once

#include <base/types.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/unique_handle.hpp>
#include <libk/sync/atomic.hpp>
#include <sync.hpp>
#include <object/ref.hpp>
#include <ipc/notification_source.hpp>

namespace ipc {
class Notification;
}
namespace mm {
class Mem;
template<class> class Cache;
} // namespace mm

// Mem owns request storage and contents; Pager owns only delivery indexes.
class Pager final : private libk::noncopyable_nonmovable {
public:
    class Claims;
    enum class Kind : u8 { PageIn, Writeback };
    struct Req {
        u64 id{};
        Kind kind{Kind::PageIn};
        usize page_index{};
        usize first{};
        usize count{1};
        u64 dirty_epoch{};
        u8 urgency{};
    };
    struct Request {
        Req info{};
        mm::Mem *mem{};
        Pager *pager{};
        Claims *worker{};
        libk::IntrusiveListHook route{}, claim{};
    };
    enum class Error : u8 { Closed, Busy, Stale, InvalidRange, GenerationExhausted };
    enum class State : u8 { Open, Closing, Forced, Closed };

    class Reply final {
        friend class Pager;
        struct Data {
            Pager *pager{};
            mm::Mem *mem{};
            Request *request{};
            static auto empty() noexcept -> Data { return {}; }
            static auto is_empty(const Data &d) noexcept -> bool { return !d.pager; }
        };
        struct Drop {
            void operator()(Data &d) const noexcept;
        };
        libk::unique_handle<Data, Drop, Data> h_{};
        explicit Reply(Request &r) noexcept : h_(Data{r.pager, r.mem, &r}) {}

    public:
        Reply() noexcept = default;
        Reply(Reply &&) noexcept = default;
        auto operator=(Reply &&) noexcept -> Reply & = default;
        [[nodiscard]] explicit operator bool() const noexcept { return h_ && h_.get().request; }
        [[nodiscard]] auto req() const noexcept -> const Req & { return h_.get().request->info; }
        [[nodiscard]] auto request() const noexcept -> Request * { return h_.get().request; }
        [[nodiscard]] auto mem() const noexcept -> mm::Mem * { return h_.get().mem; }
        // Called under the Mem content lock. Detach is final; this Reply keeps
        // the old storage borrow until its destructor, outside the content lock.
        [[nodiscard]] auto commit() noexcept -> std::expected<void, Error>;
        [[nodiscard]] auto abort() noexcept -> std::expected<void, Error>;
    };

    Pager() noexcept = default;
    ~Pager() noexcept;
    [[nodiscard]] auto state() const noexcept -> State;
    [[nodiscard]] auto pending() const noexcept -> usize;
    [[nodiscard]] auto claim(Claims *worker = nullptr) noexcept -> std::expected<Req, Error>;
    [[nodiscard]] auto reply(u64 id, mm::Mem *mem = nullptr) noexcept
        -> std::expected<Reply, Error>;
    [[nodiscard]] auto requeue(u64 id) noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto bind(ipc::Notification &, u64 badge) noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto unbind() noexcept -> bool;
    [[nodiscard]] auto close(bool force) noexcept -> bool;
    void retire(object::cleanup &&) noexcept;

private:
    friend class mm::Mem;
    template<class> friend class mm::Cache;
    class Pin {
        Pager &pager_;

    public:
        explicit Pin(Pager &p) noexcept : pager_(p) { p.pin(); }
        ~Pin() noexcept { pager_.unpin(); }
    };
    using Routes = libk::IntrusiveList<Request, &Request::route>;
    [[nodiscard]] auto enqueue(Request &, Req) noexcept -> bool;
    [[nodiscard]] auto active(const Request &) const noexcept -> bool;
    void cancel(mm::Mem &) noexcept;
    void signal() noexcept;
    [[nodiscard]] auto take(Request &) noexcept -> Reply;
    void unlink_worker(Request &) noexcept;
    void abandon(Request &, u64 id, Claims &) noexcept;
    void dispose(Reply::Data &) noexcept;
    void settle() noexcept;
    void pin() noexcept;
    void unpin() noexcept;

    mutable sync::Spin lock_{};
    Routes ready_{}, claimed_{};
    usize replies_{};
    u64 next_id_{};
    ipc::NotificationSource source_{};
    State state_{State::Open};
    libk::Atomic<usize> pins_{};
    object::cleanup cleanup_{};
};

// A worker's list borrows the same Mem-owned requests. Lock order:
// Mem content -> Pager -> Claims. The exit walk borrows Mem before unlocking.
class Pager::Claims final {
    friend class Pager;
    using List = libk::IntrusiveList<Request, &Request::claim>;
    mutable sync::Spin lock_{};
    List list_{};

public:
    Claims() noexcept = default;
    ~Claims() noexcept { libk_assert(empty()); }
    Claims(const Claims &) = delete;
    auto operator=(const Claims &) -> Claims & = delete;
    [[nodiscard]] auto empty() const noexcept -> bool;
    void release() noexcept;
};
