#include "table.hpp"
#include <new>

namespace mm {
using arch::Pte;
static bool addr_is_kernel(VPage va) noexcept {
    return va.base().raw() >= arch::PtKernelBegin;
}

static auto decode(Pte pte, VPage va, int level) noexcept -> PtLeaf {
    const auto page = pte.leaf_page();
    const auto perms = pte.perms();
    libk_assert(page && perms);
    const usize pages = usize{1} << (arch::PtBits * level);
    return {Page{*page + (va.raw() & (pages - 1))}, *perms, pages};
}

PageTable::PageTable(PageTable &&other) noexcept
    : root_(std::exchange(other.root_, mm::Page{~usize{}})), tables_(std::move(other.tables_)),
      kind_(other.kind_) {}

auto PageTable::table(mm::Page page) const noexcept -> Pte * {
    return reinterpret_cast<Pte *>(const_cast<byte *>(tables_.bytes(page)));
}

auto PageTable::create(mm::Pmm &pmm, Kind kind, const PageTable *kernel) noexcept
    -> std::expected<PageTable, PtErr> {
    libk_assert((kind == Kind::User) == (kernel != nullptr));
    libk_assert(!kernel || (kernel->kind_ == Kind::Kernel && &kernel->tables_.owner() == &pmm));
    auto pages = pmm.group();
    auto root = pages.allocate();
    if (!root)
        return std::unexpected(PtErr::NoMemory);
    if (!Pte::non_leaf(root->raw()))
        return std::unexpected(PtErr::BadPhys);
    PageTable pt{*root, std::move(pages), kind};
    auto *entries = ::new (pt.table(*root)) Pte[arch::Ptes]{};
    for (usize i = arch::Ptes / 2; kind != Kind::Device && i < arch::Ptes; ++i) {
        if (kind == Kind::User) {
            const auto entry = kernel->table(kernel->root_)[i].load();
            libk_assert(entry.next_table_page());
            entries[i].store(entry);
        } else {
            auto branch = pt.tables_.allocate();
            if (!branch)
                return std::unexpected(PtErr::NoMemory);
            auto pte = Pte::non_leaf(branch->raw());
            if (!pte)
                return std::unexpected(PtErr::BadPhys);
            ::new (pt.table(*branch)) Pte[arch::Ptes]{};
            entries[i].store(pte->with_global());
        }
    }
    return pt;
}

auto PageTable::cpu_root() const noexcept -> usize {
    libk_assert(*this && kind_ != Kind::Device);
    return arch::pt_token(root_.raw());
}

auto PageTable::walk(mm::VPage va) const noexcept -> std::expected<Walk, PtErr> {
    const usize addr = va.base().raw();
    if (!arch::pt_canonical(addr) || (kind_ != Kind::Kernel && !mm::is_user(va.base())))
        return std::unexpected(PtErr::BadAddr);
    Walk w{};
    auto page = root_;
    for (int level = arch::PtLevels - 1; level >= 0; --level) {
        // A malformed branch must never redirect a CPU edit into another owner's frame.
        if (!tables_.contains(page))
            return std::unexpected(PtErr::Corrupt);
        w.pages[level] = page;
        w.entries[level] = &table(page)[(va.raw() >> (arch::PtBits * level)) & (arch::Ptes - 1)];
        const auto entry = w.entries[level]->load();
        if (!entry.valid()) {
            w.missing = level;
            break;
        }
        if (entry.is_leaf()) {
            const auto physical = entry.leaf_page();
            if (!physical || (*physical & ((usize{1} << (arch::PtBits * level)) - 1)))
                return std::unexpected(PtErr::Corrupt);
            w.level = level;
            break;
        }
        const auto next = entry.next_table_page();
        if (!level || !next) return std::unexpected(PtErr::Corrupt);
        page = Page{*next};
    }
    return w;
}

bool PageTable::Count::include(mm::VPage va, int leaf_level) noexcept {
    if (previous_ != ~usize{} && va.raw() <= previous_)
        return false;
    const auto w = table_->walk(va);
    if (!w)
        return false;
    // Missing descendants appear once per distinct prefix, even across gaps.
    for (int level = w->missing; level > leaf_level; --level)
        if (previous_ == ~usize{} ||
            (previous_ >> (arch::PtBits * level)) != (va.raw() >> (arch::PtBits * level)))
            ++pages_;
    previous_ = va.raw();
    return true;
}

auto PageTable::encode(mm::Page page, PtPerm perms) const noexcept -> std::expected<Pte, PtErr> {
    auto pte = Pte::leaf(page.raw(), perms, kind_ != Kind::User);
    return pte ? std::expected<Pte, PtErr>{*pte} : std::unexpected(PtErr::BadPhys);
}

auto PageTable::map(mm::VPage va, mm::Page page, PtPerm perms, mm::PageGroup &reserve) noexcept
    -> std::expected<void, PtErr> {
    return map_level(va, page, perms, reserve, 0);
}

auto PageTable::map_level(VPage va, Page page, PtPerm perms, PageGroup& reserve, int leaf_level) noexcept
    -> std::expected<void, PtErr> {
    const auto mask = (usize{1} << (arch::PtBits * leaf_level)) - 1;
    if ((va.raw() | page.raw()) & mask) return std::unexpected(PtErr::BadAddr);
    const auto entry = encode(page, perms);
    if (!entry)
        return std::unexpected(entry.error());
    const auto walked = walk(va);
    if (!walked)
        return std::unexpected(walked.error());
    const auto &w = *walked;
    if (w.missing < leaf_level)
        return std::unexpected(PtErr::Exists);
    if (reserve.page_count() < usize(w.missing - leaf_level))
        return std::unexpected(PtErr::NoMemory);
    libk_assert(&reserve.owner() == &tables_.owner());
    std::array<mm::OwnedPage, arch::PtLevels - 1> fresh{};
    auto link = *entry;
    // Build the whole missing subtree privately. No partial branch is published.
    for (int level = leaf_level; level < w.missing; ++level) {
        fresh[level] = std::move(*reserve.take());
        auto parent = Pte::non_leaf(fresh[level].page().raw());
        if (!parent)
            return std::unexpected(PtErr::BadPhys);
        auto *entries = ::new (fresh[level].bytes()) Pte[arch::Ptes]{};
        entries[(va.raw() >> (arch::PtBits * level)) & (arch::Ptes - 1)].store(link);
        link = *parent;
    }
    for (int level = leaf_level; level < w.missing; ++level)
        libk_assert(tables_.attach(std::move(fresh[level])));
    w.entries[w.missing]->store(link);
    return {};
}

auto PageTable::map_direct(va_t address, Pages pages) noexcept -> std::expected<void, PtErr> {
    const auto first = VPage::from_base(address);
    if (kind_ != Kind::Kernel || !first || !pages.valid()
        || address.raw() < DirectBegin || address.raw() >= DirectEnd
        || pages.byte_size() > DirectEnd - address.raw()) return std::unexpected(PtErr::BadAddr);
    constexpr usize large = usize{1} << arch::PtBits;
    auto reserve = tables_.owner().group();
    for (usize i = 0; i < pages.page_count();) {
        const auto va = *first->checked_add(i);
        const auto pa = *pages.base().checked_add(i);
        const int level = !((va.raw() | pa.raw()) & (large - 1)) && pages.page_count() - i >= large ? 1 : 0;
        auto plan = count();
        if (!plan.include(va, level)) return std::unexpected(PtErr::BadAddr);
        if (!reserve.grow(plan.pages())) return std::unexpected(PtErr::NoMemory);
        auto result = map_level(va, pa, PtPerm::Rw, reserve, level);
        if (!result) return result;
        i += level ? large : 1;
    }
    return {};
}

auto PageTable::leaf(mm::VPage va) const noexcept -> std::expected<Pte *, PtErr> {
    const auto w = walk(va);
    if (!w)
        return std::unexpected(w.error());
    if (w->missing >= 0)
        return std::unexpected(PtErr::Missing);
    if (w->level) return std::unexpected(PtErr::BadAddr);
    return w->entries[0];
}

auto PageTable::query(mm::VPage va) const noexcept -> std::expected<PtLeaf, PtErr> {
    const auto w = walk(va);
    if (!w) return std::unexpected(w.error());
    if (w->missing >= 0) return std::unexpected(PtErr::Missing);
    return decode(w->entries[w->level]->load(), va, w->level);
}

auto PageTable::usage(mm::VPage va) const noexcept -> std::expected<PageUsage, PtErr> {
    const auto entry = leaf(va);
    if (!entry)
        return std::unexpected(entry.error());
    const auto pte = (*entry)->load();
    return PageUsage{pte.accessed(), pte.dirty()};
}

auto PageTable::touch(mm::VPage va, Perm mode) noexcept -> std::expected<bool, PtErr> {
    const auto entry = leaf(va);
    if (!entry) return std::unexpected(entry.error());
    const auto pte = (*entry)->load();
    if (!(static_cast<u8>(*pte.perms()) & static_cast<u8>(mode)))
        return std::unexpected(PtErr::BadAddr);
    if (pte.accessed() && (mode != Perm::Write || pte.dirty())) return false;
    (*entry)->update([&](Pte p) { return p.with_usage(true, p.dirty() || mode == Perm::Write); });
    return true;
}

auto PageTable::clear_usage(mm::VPage va) noexcept -> std::expected<PageUsage, PtErr> {
    const auto entry = leaf(va);
    if (!entry)
        return std::unexpected(entry.error());
    const auto pte = (*entry)->update([](Pte p) noexcept { return p.with_usage(false, false); });
    return PageUsage{pte.accessed(), pte.dirty()};
}

auto PageTable::protect(mm::VPage va, PtPerm perms) noexcept -> std::expected<PageUsage, PtErr> {
    const auto entry = leaf(va);
    if (!entry)
        return std::unexpected(entry.error());
    const auto old = (*entry)->update([&](Pte p) noexcept { return p.with_permissions(perms); });
    return PageUsage{old.accessed(), old.dirty()};
}

auto PageTable::replace(mm::VPage va, mm::Page page, PtPerm perms) noexcept
    -> std::expected<PageUsage, PtErr> {
    const auto pte = encode(page, perms);
    if (!pte)
        return std::unexpected(pte.error());
    const auto entry = leaf(va);
    if (!entry)
        return std::unexpected(entry.error());
    const auto old = (*entry)->exchange(*pte);
    return PageUsage{old.accessed(), old.dirty()};
}

auto PageTable::unmap(mm::VPage va) noexcept -> std::expected<PtRemoved, PtErr> {
    const auto walked = walk(va);
    if (!walked)
        return std::unexpected(walked.error());
    const auto &w = *walked;
    if (w.missing >= 0)
        return std::unexpected(PtErr::Missing);
    if (w.level) return std::unexpected(PtErr::BadAddr);
    const auto old = w.entries[0]->exchange(Pte{});
    PtRemoved result{{old.accessed(), old.dirty()}, {}};
    for (int level = 0; level < arch::PtLevels - 1; ++level) {
        // Upper kernel root branches are permanent: published user roots borrow them.
        if (kind_ == Kind::Kernel && level == arch::PtLevels - 2 && addr_is_kernel(va))
            break;
        const auto *entries = table(w.pages[level]);
        bool empty = true;
        for (usize i = 0; i < arch::Ptes && empty; ++i)
            empty = !entries[i].load().valid();
        if (!empty)
            break;
        w.entries[level + 1]->store(Pte{});
        auto owned = tables_.detach(w.pages[level]);
        libk_assert(owned && result.tables.try_push_back(std::move(*owned)));
    }
    return result;
}

auto PageTable::user_perms(mm::Perms perms) noexcept -> std::optional<PtPerm> {
    if (!mm::valid_perms(perms) ||
        (perms.contains(mm::Perm::Write) && perms.contains(mm::Perm::Execute)))
        return std::nullopt;
    if (perms.contains(mm::Perm::Write))
        return PtPerm::UserRw;
    if (perms.contains(mm::Perm::Execute))
        return perms.contains(mm::Perm::Read) ? PtPerm::UserRx : PtPerm::UserX;
    return perms.contains(mm::Perm::Read) ? std::optional{PtPerm::UserRo} : std::nullopt;
}

} // namespace mm
