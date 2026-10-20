#include <vm.hpp>
#include <proc.hpp>
#include <util.hpp>
#include <libk/elf.hpp>
#include <array>
#include <tuple>
#include <vector>
#include <algorithm>
#include <string.h>
#include <libk/checked_arithmetic.hpp>

static constexpr word_t page_size = 4096;

// Temporary selectors are adopted before any operation that may fail. The
// task owns their cleanup; VSpace owns committed backing after selectors close.
class proc_loader {
    using owner = cap_owner;
    CreateAuth pool_;
    cap_t vm_;

  public:
    proc_loader(CreateAuth pool, cap_t vm) : pool_(pool), vm_(vm) {}
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
            const auto start = offset % page_size;
            const auto count = std::min(n, size_t{page_size - start});
            ::memcpy(reinterpret_cast<void*>(rt_startup().ipc), bytes, count);
            const auto status = sys::memory_write(mem, offset, 0, count).status;
            if (status != STATUS_OK) return status;
            offset += count;
            bytes += count;
            n -= count;
        }
        return STATUS_OK;
    }
    auto make_desc(const void* src, size_t n) noexcept -> std::expected<owner, status_t> {
        const auto size = libk::checked_align_up(n, page_size).value_or(0);
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
        auto region = adopt(sys::vm_slice(vm_, address, size, perms, RIGHT_MAP | RIGHT_UNMAP));
        if (!region)
            return region.error();
        const auto status =
            vm_call(pool_, region->selector(), VM_OP_MAP, {mem, address, size, first, perms | flags});
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
    auto map(const libk::elf_view& elf, const proc_image& source) noexcept -> status_t {
        for (size_t i = 0; i < elf.segment_count(); ++i) {
            const auto seg = *elf.segment(i);
            if (seg.type != 1 || !seg.size)
                continue;
            const auto skip = seg.va % page_size;
            const auto size = libk::checked_align_up(seg.size + skip, page_size).value_or(0);
            const word_t perms = ((seg.flags & 4) ? VM_READ : 0) |
                                 ((seg.flags & 2) ? VM_WRITE : 0) |
                                 ((seg.flags & 1) ? VM_EXECUTE : 0);
            // Only complete pages inside the file may alias its immutable backing.
            // Partial ELF pages are zeroed allocations, never archive neighbours.
            const bool share = source.memory &&
                               (source.perms & (perms & ~VM_WRITE)) == (perms & ~VM_WRITE) &&
                               (source.offset + seg.offset) % page_size == skip;
            const auto first = share ? (skip + page_size - 1) & ~(page_size - 1) : 0;
            const auto last = share ? (skip + seg.file_size) & ~(page_size - 1) : 0;
            word_t offset{};
            while (offset < size) {
                const bool shared = offset >= first && offset < last;
                const auto length = shared                           ? last - offset
                                    : offset < first && first < last ? first - offset
                                                                     : size - offset;
                std::expected<owner, status_t> mem;
                word_t page{};
                if (shared) {
                    page = (source.offset + seg.offset + offset - skip) / page_size;
                    const CapView view{.version = CAP_ATTENUATION_VERSION_CURRENT,
                                       .kind = ObjMem,
                                       .size = CAP_ATTENUATION_SIZE,
                                       .rights = RIGHT_MAP,
                                       .words = {page, length / page_size,
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

auto proc_spawn(proc_task& r, const proc_auth& auth, const proc_image& image, const proc_opts& o) noexcept -> status_t {
    const auto mod = libk::elf_view::parse(image.bytes);
    const auto build = [&]() noexcept -> status_t {
        if (image.offset > UINT64_MAX - image.bytes.size())
            return STATUS_BAD_ARGS;
        if (!mod || mod.machine() != 243)
            return STATUS_BAD_ARGS;
        if (o.caps.size() > (UINT32_MAX - sizeof(rt_bindings)) / sizeof(rt_binding))
            return STATUS_BAD_ARGS;
        size_t binding_size = sizeof(rt_bindings) + o.caps.size() * sizeof(rt_binding);
        for (const auto& src : o.caps) {
            if ((std::holds_alternative<cap_t>(src.cap) ? !std::get<cap_t>(src.cap) : !std::get<cap_owner*>(src.cap) || !*std::get<cap_owner*>(src.cap))
                || !src.key.name || !src.key.kind)
                return STATUS_BAD_ARGS;
            const auto name = std::string_view{src.key.name};
            if (name.empty() || name.size() > UINT32_MAX - binding_size)
                return STATUS_BAD_ARGS;
            binding_size += name.size();
        }
        if (binding_size > UINT32_MAX)
            return STATUS_BAD_ARGS;
        const auto binding_bytes = libk::checked_align_up(binding_size, page_size).value_or(0);
        if (!auth.cpus || o.cpu >= auth.cpus) return STATUS_BAD_ARGS;
        std::vector<std::pair<word_t, word_t>> ranges{
            {o.va.stack, o.va.stack_size}, {o.va.bindings, binding_bytes}, {o.va.ipc, page_size}};
        for (size_t i = 0; i < mod.segment_count(); ++i) {
            const auto seg = *mod.segment(i);
            if (seg.type == 1 && seg.size) {
                const auto skip = seg.va % page_size;
                ranges.emplace_back(seg.va - skip,
                    libk::checked_align_up(seg.size + skip, page_size).value_or(0));
            }
        }
        // ELF program headers need not be sorted. Validate the complete layout
        // before allocating anything in the child, including page envelopes.
        std::ranges::sort(ranges);
        word_t end{};
        for (const auto& [address, size] : ranges) {
            if (!address || !size || (address | size) % page_size ||
                address < end || size > UINT64_MAX - address) return STATUS_BAD_ARGS;
            end = address + size;
        }
        const auto argv = o.argv;
        if (argv.empty())
            return STATUS_BAD_ARGS;
        size_t strings{};
        for (const auto args : {argv, o.envp})
            for (const auto arg : args) {
                if (arg.find('\0') != std::string_view::npos || arg.size() >= o.va.stack_size - strings)
                    return STATUS_BAD_ARGS;
                strings += arg.size() + 1;
            }
        auto s = r.pool() ? STATUS_OK : r.open(auth, o.resources);
        if (s != STATUS_OK)
            return s;
        proc_loader load{r.alloc(), r.vspace()};
        s = load.map(mod, image);
        if (s != STATUS_OK)
            return s;
        auto stack = load.memory(o.va.stack_size);
        if (!stack)
            return stack.error();
        auto ipc = load.anonymous(o.va.ipc, page_size, VM_READ | VM_WRITE);
        if (!ipc)
            return ipc.error();
        s = sys::memory_populate(ipc->selector(), 0).status;
        if (s != STATUS_OK)
            return s;
        auto events = load.adopt(sys::port_create(r.alloc(), o.resources.watches, o.resources.hints));
        if (!events)
            return events.error();
        std::vector<AuxEntry> aux{{AT_PAGESZ, page_size},
                                  {AT_ENTRY, mod.entry()},
                                  {AT_SYS_CPUS, auth.cpus},
                                  {AT_SYS_IPC, o.va.ipc}};
        for (auto [key, cap, rights] :
             std::array{std::tuple{AT_SYS_POOL, r.pool(), o.resources.pool_perms},
                        std::tuple{AT_SYS_DOMAIN, r.domain(), word_t{RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE | RIGHT_DELEGATE}},
                        std::tuple{AT_SYS_SCOPE, r.scope(), word_t{RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE}},
                        std::tuple{AT_SYS_VSPACE, r.vspace(),
                                   word_t{RIGHT_MANAGE | RIGHT_DELEGATE | RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP |
                                          RIGHT_DESTROY | RIGHT_INSPECT}},
                        std::tuple{AT_SYS_CSPACE, r.cspace(), word_t{RIGHT_MANAGE | RIGHT_INSPECT}},
                        std::tuple{AT_SYS_EVENTS, events->selector(),
                                   word_t{RIGHT_MANAGE | RIGHT_RECEIVE | RIGHT_DUPLICATE}}}) {
            const auto copy = sys::cap_duplicate(cap, r.cspace(), rights);
            if (copy.status != STATUS_OK)
                return copy.status;
            aux.push_back({static_cast<word_t>(key), copy.value});
        }
        auto desc = load.memory(page_size);
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
                const auto kind = src.key.kind;
                if (view && (view->version != CAP_ATTENUATION_VERSION_CURRENT ||
                             view->size != CAP_ATTENUATION_SIZE || view->kind != kind))
                    return STATUS_BAD_ARGS;
                const auto rights = view ? view->rights : std::get<word_t>(src.limit);
                sys::SysResult copied;
                const auto owned = std::get_if<cap_owner*>(&src.cap);
                const auto source = owned ? (*owned)->selector() : std::get<cap_t>(src.cap);
                if (owned) {
                    // Native Move preserves the exact view; Copy/Delegate
                    // attenuate a borrowed source. Release only on commit.
                    if (view || rights || src.badge) return STATUS_BAD_ARGS;
                    copied = sys::cap_move(source, r.cspace());
                    if (copied.status == STATUS_OK) (void)(*owned)->release();
                } else if (src.badge)
                    copied = sys::channel_mint(source, r.cspace(), src.badge, rights);
                else if (!view)
                    copied = sys::cap_duplicate(source, r.cspace(), rights);
                else {
                    std::array<uint8_t, CAP_ATTENUATION_SIZE> wire{};
                    cap_encode(*view, wire);
                    s = load.write(desc->selector(), page_size, 0, wire.data(), wire.size());
                    if (s != STATUS_OK)
                        return s;
                    copied = sys::cap_typed_delegate(source, r.cspace(), desc->selector());
                }
                if (copied.status != STATUS_OK)
                    return copied.status;
                const std::string_view name{src.key.name};
                const rt_binding entry{copied.value,
                                       src.bytes,
                                       kind,
                                       0,
                                       static_cast<uint32_t>(name_offset),
                                       static_cast<uint32_t>(name.size())};
                ::memcpy(data.data() + sizeof(header) + i * sizeof(entry), &entry, sizeof(entry));
                ::memcpy(data.data() + name_offset, name.data(), name.size());
                name_offset += name.size();
            }
            auto memory = load.memory(binding_bytes);
            if (!memory)
                return memory.error();
            s = load.write(memory->selector(), binding_bytes, 0, data.data(), data.size());
            if (s == STATUS_OK)
                s = sys::memory_seal(memory->selector()).status;
            if (s == STATUS_OK)
                s = load.install(memory->selector(), o.va.bindings, binding_bytes, VM_READ);
            if (s != STATUS_OK)
                return s;
            aux.push_back({AT_SYS_BINDINGS, o.va.bindings});
            aux.push_back({AT_SYS_BINDINGS_SIZE, binding_size});
        }
        aux.push_back({AT_NULL, 0});
        const auto pointers = (argv.size() + o.envp.size() + 3) * sizeof(word_t);
        const auto prefix = pointers + aux.size() * sizeof(AuxEntry);
        if (prefix > o.va.stack_size || strings > o.va.stack_size - prefix)
            return STATUS_BAD_ARGS;
        const auto stack_bytes = (prefix + strings + 15) & ~size_t{15};
        if (stack_bytes > o.va.stack_size)
            return STATUS_BAD_ARGS;
        const auto sp = o.va.stack + o.va.stack_size - stack_bytes;
        std::vector<uint8_t> initial(stack_bytes);
        auto* words = reinterpret_cast<word_t*>(initial.data());
        words[0] = argv.size();
        size_t offset = prefix;
        size_t pointer = 1;
        for (const auto args : {argv, o.envp}) {
            for (const auto arg : args) {
                words[pointer++] = sp + offset;
                ::memcpy(initial.data() + offset, arg.data(), arg.size());
                offset += arg.size() + 1;
            }
            ++pointer; // argv/envp each end with a zero pointer.
        }
        ::memcpy(initial.data() + pointers, aux.data(), aux.size() * sizeof(AuxEntry));
        s = load.write(stack->selector(), o.va.stack_size, o.va.stack_size - stack_bytes,
                       initial.data(), initial.size());
        if (s == STATUS_OK)
            s = load.install(stack->selector(), o.va.stack, o.va.stack_size, VM_READ | VM_WRITE);
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
        return r.prepare(desc->selector(), o.events, o.cpu);
    };
    const auto status = build();
    if (status != STATUS_OK) {
        const auto stopped = r.stop(status);
        if (stopped != STATUS_OK)
            return stopped;
    }
    return status;
}

auto proc_archive::open(cap_t cap, size_t bytes) -> status_t {
    const auto copied = sys::cap_duplicate(cap, rt_startup().cspace, RIGHT_MAP);
    if (copied.status != STATUS_OK) return copied.status;
    auto mapped = vm_area::map(rt_vm(), cap_owner{copied.value}, 0, (bytes + 4095) & ~size_t{4095}, VM_READ);
    if (!mapped) return mapped.error();
    memory = std::move(*mapped);
    files = libk::cpio_view::parse({reinterpret_cast<const std::byte*>(memory.address), bytes});
    return files ? STATUS_OK : STATUS_BAD_ARGS;
}
auto proc_archive::image(std::string_view name) const -> std::expected<proc_image, status_t> {
    const auto file = files.find(name);
    if (!file) return std::unexpected(STATUS_NOT_FOUND);
    return proc_image{.bytes=file->data};
}
