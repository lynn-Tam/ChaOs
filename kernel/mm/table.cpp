#include "table.hpp"
#include <mm/table.hpp>
#include <new>

namespace mm {
using arch::Pte;
static bool addr_is_kernel(VPage va) noexcept {
    return va.base().raw() >= arch::PtKernelBegin;
}

static auto decode(Pte pte) noexcept -> PtLeaf {
    const auto page = pte.leaf_page();
    const auto perms = pte.perms();
    libk_assert(page && perms);
    return {*page, *perms};
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
    if (!Pte::non_leaf(*root))
        return std::unexpected(PtErr::BadPhys);
    PageTable pt{*root, std::move(pages), kind};
    auto *entries = ::new (pt.table(*root)) Pte[arch::Ptes]{};
    for (usize i = arch::Ptes / 2; i < arch::Ptes && kind != Kind::Io; ++i) {
        if (kind == Kind::User) {
            const auto entry = kernel->table(kernel->root_)[i].load();
            libk_assert(entry.next_table_page());
            entries[i].store(entry);
        } else {
            auto branch = pt.tables_.allocate();
            if (!branch)
                return std::unexpected(PtErr::NoMemory);
            auto pte = Pte::non_leaf(*branch);
            if (!pte)
                return std::unexpected(PtErr::BadPhys);
            ::new (pt.table(*branch)) Pte[arch::Ptes]{};
            entries[i].store(*pte);
        }
    }
    return pt;
}

auto PageTable::cpu_root() const noexcept -> usize {
    libk_assert(*this && kind_ != Kind::Io);
    return arch::pt_token(root_);
}

auto PageTable::walk(mm::VPage va) const noexcept -> std::expected<Walk, PtErr> {
    const usize addr = va.base().raw();
    if (!arch::pt_canonical(addr) || (kind_ == Kind::User && !mm::is_user(va.base())) ||
        (kind_ == Kind::Io && (va.base().raw() == 0 || va.base().raw() >= arch::PtUserEnd)))
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
        if (level == 0) {
            if (!entry.leaf_page())
                return std::unexpected(PtErr::Corrupt);
        } else {
            const auto next = entry.next_table_page();
            if (!next)
                return std::unexpected(PtErr::Corrupt);
            page = *next;
        }
    }
    return w;
}

bool PageTable::Count::include(mm::VPage va) noexcept {
    if (previous_ != ~usize{} && va.raw() <= previous_)
        return false;
    const auto w = table_->walk(va);
    if (!w)
        return false;
    // Missing descendants appear once per distinct prefix, even across gaps.
    for (int level = w->missing; level > 0; --level)
        if (previous_ == ~usize{} ||
            (previous_ >> (arch::PtBits * level)) != (va.raw() >> (arch::PtBits * level)))
            ++pages_;
    previous_ = va.raw();
    return true;
}

auto PageTable::encode(mm::Page page, PtPerm perms) const noexcept -> std::expected<Pte, PtErr> {
    // CPU aliases share one immutable attribute. DMA translation is separate.
    const auto attr = kind_ == Kind::Io ? std::optional{CpuAttr::Native}
                                       : tables_.owner().attr_of(Pages{page, 1});
    if (!attr) return std::unexpected(PtErr::BadPhys);
    if (!Pte::supports(*attr)) return std::unexpected(PtErr::BadAttr);
    auto pte = Pte::leaf_4k(page, perms, kind_ != Kind::User, *attr);
    return pte ? std::expected<Pte, PtErr>{*pte} : std::unexpected(PtErr::BadPhys);
}

auto PageTable::map(mm::VPage va, mm::Page page, PtPerm perms, mm::PageGroup &reserve) noexcept
    -> std::expected<void, PtErr> {
    const auto entry = encode(page, perms);
    if (!entry)
        return std::unexpected(entry.error());
    const auto walked = walk(va);
    if (!walked)
        return std::unexpected(walked.error());
    const auto &w = *walked;
    if (w.missing < 0)
        return std::unexpected(PtErr::Exists);
    if (reserve.page_count() < usize(w.missing))
        return std::unexpected(PtErr::NoMemory);
    libk_assert(&reserve.owner() == &tables_.owner());
    std::array<mm::OwnedPage, arch::PtLevels - 1> fresh{};
    auto link = *entry;
    // Build the whole missing subtree privately. No partial branch is published.
    for (int level = 0; level < w.missing; ++level) {
        fresh[level] = std::move(*reserve.take());
        auto parent = Pte::non_leaf(fresh[level].page());
        if (!parent)
            return std::unexpected(PtErr::BadPhys);
        auto *entries = ::new (fresh[level].bytes()) Pte[arch::Ptes]{};
        entries[(va.raw() >> (arch::PtBits * level)) & (arch::Ptes - 1)].store(link);
        link = *parent;
    }
    for (int level = 0; level < w.missing; ++level)
        libk_assert(tables_.attach(std::move(fresh[level])));
    w.entries[w.missing]->store(link);
    return {};
}

auto PageTable::leaf(mm::VPage va) const noexcept -> std::expected<Pte *, PtErr> {
    const auto w = walk(va);
    if (!w)
        return std::unexpected(w.error());
    if (w->missing >= 0)
        return std::unexpected(PtErr::Missing);
    return w->entries[0];
}

auto PageTable::query(mm::VPage va) const noexcept -> std::expected<PtLeaf, PtErr> {
    const auto entry = leaf(va);
    if (!entry)
        return std::unexpected(entry.error());
    return decode((*entry)->load());
}

auto PageTable::usage(mm::VPage va) const noexcept -> std::expected<PageUsage, PtErr> {
    const auto entry = leaf(va);
    if (!entry)
        return std::unexpected(entry.error());
    const auto pte = (*entry)->load();
    return PageUsage{pte.accessed(), pte.dirty()};
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

auto PageTable::dma_pages(usize first, usize count) noexcept -> std::expected<usize, PtErr> {
    constexpr usize limit = usize{1} << 38;
    if (first == 0 || first % mm::page_size != 0 || first >= limit || count == 0 ||
        count > (limit - first) / mm::page_size)
        return std::unexpected(PtErr::BadAddr);
    usize pages = 1;
    const usize last = first + (count - 1) * mm::page_size;
    for (int level = 1; level < arch::PtLevels; ++level) {
        const auto shift = 12 + arch::PtBits * level;
        pages += (last >> shift) - (first >> shift) + 1;
    }
    return pages;
}

} // namespace mm
