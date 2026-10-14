#pragma once

#include <irq/route.hpp>
#include <ipc/notification_source.hpp>
#include <expected>
#include <object/ref.hpp>
#include <mm/phys.hpp>
#include <mm/kspace.hpp>
#include <work.hpp>
#include <uapi/irq.h>

class Cpus;

namespace irq {

class Ctl final : private libk::noncopyable_nonmovable {
    friend class ::Cpus;
public:
    Ctl(mm::KSpace&, Cpus&, WorkQueue&, mm::Phys::Lease&&, IrqConfig) noexcept;
    ~Ctl() noexcept;
    auto open() noexcept -> status_t;
    auto line(u32) noexcept -> std::expected<Line, status_t>;
    void retire(object::cleanup&&) noexcept;
private:
    void dispatch() noexcept;
    void set(u32, bool) noexcept;
    auto take() noexcept -> u32;
    auto word(usize offset) const noexcept -> volatile u32*;
    void progress() noexcept;
    mm::KSpace& vm_;
    Cpus& cpus_;
    WorkQueue& work_;
    mm::Phys::Lease claim_;
    IrqConfig config_;
    mm::KMap mapping_;
    Routes routes_;
    std::optional<mm::Flush> flush_;
    object::cleanup cleanup_;
    Work job_;
    Ctl* next_{};
    bool open_{};
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
    explicit Irq(Line line, object::ref<Ctl> owner = {}) noexcept;
    ~Irq() noexcept;
    [[nodiscard]] auto source() const noexcept -> Line { return route_.line_; }
    [[nodiscard]] auto bound() const noexcept -> bool;
    [[nodiscard]] auto pending() const noexcept -> bool;
    [[nodiscard]] auto closed() const noexcept -> bool;
    [[nodiscard]] auto bind(ipc::Notification&, u64 badge) noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto unbind() noexcept -> bool;
    [[nodiscard]] auto delivery() const noexcept -> std::expected<Delivery, Error>;
    [[nodiscard]] auto ack(u64 generation, u64 sequence) noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto close() noexcept -> bool;
    void retire(object::cleanup&& cleanup) noexcept;
private:
    [[nodiscard]] auto publish() noexcept -> bool;
    void controller_closed() noexcept;
    object::ref<Ctl> owner_;
    Route route_;
    ipc::NotificationSource notice_{};
    u64 generation_{};
    u64 sequence_{};
    bool pending_{};
    bool closed_{};
};

} // namespace irq
