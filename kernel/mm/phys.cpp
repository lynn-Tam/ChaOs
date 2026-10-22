#include <mm/phys.hpp>
#include <pte.hpp>

namespace mm {

Phys::Claim::Claim(object::ref<Phys>&& owner, Entry* claim) noexcept
    : entry_(Claim::Data{std::move(owner), claim}) {}
void Phys::Claim::Drop::operator()(Data& data) const noexcept {
    data.owner->release(data.claim);
}
auto Phys::claim(object::ref<Phys> self, const object::ref<>& quota, Pages range) noexcept
    -> std::expected<Claim, status_t> {
    if (!self || &self.get() != this || !range.valid() ||
        !arch::Pte::non_leaf(range.base().raw()) ||
        !arch::Pte::non_leaf(range.base().raw() + range.page_count() - 1))
        return std::unexpected(STATUS_BAD_ARGS);
    for (const auto& region : pmm_.regions())
        if (region.is_ram() && region.range.intersects(range))
            return std::unexpected(STATUS_DENIED);
    for (const auto& region : reserved_)
        if (region.range.intersects(range)) return std::unexpected(STATUS_DENIED);
    auto pending = storage_.create(quota, range);
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
    return Claim{std::move(self), *pending};
}
void Phys::release(Entry* claim) noexcept {
    { sync::Lock guard{lock_}; claims_.erase(*claim); }
    storage_.destroy(*claim);
}

} // namespace mm
