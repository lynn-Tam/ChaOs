#include <mm/phys.hpp>
#include <pte.hpp>

namespace mm {

Phys::Lease::Lease(object::ref<Phys>&& owner, Claim* claim) noexcept
    : lease_(Lease::Data{std::move(owner), claim}) {}
void Phys::Lease::Drop::operator()(Data& data) const noexcept {
    data.owner->release(data.claim);
}
auto Phys::claim(object::ref<Phys> self, const object::ref<>& payer, Pages range) noexcept
    -> std::expected<Lease, status_t> {
    if (!self || &self.get() != this || !range.valid() ||
        !arch::Pte::non_leaf(range.base().raw()) ||
        !arch::Pte::non_leaf(range.base().raw() + range.page_count() - 1))
        return std::unexpected(STATUS_BAD_ARGS);
    for (const auto& region : pmm_.regions())
        if (region.is_ram() && region.range.intersects(range))
            return std::unexpected(STATUS_DENIED);
    for (const auto& region : reserved_)
        if (region.range.intersects(range)) return std::unexpected(STATUS_DENIED);
    auto pending = storage_.create(payer, range);
    if (!pending) return std::unexpected(STATUS_NO_MEMORY);
    {
        sync::Lock guard{lock_};
        for (const auto& claim : claims_)
            if (claim.range.intersects(range)) {
                guard.restore();
                storage_.destroy(**pending);
                return std::unexpected(STATUS_BUSY);
            }
        claims_.push_back(**pending);
    }
    return Lease{std::move(self), *pending};
}
void Phys::release(Claim* claim) noexcept {
    { sync::Lock guard{lock_}; claims_.erase(*claim); }
    storage_.destroy(*claim);
}

} // namespace mm
