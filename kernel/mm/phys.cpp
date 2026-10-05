#include <mm/phys.hpp>
#include <algorithm>

namespace mm {

auto PhysMap::add_ram(Pages r, CpuAttr attr) noexcept -> std::expected<void, PhysErr> {
    if (!r.valid()) return std::unexpected(PhysErr::Invalid);
    if (!input_.try_emplace_back(Region{r, Region::Kind::Ram, attr}))
        return std::unexpected(PhysErr::Capacity);
    return {};
}

auto PhysMap::reserve(Pages r, Region::Kind kind) noexcept -> std::expected<void, PhysErr> {
    if (!r.valid() || kind == Region::Kind::Ram || kind == Region::Kind::Mmio)
        return std::unexpected(PhysErr::Invalid);
    if (!input_.try_emplace_back(Region{r, kind}))
        return std::unexpected(PhysErr::Capacity);
    return {};
}

auto PhysMap::finish(RegionList& out) && noexcept -> std::expected<void, PhysErr> {
    out.clear();
    auto fail = [&](PhysErr e) -> std::expected<void, PhysErr> {
        out.clear();
        return std::unexpected(e);
    };
    std::ranges::sort(input_, {}, [](const Region& r) { return r.range.base(); });
    bool found = false;
    Pages previous{};
    for (const auto& source : input_) {
        if (source.kind != Region::Kind::Ram) continue;
        const auto bank = source.range;
        if (previous.valid() && previous.intersects(bank)) return fail(PhysErr::Overlap);
        found = true;
        previous = bank;
        auto end = *bank.limit();
        for (auto p = bank.base(); p < end;) {
            auto next = end;
            Region::Kind kind = Region::Kind::Ram;
            for (const auto& r : input_) {
                // Ram < Boot < Firmware < Kernel. Keep enum order semantic,
                // rather than encoding precedence in public classification.
                auto rank = [](Region::Kind k) {
                    return k == Region::Kind::Kernel ? 3 : k == Region::Kind::Firmware ? 2
                         : k == Region::Kind::Boot ? 1 : 0;
                };
                auto last = *r.range.limit();
                if (p < r.range.base()) next = std::min(next, r.range.base());
                if (p < last) next = std::min(next, last);
                if (r.range.contains(p) && rank(r.kind) > rank(kind)) kind = r.kind;
            }
            auto n = next.raw() - p.raw();
            // Boot runs preserve individual reclaimable resource boundaries.
            if (!out.empty() && kind != Region::Kind::Boot && out.back().kind == kind && out.back().attr == source.attr
                && *out.back().range.limit() == p) {
                auto& r = out.back().range;
                r = Pages{r.base(), r.page_count() + n};
            } else if (!out.try_emplace_back(Region{Pages{p, n}, kind, source.attr})) {
                return fail(PhysErr::Capacity);
            }
            p = next;
        }
    }
    return found ? std::expected<void, PhysErr>{} : fail(PhysErr::NoRam);
}

bool DirectMap::valid() const noexcept {
    const auto& l = layout_;
    auto end = l.physical_base.checked_add(l.window_size);
    if (!l.window_size || !l.virtual_base.valid() || !end
        || !l.virtual_base.checked_add(l.window_size)) return false;
    bool found = false;
    for (auto r : ranges()) {
        if (!r.valid() || r.base().base() < l.physical_base
            || r.limit()->base() > *end) return false;
        found = true;
    }
    return found;
}

bool DirectMap::contains(Pages r) const noexcept {
    if (!r.valid()) return false;
    auto p = r.base(), end = *r.limit();
    for (auto bank : ranges()) {
        auto last = *bank.limit();
        if (last <= p) continue;
        if (p < bank.base()) return false;
        if (end <= last) return true;
        p = last;
    }
    return false;
}

auto DirectMap::map(Phys address, usize size) const noexcept -> std::expected<Virt, Error> {
    auto r = Pages::covering_bytes(address, size);
    if (!r) return std::unexpected(Error::Overflow);
    if (!contains(*r)) return std::unexpected(Error::NotMapped);
    auto offset = layout_.physical_base.checked_distance_to(address);
    if (!offset || *offset > layout_.window_size || size > layout_.window_size - *offset)
        return std::unexpected(Error::OutsideWindow);
    auto va = layout_.virtual_base.checked_add(*offset);
    if (!va) return std::unexpected(Error::Overflow);
    return *va;
}

auto DirectMap::unmap(Virt address, usize size) const noexcept -> std::expected<Phys, Error> {
    auto offset = layout_.virtual_base.checked_distance_to(address);
    if (!offset || *offset > layout_.window_size || size > layout_.window_size - *offset)
        return std::unexpected(Error::OutsideWindow);
    auto pa = layout_.physical_base.checked_add(*offset);
    if (!pa) return std::unexpected(Error::Overflow);
    auto r = Pages::covering_bytes(*pa, size);
    if (!r || !contains(*r)) return std::unexpected(Error::NotMapped);
    return *pa;
}

} // namespace mm
