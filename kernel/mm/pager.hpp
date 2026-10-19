#pragma once

#include <libk/types.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/unique_handle.hpp>
#include <atomic>
#include <sync.hpp>
#include <cap/grant.hpp>
#include <wait.hpp>
#include <work.hpp>

namespace mm {
class Mem;
template<class> class Cache;
} // namespace mm

// Mem owns request storage and contents. Pager is the authorized service
// session and owns ready/claimed indexes; worker exit does not abandon a claim.
class Pager final : private libk::noncopyable_nonmovable {
public:
    static constexpr bool fixed_bill = true;
    enum class Kind : u8 { PageIn, Writeback };
    struct Req {
        u64 id{};
        Kind kind{Kind::PageIn};
        usize page_index{};
        usize first{};
        usize count{1};
        u64 dirty_epoch{};
    };
    struct Request {
        Req info{};
        mm::Mem *mem{};
        Pager *pager{};
        libk::intrusive_list_hook route{};
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
        [[nodiscard]] auto commit(cap::Graph::Admission = {}, std::span<const object::ref<>*> = {},
            Source::Action = {}, bool fail = false) noexcept -> std::expected<void, Error>;
        [[nodiscard]] auto abort() noexcept -> std::expected<void, Error>;
    };

    Pager() noexcept = default;
    void bind_work(WorkQueue& work, object::Key owner) noexcept { work_ = &work; job_.bind(owner); events_.bind_work(work, owner); }
    ~Pager() noexcept;
    [[nodiscard]] auto state() const noexcept -> State;
    auto events() noexcept -> Source& { return events_; }
    [[nodiscard]] auto pending() const noexcept -> usize;
    [[nodiscard]] auto claim(cap::Graph::Admission = {}, std::span<const object::ref<>*> = {}) noexcept -> std::expected<Req, Error>;
    [[nodiscard]] auto reply(u64 id, mm::Mem *mem = nullptr) noexcept
        -> std::expected<Reply, Error>;
    [[nodiscard]] auto requeue(u64 id, cap::Graph::Admission = {}, std::span<const object::ref<>*> = {}) noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto close(bool force) noexcept -> bool;
    void retire(object::cleanup &&) noexcept;

private:
    friend class mm::Mem;
    template<class> friend class mm::Cache;
    class Pin {
        Pager &pager_;

    public:
        explicit Pin(Pager &p) noexcept : pager_(p) { p.pin(); }
        ~Pin() noexcept { pager_.refresh(); pager_.unpin(); }
    };
    using Routes = libk::intrusive_list<Request, &Request::route>;
    [[nodiscard]] auto enqueue(Request &, Req) noexcept -> bool;
    [[nodiscard]] auto active(const Request &) const noexcept -> bool;
    void cancel(Request&) noexcept;
    void signal() noexcept;
    [[nodiscard]] auto take(Request &) noexcept -> Reply;
    void dispose(Reply::Data &) noexcept;
    void settle() noexcept;
    void pin() noexcept;
    void unpin() noexcept;
    void refresh() noexcept;
    void complete_cleanup() noexcept;
    void force_close() noexcept { (void)close(true); }

    mutable sync::Spin lock_{};
    Routes ready_{}, claimed_{};
    usize replies_{};
    u64 next_id_{};
    Src<SOURCE_READY | SOURCE_TERMINAL> events_{Source::Drained::bind<&Pager::complete_cleanup>(*this)};
    State state_{State::Open};
    std::atomic<usize> pins_{};
    object::cleanup cleanup_{};
    WorkQueue* work_{};
    Work job_{Work::Fn::bind<&Pager::force_close>(*this)};
};
