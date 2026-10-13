#include <exec.hpp>
#include <util.hpp>
#include <libk/elf.hpp>
#include <array>
#include <tuple>
#include <vector>
#include <algorithm>
#include <string.h>

// Temporary selectors are adopted before any operation that may fail. The
// task owns their cleanup; VSpace owns committed backing after selectors close.
class pr_loader {
    using owner = cap_owner;
    cap_t pool_, vm_;
    vm_area& scratch_;

  public:
    pr_loader(cap_t pool, cap_t vm, vm_area& s) : pool_(pool), vm_(vm), scratch_(s) {}
    auto adopt(sys::SysResult r) noexcept -> std::expected<owner, status_t> {
        owner cap{r.value};
        if (!cap)
            return std::unexpected(r.status == STATUS_OK ? STATUS_INVALID_CAP : r.status);
        if (r.status != STATUS_OK)
            return std::unexpected(r.status);
        return cap;
    }
    auto memory(word_t size, word_t perms = VM_READ | VM_WRITE) noexcept
        -> std::expected<owner, status_t> {
        return adopt(sys::memory_create(pool_, size, perms));
    }
    auto write(cap_t mem, word_t size, word_t offset, const void* src, size_t n) noexcept
        -> status_t {
        if (offset > size || n > size - offset || (n && !src))
            return STATUS_BAD_ARGS;
        const auto* bytes = static_cast<const uint8_t*>(src);
        while (n) {
            const auto page = offset / pr_page_size;
            const auto start = offset % pr_page_size;
            const auto count = std::min(n, size_t{pr_page_size - start});
            auto s = sys::memory_populate(mem, page).status;
            if (s != STATUS_OK)
                return s;
            s = scratch_.attach(mem, page, pr_page_size, VM_READ | VM_WRITE);
            if (s != STATUS_OK)
                return s;
            std::copy_n(bytes, count, reinterpret_cast<uint8_t*>(scratch_.address) + start);
            const auto detached = scratch_.detach();
            if (detached != STATUS_OK)
                return detached;
            offset += count;
            bytes += count;
            n -= count;
        }
        return STATUS_OK;
    }
    auto make_desc(const void* src, size_t n) noexcept -> std::expected<owner, status_t> {
        const auto size = pr_window::round_size(n);
        if (!size || !src)
            return std::unexpected(STATUS_BAD_ARGS);
        auto mem = memory(size);
        if (!mem)
            return mem;
        const auto s = write(mem->selector(), size, 0, src, n);
        if (s != STATUS_OK)
            return std::unexpected(s);
        return mem;
    }
    auto install(cap_t mem, word_t address, word_t size, word_t perms, word_t first = 0,
                 word_t flags = 0) noexcept -> status_t {
        if (!pr_window{address, size}.valid())
            return STATUS_BAD_ARGS;
        auto region = adopt(sys::vm_slice(vm_, address, size, perms, RIGHT_MAP | RIGHT_UNMAP));
        if (!region)
            return region.error();
        const auto status =
            sys::vm_map(region->selector(), mem, address, size, first, perms | flags).status;
        return status;
    }
    auto anonymous(word_t address, word_t size, word_t perms) noexcept
        -> std::expected<owner, status_t> {
        auto mem = memory(size);
        if (!mem)
            return mem;
        const auto s = install(mem->selector(), address, size, perms);
        if (s != STATUS_OK)
            return std::unexpected(s);
        return mem;
    }
    auto map(const libk::elf_view& elf, const pr_image& source) noexcept -> status_t {
        for (size_t i = 0; i < elf.segment_count(); ++i) {
            const auto seg = *elf.segment(i);
            if (seg.type != 1 || !seg.size)
                continue;
            const auto skip = seg.va % pr_page_size;
            const auto size = pr_window::round_size(seg.size + skip);
            const word_t perms = ((seg.flags & 4) ? VM_READ : 0) |
                                 ((seg.flags & 2) ? VM_WRITE : 0) |
                                 ((seg.flags & 1) ? VM_EXECUTE : 0);
            // Only complete pages inside the file may alias its immutable backing.
            // Partial ELF pages are zeroed allocations, never archive neighbours.
            const bool share = source.memory &&
                               (source.perms & (perms & ~VM_WRITE)) == (perms & ~VM_WRITE) &&
                               (source.offset + seg.offset) % pr_page_size == skip;
            const auto first = share ? (skip + pr_page_size - 1) & ~(pr_page_size - 1) : 0;
            const auto last = share ? (skip + seg.file_size) & ~(pr_page_size - 1) : 0;
            word_t offset{};
            while (offset < size) {
                const bool shared = offset >= first && offset < last;
                const auto length = shared                           ? last - offset
                                    : offset < first && first < last ? first - offset
                                                                     : size - offset;
                std::expected<owner, status_t> mem;
                word_t page{};
                if (shared) {
                    page = (source.offset + seg.offset + offset - skip) / pr_page_size;
                    const CapView view{.version = CAP_ATTENUATION_VERSION_CURRENT,
                                       .kind = ObjMem,
                                       .size = CAP_ATTENUATION_SIZE,
                                       .rights = RIGHT_MAP,
                                       .words = {page, length / pr_page_size,
                                                 (perms & VM_WRITE) ? VM_READ : perms}};
                    std::array<uint8_t, CAP_ATTENUATION_SIZE> wire{};
                    cap_encode(view, wire);
                    auto desc = make_desc(wire.data(), wire.size());
                    if (!desc)
                        return desc.error();
                    mem = adopt(sys::cap_typed_delegate(source.memory, 0, desc->selector()));
                } else {
                    mem = memory(length, perms | VM_READ | VM_WRITE);
                    if (mem) {
                        const auto begin = std::max(offset, skip);
                        const auto end = std::min(offset + length, skip + seg.file_size);
                        auto status =
                            begin < end
                                ? write(mem->selector(), length, begin - offset,
                                        elf.bytes().data() + seg.offset + begin - skip, end - begin)
                                : STATUS_OK;
                        if (status == STATUS_OK && (perms & VM_EXECUTE))
                            status = sys::memory_seal(mem->selector()).status;
                        if (status != STATUS_OK)
                            return status;
                    }
                }
                if (!mem)
                    return mem.error();
                const auto status =
                    install(mem->selector(), seg.va - skip + offset, length, perms, page,
                            shared && (perms & VM_WRITE) ? VM_MAP_PRIVATE : 0);
                if (status != STATUS_OK)
                    return status;
                offset += length;
            }
        }
        return STATUS_OK;
    }
};

auto pr_exec(pr_task& r, const pr_auth& auth, const pr_image& image, const pr_opts& o,
             uint32_t first_cpu) noexcept -> status_t {
    const auto mod = libk::elf_view::parse(image.bytes);
    const auto build = [&]() noexcept -> status_t {
        if (image.offset > UINT64_MAX - image.bytes.size())
            return STATUS_BAD_ARGS;
        if (!mod || mod.machine() != 243)
            return STATUS_BAD_ARGS;
        word_t end{};
        size_t binding_size = sizeof(rt_bindings) + o.caps.size() * sizeof(rt_binding);
        for (const auto& src : o.caps) {
            if (!src.cap || !src.contract.name || !src.contract.kind)
                return STATUS_BAD_ARGS;
            const auto name = std::string_view{src.contract.name};
            if (name.empty() || name.size() > UINT32_MAX - binding_size)
                return STATUS_BAD_ARGS;
            binding_size += name.size();
        }
        if (binding_size > UINT32_MAX)
            return STATUS_BAD_ARGS;
        const pr_window bindings{o.va.bindings, pr_window::round_size(binding_size)};
        const pr_window ipc_range{o.va.ipc, pr_page_size};
        if (!auth.cpus || !auth.domain || !o.va.stack.valid() || !bindings.valid() ||
            !ipc_range.valid() || o.va.stack.overlaps(bindings) || o.va.stack.overlaps(ipc_range) ||
            bindings.overlaps(ipc_range))
            return STATUS_BAD_ARGS;
        for (size_t i = 0; i < mod.segment_count(); ++i) {
            const auto decoded = mod.segment(i);
            if (!decoded)
                return STATUS_BAD_ARGS;
            if (decoded->type != 1 || !decoded->size)
                continue;
            const auto skip = decoded->va % pr_page_size;
            if (decoded->size > UINT64_MAX - skip)
                return STATUS_BAD_ARGS;
            const pr_window w{decoded->va - skip, pr_window::round_size(decoded->size + skip)};
            if (w.address < end)
                return STATUS_BAD_ARGS;
            end = w.address + w.size;
            if (!w.valid() || w.overlaps(o.va.stack) || w.overlaps(bindings) ||
                w.overlaps(ipc_range))
                return STATUS_BAD_ARGS;
        }
        const auto argv = o.argv;
        if (argv.empty())
            return STATUS_BAD_ARGS;
        size_t strings{};
        for (const auto arg : argv) {
            if (arg.find('\0') != std::string_view::npos || arg.size() >= o.va.stack.size - strings)
                return STATUS_BAD_ARGS;
            strings += arg.size() + 1;
        }
        auto s = r.open(auth.pool, o.resources);
        if (s != STATUS_OK)
            return s;
        vm_area scratch;
        s = scratch.open(auth.vspace, 0, pr_page_size);
        if (s != STATUS_OK)
            return s;
        pr_loader load{r.pool(), r.vspace(), scratch};
        s = load.map(mod, image);
        if (s != STATUS_OK)
            return s;
        auto stack = load.anonymous(o.va.stack.address, o.va.stack.size, VM_READ | VM_WRITE);
        if (!stack)
            return stack.error();
        auto ipc = load.anonymous(o.va.ipc, pr_page_size, VM_READ | VM_WRITE);
        if (!ipc)
            return ipc.error();
        s = sys::memory_populate(ipc->selector(), 0).status;
        if (s != STATUS_OK)
            return s;
        auto events = load.adopt(sys::notification_create(r.pool(), rt_event_badge));
        if (!events)
            return events.error();
        std::vector<AuxEntry> aux{{AT_PAGESZ, pr_page_size},
                                  {AT_ENTRY, mod.entry()},
                                  {AT_SYS_CPUS, auth.cpus},
                                  {AT_SYS_IPC, o.va.ipc}};
        for (auto [key, cap, rights] :
             std::array{std::tuple{AT_SYS_POOL, r.pool(), o.resources.pool_perms},
                        std::tuple{AT_SYS_VSPACE, r.vspace(),
                                   word_t{RIGHT_DELEGATE | RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP |
                                          RIGHT_DESTROY}},
                        std::tuple{AT_SYS_CSPACE, r.cspace(), word_t{RIGHT_MANAGE}},
                        std::tuple{AT_SYS_EVENTS, events->selector(),
                                   word_t{RIGHT_SIGNAL | RIGHT_RECEIVE | RIGHT_DUPLICATE}}}) {
            const auto copy = sys::cap_duplicate(cap, r.cspace(), rights);
            if (copy.status != STATUS_OK)
                return copy.status;
            aux.push_back({static_cast<word_t>(key), copy.value});
        }
        auto desc = load.memory(pr_page_size);
        if (!desc)
            return desc.error();
        if (!o.caps.empty()) {
            std::vector<uint8_t> data(binding_size);
            const rt_bindings header{rt_bindings_version, static_cast<uint32_t>(binding_size),
                                     static_cast<uint32_t>(o.caps.size()), 0};
            ::memcpy(data.data(), &header, sizeof(header));
            size_t name_offset = sizeof(header) + o.caps.size() * sizeof(rt_binding);
            for (size_t i = 0; i < o.caps.size(); ++i) {
                const auto& src = o.caps[i];
                const auto* view = std::get_if<CapView>(&src.limit);
                const auto kind = src.contract.kind;
                if (view && (view->version != CAP_ATTENUATION_VERSION_CURRENT ||
                             view->size != CAP_ATTENUATION_SIZE || view->kind != kind))
                    return STATUS_BAD_ARGS;
                const auto rights = view ? view->rights : std::get<word_t>(src.limit);
                sys::SysResult copied;
                if (src.badge)
                    copied = sys::channel_mint(src.cap, r.cspace(), src.badge, rights);
                else if (!view)
                    copied = sys::cap_duplicate(src.cap, r.cspace(), rights);
                else {
                    std::array<uint8_t, CAP_ATTENUATION_SIZE> wire{};
                    cap_encode(*view, wire);
                    s = load.write(desc->selector(), pr_page_size, 0, wire.data(), wire.size());
                    if (s != STATUS_OK)
                        return s;
                    copied = sys::cap_typed_delegate(src.cap, r.cspace(), desc->selector());
                }
                if (copied.status != STATUS_OK)
                    return copied.status;
                const std::string_view name{src.contract.name};
                const rt_binding entry{copied.value,
                                       src.bytes,
                                       src.contract.protocol,
                                       kind,
                                       src.contract.major,
                                       src.contract.minor,
                                       0,
                                       static_cast<uint32_t>(name_offset),
                                       static_cast<uint32_t>(name.size())};
                ::memcpy(data.data() + sizeof(header) + i * sizeof(entry), &entry, sizeof(entry));
                ::memcpy(data.data() + name_offset, name.data(), name.size());
                name_offset += name.size();
            }
            auto memory = load.anonymous(bindings.address, bindings.size, VM_READ);
            if (!memory)
                return memory.error();
            s = load.write(memory->selector(), bindings.size, 0, data.data(), data.size());
            if (s == STATUS_OK)
                s = sys::memory_seal(memory->selector()).status;
            if (s != STATUS_OK)
                return s;
            aux.push_back({AT_SYS_BINDINGS, bindings.address});
            aux.push_back({AT_SYS_BINDINGS_SIZE, binding_size});
        }
        aux.push_back({AT_NULL, 0});
        const auto pointers = (argv.size() + 3) * sizeof(word_t);
        const auto prefix = pointers + aux.size() * sizeof(AuxEntry);
        if (prefix > o.va.stack.size || strings > o.va.stack.size - prefix)
            return STATUS_BAD_ARGS;
        const auto stack_bytes = (prefix + strings + 15) & ~size_t{15};
        if (stack_bytes > o.va.stack.size)
            return STATUS_BAD_ARGS;
        const auto sp = o.va.stack.address + o.va.stack.size - stack_bytes;
        std::vector<uint8_t> initial(stack_bytes);
        auto* words = reinterpret_cast<word_t*>(initial.data());
        words[0] = argv.size();
        size_t offset = prefix;
        for (size_t i = 0; i < argv.size(); ++i) {
            words[i + 1] = sp + offset;
            ::memcpy(initial.data() + offset, argv[i].data(), argv[i].size());
            offset += argv[i].size() + 1;
        }
        ::memcpy(initial.data() + pointers, aux.data(), aux.size() * sizeof(AuxEntry));
        s = load.write(stack->selector(), o.va.stack.size, o.va.stack.size - stack_bytes,
                       initial.data(), initial.size());
        if (s != STATUS_OK)
            return s;
        ThreadInit init{};
        init.version = THREAD_START_VERSION;
        init.entry = mod.entry();
        init.stack = sp;
        init.ipc = {.memory = ipc->selector(), .page = 0, .address = o.va.ipc, .pages = 1};
        s = load.write(desc->selector(), 4096, 0, &init, sizeof(init));
        if (s != STATUS_OK)
            return s;
        s = scratch.close();
        return s == STATUS_OK
                   ? r.start(desc->selector(), auth.domain, auth.cpus, o.sched, o.events, first_cpu)
                   : s;
    };
    const auto status = build();
    if (status != STATUS_OK) {
        const auto stopped = r.stop(status);
        if (stopped != STATUS_OK)
            return stopped;
    }
    return status;
}
