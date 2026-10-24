#pragma once

#include <irq/route.hpp>
#include <expected>
#include <object/ref.hpp>
#include <mm/phys.hpp>
#include <mm/kspace.hpp>
#include <work.hpp>
#include <cap/grant.hpp>
#include <uapi/irq.h>

class Cpus;

namespace irq {

class Ctl final : private libk::noncopyable_nonmovable {
    friend class ::Cpus;
public:
    static constexpr bool fixed_usage = true;
    Ctl(mm::KSpace&, Cpus&, WorkQueue&, mm::Phys::Claim&&, IrqConfig) noexcept;
    ~Ctl() noexcept;
    auto open() noexcept -> status_t;
    void activate() noexcept;
    template<class Publish> auto admit(Publish&& publish) noexcept -> status_t { return bindings_.admit(std::forward<Publish>(publish)); }
    void links(u64 cursor, LinkRecords& out) const noexcept;
    auto line(u32) noexcept -> std::expected<Line, status_t>;
    void bind_work(WorkQueue&, object::Key owner) noexcept { job_.bind(owner); bindings_.bind(owner); }
    void retire(object::cleanup&&) noexcept;
private:
    void dispatch() noexcept;
    void set(u32, bool) noexcept;
    auto take() noexcept -> u32;
    void progress() noexcept;
    void post() noexcept { work_.post(job_); }
    void invalidated() noexcept { auto self = object::ref<>::retain_alive(job_.key()); static_cast<void>(self.destroy()); }
    cap::Bindings<1> bindings_{Source::Action::bind<&Ctl::invalidated>(*this), Source::Action::bind<&Ctl::post>(*this)};
    mm::KSpace& vm_;
    Cpus& cpus_;
    WorkQueue& work_;
    mm::Phys::Claim claim_;
    IrqConfig config_;
    mm::KMap mapping_;
    Routes routes_;
    std::optional<mm::Flush> flush_;
    object::cleanup cleanup_;
    Work job_;
    Ctl* next_{};
    bool open_{}, mapped_{};
};


enum class Error : u8 { InvalidState, Busy, StaleSequence, BadSequence, Closed };
struct Delivery final {
    u64 sequence{};
    u64 generation{};
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return sequence != 0 && generation != 0;
    }
};

class Irq final : private libk::noncopyable_nonmovable {
public:
    static constexpr bool fixed_usage = true;
    explicit Irq(Line line, object::ref<Ctl> owner = {}) noexcept;
    ~Irq() noexcept;
    [[nodiscard]] auto source() const noexcept -> Line { return route_.line_; }
    auto events() noexcept -> Source& { return events_; }
    template<class Publish> auto admit(Publish&& publish) noexcept -> status_t { return bindings_.admit(std::forward<Publish>(publish)); }
    void bind_work(WorkQueue& work, object::Key key) noexcept { bindings_.bind(key); key_ = key; events_.bind_work(work, key); }
    void links(u64 cursor, LinkRecords& out) const noexcept;
    [[nodiscard]] auto enable(bool) noexcept -> std::expected<u64, Error>;
    [[nodiscard]] auto bound() const noexcept -> bool;
    [[nodiscard]] auto pending() const noexcept -> bool;
    [[nodiscard]] auto closed() const noexcept -> bool;
    [[nodiscard]] auto delivery() const noexcept -> std::expected<Delivery, Error>;
    [[nodiscard]] auto ack(u64 generation, u64 sequence) noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto close() noexcept -> bool;
    void retire(object::cleanup&& cleanup) noexcept;
private:
    [[nodiscard]] auto publish() noexcept -> Route::Delivery;
    auto controller_closed() noexcept -> Source::Publication;
    void complete_cleanup() noexcept;
    void invalidated() noexcept { static_cast<void>(close()); }
    object::ref<Ctl> owner_;
    object::Key key_;
    cap::Bindings<1> bindings_{Source::Action::bind<&Irq::invalidated>(*this), Source::Action::bind<&Irq::complete_cleanup>(*this)};
    Route route_;
    Src<SOURCE_READY | SOURCE_TERMINAL> events_{Source::Drained::bind<&Irq::complete_cleanup>(*this)};
    object::cleanup cleanup_;
    u64 generation_{};
    u64 sequence_{};
    bool pending_{};
    bool closed_{};
};

} // namespace irq
