#pragma once

#include <mm/pmm.hpp>
#include <object/ref.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/unique_handle.hpp>
#include <uapi/abi.h>

namespace mm {

// All attenuated physical authorities share this one external-range registry.
// PMM remains authoritative for RAM and boot reservations.
class Phys final : private libk::noncopyable_nonmovable {
    struct Claim {
        explicit Claim(Pages range) noexcept : range(range) {}
        Pages range;
        libk::intrusive_list_hook hook;
    };
public:
    class Lease {
        friend class Phys;
        struct Data {
            object::ref<Phys> owner;
            Claim* claim{};
            static auto empty() noexcept -> Data { return {}; }
            static auto is_empty(const Data& d) noexcept -> bool { return !d.claim; }
        };
        struct Drop { void operator()(Data&) const noexcept; };
        libk::unique_handle<Data, Drop, Data> lease_;
        Lease(object::ref<Phys>&&, Claim*) noexcept;
    public:
        Lease() noexcept = default;
        explicit operator bool() const noexcept { return bool(lease_); }
        auto covers(Pages range) const noexcept -> bool {
            return lease_ && lease_.get().claim->range.contains(range);
        }
    };
    explicit Phys(Pmm& pmm, RegionList&& reserved) noexcept
        : pmm_(pmm), reserved_(std::move(reserved)), storage_(pmm) {}
    ~Phys() noexcept { libk_assert(claims_.empty()); }
    auto claim(object::ref<Phys>, const object::ref<>& payer, Pages) noexcept
        -> std::expected<Lease, status_t>;
private:
    void release(Claim*) noexcept;
    Pmm& pmm_;
    RegionList reserved_;
    Slab<Claim, false> storage_;
    sync::Spin lock_;
    libk::intrusive_list<Claim, &Claim::hook> claims_;
};

} // namespace mm
