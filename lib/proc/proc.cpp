#include <vm.hpp>
#include <proc.hpp>
#include <thread.hpp>
#include <libk/elf.hpp>
#include <uapi/riscv64.h>
#include <array>
#include <tuple>
#include <bit>
#include <atomic>
#include <libk/inplace_vector.hpp>
#include <climits>
#include <algorithm>
#include <string.h>
#include <libk/checked_arithmetic.hpp>

_proc::~_proc() {
    if (scope_) (void)_scope_close(scope());
    if (quota_) (void)_quota_close(quota());
}
auto _proc::open(const _proc_auth& auth, const _proc_limits& q) noexcept -> status_t {
    if (state_ != state::New || quota_) return STATUS_BUSY;
    const auto quota = _quota_create(auth.alloc, q.bytes, q.caps, q.kinds,
        q.max_bytes ? q.max_bytes : q.bytes, q.max_caps ? q.max_caps : q.caps);
    quota_ = _cap_owner{quota.value};
    if (quota.status != STATUS_OK) return quota.status;
    const auto scope = _scope_create(auth.alloc); scope_ = _cap_owner{scope.value};
    if (scope.status != STATUS_OK) return scope.status;
    auto status = _domain_info(auth.domain).status;
    if (status != STATUS_OK) return status;
    cpus_ = reinterpret_cast<const DomainInfo*>(_ipc())->cpus;
    if (!cpus_) return STATUS_DENIED;
    const auto domain = _domain_create(alloc(), auth.domain, 0, 0, 1024, cpus_); domain_ = _cap_owner{domain.value};
    if (domain.status != STATUS_OK) return domain.status;
    const auto vm = _vspace_create(alloc()); vm_ = _cap_owner{vm.value};
    if (vm.status != STATUS_OK) return vm.status;
    const auto cs = _cspace_create(alloc(), q.slots, q.pages); cs_ = _cap_owner{cs.value};
    if (cs.status != STATUS_OK) return cs.status;
    return lifecycle_.open(alloc(), 4, 0, 2);
}
auto _proc::prepare(cap_t desc, _ev_port* events, uint32_t cpu) noexcept -> status_t {
    if (thread_) return STATUS_BUSY;
    const auto thread = _thread_create(alloc(), vspace(), cspace(), desc, 0, cpu); thread_ = _cap_owner{thread.value};
    if (thread.status != STATUS_OK) return thread.status;
    const auto sc = _sc_create(alloc(), domain(), 0, 0, 1024, cpus_); sc_ = _cap_owner{sc.value};
    if (sc.status != STATUS_OK) return sc.status;
    auto status = _sc_bind(sc_.selector(), thread_.selector()).status;
    if (status != STATUS_OK) return status;
    events_ = events;
    if (!events) return STATUS_OK;
    const std::array<cap_t, Watches> sources{ lifecycle_.ends[0].selector(), vspace(), scope(), quota()};
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto mask = (i == Exec) ? 0 : i == Life ? SOURCE_READY | SOURCE_TERMINAL
            : STATE_DRAINED;
        status = watches_[i].open(events, sources[i], mask, 0, (i == Exec) ? WATCH_CHANGED : 0,
            i == Life ? SOURCE_READ : i == Exec ? SOURCE_EXIT : SOURCE_STATE);
        if (status != STATUS_OK) return status;
        if (i == Scope || i == Quota) watches_[i].ack(); // Creation proved these lifecycle sources are open.
    }
    return STATUS_OK;
}
auto _proc::start() noexcept -> status_t {
    if (!thread_ || state_ != state::New) return STATUS_BAD_ARGS;
    const auto status = _execution_set(thread_.selector(), true).status;
    if (status != STATUS_OK) return status;
    state_ = state::Running;
    if (const auto rc = thread_.close(); rc != STATUS_OK) return rc;
    return sc_.close();
}
auto _proc::stop(int reason) noexcept -> status_t {
    if (state_ >= state::Stopping) return STATUS_OK;
    if (vm_) {
        const auto status = _vspace_stop(vspace()).status;
        if (status != STATUS_OK) return status;
    }
    result_.code = reason;
    result_.exit.origin = EXIT_STOP;
    result_.exit.status = reason;
    state_ = state::Stopping; dirty_ = true;
    if (const auto status = watches_[Life].close(); status != STATUS_OK) return status;
    return STATUS_OK;
}
auto _proc::retire() noexcept -> status_t {
    const auto status = stopping() ? STATUS_OK : stop();
    if (status != STATUS_OK) return status;
    retiring_ = true;
    if (state_ == state::Exited) {
        const auto closed = close();
        if (closed != STATUS_WOULD_BLOCK) return closed;
    }
    return STATUS_OK;
}
auto _proc::close() noexcept -> status_t {
    if (state_ == state::Exited) {
        for (auto i : {Life, Exec}) { const auto status = watches_[i].close(); if (status != STATUS_OK) return status; }
        const auto life = lifecycle_.close(); if (life != STATUS_OK) return life;
        for (auto* cap : {&thread_, &sc_}) { const auto status = cap->close(); if (status != STATUS_OK) return status; }
        if (scope_) { const auto status = _scope_close(scope()).status; if (status != STATUS_OK) return status; }
        for (auto* cap : {&cs_, &vm_, &domain_}) { const auto status = cap->close(); if (status != STATUS_OK) return status; }
        state_ = state::Closing;
    }
    if (scope_) {
        const auto facts = _source_info(scope());
        if (facts.status != STATUS_OK) return facts.status;
        if (!(facts.value & STATE_DRAINED)) return STATUS_WOULD_BLOCK;
        if (const auto status = watches_[Scope].close(); status != STATUS_OK) return status;
        if (const auto status = scope_.close(); status != STATUS_OK) return status;
    }
    if (quota_) {
        const auto facts = _source_info(quota());
        if (facts.status != STATUS_OK) return facts.status;
        if (facts.value & STATE_OPEN) {
            const auto status = _quota_close(quota()).status;
            if (status != STATUS_OK) return status;
            return STATUS_WOULD_BLOCK;
        }
        if (!(facts.value & STATE_DRAINED)) return STATUS_WOULD_BLOCK;
        if (const auto status = watches_[Quota].close(); status != STATUS_OK) return status;
        const auto destroyed = _object_destroy(quota()).status; if (destroyed != STATUS_OK) return destroyed;
        if (const auto status = quota_.close(); status != STATUS_OK) return status;
    }
    state_ = state::Drained;
    return STATUS_OK;
}
auto _proc::poll() noexcept -> status_t {
    if (state_ == state::Drained) return STATUS_OK;
    if (state_ == state::New) return STATUS_OK;
    if (state_ == state::Running) {
        for (size_t n = 0; n < 8; ++n) {
            _ch_packet packet;
            const auto received = _ch_try_recv(lifecycle_.ends[0].selector(), packet, 0).status;
            if (received == STATUS_WOULD_BLOCK) break;
            if (received != STATUS_OK) return received;
            const auto valid = packet.word_count == PROGRAM_EXIT_WORDS && packet.words[0] == PROGRAM_EXIT_VERSION && !packet.called && !packet.count;
            const auto status = stop(valid ? static_cast<int>(packet.words[1]) : STATUS_PEER_FAULT);
            if (status != STATUS_OK) return status;
            result_.exit.origin = valid ? EXIT_NORMAL : EXIT_STOP;
            break;
        }
        if (state_ == state::Running && watches_[Life]) {
            const auto status = watches_[Life].arm().status;
            if (status != STATUS_OK) return status;
        }
    }
    if (state_ <= state::Stopping) {
        ExecInfo info{};
        if (vm_) {
            const auto status = _vspace_exit(vspace()).status;
            if (status != STATUS_OK) return status;
            ::memcpy(&info, reinterpret_cast<const void*>(_ipc()), sizeof(info));
        } else info.flags = EXEC_STOPPED;
        if (info.flags & EXEC_FAILED) {
            if (state_ < state::Stopping) {
                const auto status = stop(info.failure.status);
                if (status != STATUS_OK) return status;
            }
            result_.code = info.failure.status; result_.thread = info.thread; result_.exit = info.failure;
        }
        if (state_ == state::Stopping) {
            if (domain_) { const auto status = _domain_close(domain()).status; if (status != STATUS_OK) return status; }
            if (info.flags & EXEC_STOPPED) state_ = state::Exited;
        }
        if (state_ <= state::Stopping && watches_[Exec]) {
            const auto status = watches_[Exec].arm(info.sequence).status;
            if (status != STATUS_OK) return status;
        }

    }
    if (retiring_ && state_ >= state::Exited) {
        const auto status = close();
        if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) return status;
    }
    dirty_ = false;
    return STATUS_OK;
}

static constexpr word_t page_size = 4096;

auto _proc_image::map(_vm_area& source, _cap_owner memory, uint64_t offset, uint64_t size, word_t perms)
    -> std::expected<_proc_image, status_t> {
    const auto end = libk::checked_add(offset, size);
    const auto bytes = end ? libk::checked_align_up(*end, page_size).value_or(0) : 0;
    if (!size || !bytes) return std::unexpected(STATUS_BAD_ARGS);
    auto mapping = _vm_area::map(_vm(), std::move(memory), 0, bytes, VM_READ);
    if (!mapping) return std::unexpected(mapping.error());
    source = std::move(*mapping);
    return _proc_image{.bytes={reinterpret_cast<const std::byte*>(source.address + offset), size_t(size)},
        .memory=source.memory.selector(), .offset=offset, .perms=perms};
}

// Temporary selectors are adopted before any operation that may fail. The
// task owns their cleanup; VSpace owns committed backing after selectors close.
class _proc_loader {
    using owner = _cap_owner;
    CreateAuth pool_;
    cap_t vm_, pager_;

  public:
    _proc_loader(CreateAuth pool, cap_t vm, cap_t pager) : pool_(pool), vm_(vm), pager_(pager) {}
    auto adopt(_sys_result r) noexcept -> std::expected<owner, status_t> {
        owner cap{r.value};
        if (r.status == STATUS_PENDING) r.status = _op_wait(cap.selector()).status;
        if (!cap)
            return std::unexpected(r.status == STATUS_OK ? STATUS_INVALID_CAP : r.status);
        if (r.status != STATUS_OK)
            return std::unexpected(r.status);
        return cap;
    }
    auto memory(word_t size, word_t perms = VM_READ | VM_WRITE) noexcept
        -> std::expected<owner, status_t> {
        return adopt(_memory_create(pool_, size, perms));
    }
    auto write(cap_t mem, word_t size, word_t offset, const void* src, size_t n) noexcept
        -> status_t {
        if (offset > size || n > size - offset || (n && !src))
            return STATUS_BAD_ARGS;
        return _mem_write(mem, offset, {reinterpret_cast<const std::byte*>(src), n}).status;
    }
    auto install(cap_t mem, word_t address, word_t size, word_t perms, word_t first = 0) noexcept -> status_t {
        auto region = adopt(_vm_slice(vm_, address, size, perms, RIGHT_MAP | RIGHT_UNMAP));
        if (!region)
            return region.error();
        const auto status =
            _vm_wait(pool_, region->selector(), VM_OP_MAP, {mem, address, size, first, perms});
        return status;
    }
    auto map(const libk::elf_view& elf, const _proc_image& source) noexcept -> status_t {
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
                owner mem;
                cap_t cap = source.memory;
                word_t page{};
                if (shared) {
                    // Map's range and slice ceiling already attenuate this
                    // backing. The installed relation retains its grant.
                    page = (source.offset + seg.offset + offset - skip) / page_size;
                } else {
                    auto made = memory(length, perms | VM_READ | VM_WRITE);
                    if (!made) return made.error();
                    mem = std::move(*made); cap = mem.selector();
                    const auto begin = std::max(offset, skip);
                    const auto end = std::min(offset + length, skip + seg.file_size);
                    auto status = begin < end
                        ? write(cap, length, begin - offset, elf.bytes().data() + seg.offset + begin - skip, end - begin)
                        : STATUS_OK;
                    if (status == STATUS_OK && (perms & VM_EXECUTE)) status = _memory_seal(cap).status;
                    if (status != STATUS_OK) return status;
                }
                if (shared && (perms & VM_WRITE)) {
                    auto made = adopt(_memory_clone(pool_, cap, page, length / page_size, pager_));
                    if (!made) return made.error();
                    mem = std::move(*made); cap = mem.selector(); page = 0;
                }
                if (mem && pager_ && !shared) {
                    const auto bound = _memory_bind_pager(mem.selector(), pager_);
                    if (bound.status != STATUS_OK) return bound.status;
                }
                const auto status = install(cap, seg.va - skip + offset, length, perms, page);
                if (status != STATUS_OK)
                    return status;
                offset += length;
            }
        }
        return STATUS_OK;
    }
};

auto _proc_create(const _proc_auth& auth, const _proc_image& image, const _proc_opts& o) noexcept -> std::expected<_proc, status_t> {
    _proc r;
    const auto capacity = libk::elf_view::segment_capacity(image.bytes);
    if (!capacity) return std::unexpected(STATUS_BAD_ARGS);
    std::unique_ptr<libk::elf_seg[]> segments{new (std::nothrow) libk::elf_seg[capacity]};
    if (!segments) return std::unexpected(STATUS_NO_MEMORY);
    const auto mod = libk::elf_view::parse(image.bytes, {segments.get(), capacity});
    const auto build = [&]() noexcept -> status_t {
        if (image.offset > UINT64_MAX - image.bytes.size())
            return STATUS_BAD_ARGS;
        if (!mod || mod.machine() != 243)
            return STATUS_BAD_ARGS;
        const auto note = mod.note(NATIVE_ABI_NAME, NATIVE_ABI_NOTE);
        uint32_t abi{};
        if (note.size() != sizeof(abi)) return STATUS_BAD_ARGS;
        ::memcpy(&abi, note.data(), sizeof(abi));
        if (abi != NATIVE_ABI_VERSION) return STATUS_BAD_ARGS;
        if (!o.layout.stack_size || o.layout.stack_size % page_size) return STATUS_BAD_ARGS;
        word_t stack_size = o.layout.stack_size;
        for (size_t i = 0; i < mod.segment_count(); ++i) {
            const auto seg = *mod.segment(i);
            if (seg.type != 7) continue;
            const auto tls = libk::checked_add(seg.size, std::max(word_t{16}, seg.align) - 1);
            const auto total = tls ? libk::checked_add(stack_size, *tls) : std::nullopt;
            const auto aligned = total ? libk::checked_align_up(*total, page_size) : std::nullopt;
            if (!aligned) return STATUS_BAD_ARGS;
            stack_size = *aligned;
        }
        if (!o.layout.guard_size || o.layout.guard_size % page_size || stack_size > SIZE_MAX - o.layout.guard_size)
            return STATUS_BAD_ARGS;
        const auto guard = o.layout.guard_size;
        if (stack_size + guard > RISCV64_LOWER_CANONICAL_END - RISCV64_LOW_GUARD_END) return STATUS_BAD_ARGS;
        std::unique_ptr<std::pair<word_t, word_t>[]> storage{new (std::nothrow) std::pair<word_t, word_t>[mod.segment_count() + 2]};
        if (!storage) return STATUS_NO_MEMORY;
        size_t count{};
        if (o.layout.stack) storage[count++] = {o.layout.stack, stack_size + guard};
        if (o.layout.ipc) storage[count++] = {o.layout.ipc, page_size};
        for (size_t i = 0; i < mod.segment_count(); ++i) {
            const auto seg = *mod.segment(i);
            if (seg.type == 1 && seg.size) {
                const auto skip = seg.va % page_size;
                storage[count++] = {seg.va - skip, libk::checked_align_up(seg.size + skip, page_size).value_or(0)};
            }
        }
        // ELF program headers need not be sorted. Validate the complete layout
        // before allocating anything in the child, including page envelopes.
        const auto ranges = std::span{storage.get(), count};
        std::ranges::sort(ranges);
        word_t end{};
        for (const auto& [address, size] : ranges) {
            if (!address || !size || (address | size) % page_size ||
                address < end || address < RISCV64_LOW_GUARD_END || address >= RISCV64_LOWER_CANONICAL_END
                || size > RISCV64_LOWER_CANONICAL_END - address) return STATUS_BAD_ARGS;
            end = address + size;
        }
        const auto argv = o.argv;
        if (argv.size() > INT_MAX || o.envp.size() > INT_MAX) return STATUS_BAD_ARGS;
        size_t strings{};
        for (const auto args : {argv, o.envp})
            for (const auto arg : args) {
                if (arg.find('\0') != std::string_view::npos || arg.size() >= o.layout.stack_size - strings)
                    return STATUS_BAD_ARGS;
                strings += arg.size() + 1;
            }
        auto s = r.open(auth, o.resources);
        if (s != STATUS_OK)
            return s;
        static std::atomic<unsigned> next_cpu;
        auto choices = r.cpus_;
        if (!o.cpu) for (auto n = next_cpu.fetch_add(1, std::memory_order_relaxed) % std::popcount(choices); n; --n)
            choices &= choices - 1;
        const auto cpu = o.cpu.value_or(std::countr_zero(choices));
        if (cpu >= 64 || !(r.cpus_ & (word_t{1} << cpu))) return STATUS_DENIED;
        _proc_loader load{r.alloc(), r.vspace(), o.pager};
        s = load.map(mod, image);
        if (s != STATUS_OK)
            return s;
        const VmAlloc request{VM_ALLOC_VERSION, sizeof(VmAlloc), o.layout.stack, stack_size + guard,
            page_size, 0, UINT64_MAX, VM_READ | VM_WRITE};
        ::memcpy(reinterpret_cast<void*>(_ipc()), &request, sizeof(request));
        const auto stack_claim = _vm_reserve(r.vspace());
        if (stack_claim.status != STATUS_OK) return stack_claim.status;
        _cap_owner stack_region{stack_claim.value};
        const auto stack_base = stack_claim.value2;
        s = _vm_guard(stack_region.selector(), stack_base, guard).status;
        if (s != STATUS_OK) return s;
        auto ipc_request = request; ipc_request.address = o.layout.ipc; ipc_request.bytes = page_size;
        ::memcpy(reinterpret_cast<void*>(_ipc()), &ipc_request, sizeof(ipc_request));
        const auto ipc_claim = _vm_reserve(r.vspace());
        if (ipc_claim.status != STATUS_OK) return ipc_claim.status;
        _cap_owner ipc_region{ipc_claim.value};
        const auto ipc_base = ipc_claim.value2;
        auto stack = load.memory(stack_size);
        if (!stack)
            return stack.error();
        auto ipc = load.memory(page_size);
        if (ipc) {
            s = _vm_wait(r.alloc(), ipc_region.selector(), VM_OP_MAP, {ipc->selector(), ipc_base, page_size, 0, VM_READ | VM_WRITE});
            if (s != STATUS_OK) return s;
        }
        if (!ipc)
            return ipc.error();
        s = _memory_populate(ipc->selector(), 0).status;
        if (s != STATUS_OK)
            return s;
        auto events = load.adopt(_port_create(r.alloc(), o.resources.watches, o.resources.hints));
        if (!events)
            return events.error();
        libk::inplace_vector<AuxEntry, 25> aux{AuxEntry{AT_PAGESZ, page_size},
                                  AuxEntry{AT_ENTRY, mod.entry()},
                                  AuxEntry{AT_SYS_CPUS, word_t(std::popcount(r.cpus_))},
                                  AuxEntry{AT_SYS_IPC, ipc_base},
                                  AuxEntry{AT_SYS_STACK, stack_base}, AuxEntry{AT_SYS_STACK_SIZE, stack_size + guard},
                                  AuxEntry{AT_SYS_GUARD_SIZE, guard}, AuxEntry{AT_SYS_ABI, NATIVE_ABI_VERSION},
                                  AuxEntry{AT_SYS_THREAD, 0}, AuxEntry{AT_SYS_SC, 0}};
        for (auto [key, cap, rights] :
             std::array{std::tuple{AT_SYS_STACK_REGION, stack_region.selector(), word_t{RIGHT_MAP | RIGHT_UNMAP | RIGHT_PROTECT | RIGHT_RESERVE | RIGHT_INSPECT | RIGHT_DUPLICATE | RIGHT_DELEGATE}},
                        std::tuple{AT_SYS_IPC_REGION, ipc_region.selector(), word_t{RIGHT_MAP | RIGHT_UNMAP | RIGHT_PROTECT | RIGHT_RESERVE | RIGHT_INSPECT | RIGHT_DUPLICATE | RIGHT_DELEGATE}},
                        std::tuple{AT_SYS_STACK_MEM, stack->selector(), word_t{RIGHT_DESTROY | RIGHT_MAP | RIGHT_INSPECT}},
                        std::tuple{AT_SYS_IPC_MEM, ipc->selector(), word_t{RIGHT_DESTROY | RIGHT_MAP | RIGHT_INSPECT}},
                        std::tuple{AT_SYS_QUOTA, r.quota(), o.resources.quota_perms},
                        std::tuple{AT_SYS_DOMAIN, r.domain(), word_t{RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE | RIGHT_DELEGATE}},
                        std::tuple{AT_SYS_SCOPE, r.scope(), word_t{RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE}},
                        std::tuple{AT_SYS_VSPACE, r.vspace(),
                                   word_t{RIGHT_MANAGE | RIGHT_DELEGATE | RIGHT_RESERVE | RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP |
                                          RIGHT_INSPECT}},
                        std::tuple{AT_SYS_CSPACE, r.cspace(), word_t{RIGHT_MANAGE | RIGHT_INSPECT}},
                        std::tuple{AT_SYS_EVENTS, events->selector(),
                                   word_t{RIGHT_MANAGE | RIGHT_RECEIVE | RIGHT_DUPLICATE | RIGHT_DESTROY}}}) {
            const auto copy = _cap_duplicate(cap, r.cspace(), rights);
            if (copy.status != STATUS_OK)
                return copy.status;
            libk_assert(aux.try_push_back(AuxEntry{static_cast<word_t>(key), copy.value}));
        }
        auto desc = load.memory(page_size);
        if (!desc)
            return desc.error();
        if (o.bootstrap) {
            const auto copy = _cap_duplicate(o.bootstrap, r.cspace(), RIGHT_SEND | RIGHT_DUPLICATE);
            if (copy.status != STATUS_OK) return copy.status;
            libk_assert(aux.try_push_back(AuxEntry{AT_SYS_BOOTSTRAP, copy.value}));
        }
        if (o.pager) {
            const auto copied = _cap_duplicate(o.pager, r.cspace(), RIGHT_ATTACH | RIGHT_DUPLICATE);
            if (copied.status != STATUS_OK) return copied.status;
            libk_assert(aux.try_push_back(AuxEntry{AT_SYS_PAGER, copied.value}));
        }
        const auto lifecycle = _channel_mint(r.lifecycle(), r.cspace(), 1, RIGHT_SEND | RIGHT_DUPLICATE);
        if (lifecycle.status != STATUS_OK) return lifecycle.status;
        libk_assert(aux.try_push_back(AuxEntry{AT_SYS_LIFECYCLE, lifecycle.value}));
        libk_assert(aux.try_push_back(AuxEntry{AT_NULL, 0}));
        const auto pointers = (argv.size() + o.envp.size() + 3) * sizeof(word_t);
        const auto prefix = pointers + aux.size() * sizeof(AuxEntry);
        if (prefix > stack_size || strings > stack_size - prefix)
            return STATUS_BAD_ARGS;
        const auto initial_bytes = (prefix + strings + 15) & ~size_t{15};
        if (initial_bytes > stack_size)
            return STATUS_BAD_ARGS;
        const auto sp = stack_base + guard + stack_size - initial_bytes;
        std::unique_ptr<uint8_t[]> initial{new (std::nothrow) uint8_t[initial_bytes]{}};
        if (!initial) return STATUS_NO_MEMORY;
        auto* words = reinterpret_cast<word_t*>(initial.get());
        words[0] = argv.size();
        size_t offset = prefix;
        size_t pointer = 1;
        for (const auto args : {argv, o.envp}) {
            for (const auto arg : args) {
                words[pointer++] = sp + offset;
                ::memcpy(initial.get() + offset, arg.data(), arg.size());
                offset += arg.size() + 1;
            }
            ++pointer; // argv/envp each end with a zero pointer.
        }
        ::memcpy(initial.get() + pointers, aux.data(), aux.size() * sizeof(AuxEntry));
        s = load.write(stack->selector(), stack_size, stack_size - initial_bytes,
                       initial.get(), initial_bytes);
        if (s == STATUS_OK)
            s = _vm_wait(r.alloc(), stack_region.selector(), VM_OP_MAP, {stack->selector(), stack_base + guard, stack_size, 0, VM_READ | VM_WRITE});
        if (s != STATUS_OK)
            return s;
        ThreadInit init{};
        init.version = THREAD_START_VERSION;
        init.entry = mod.entry();
        init.stack = sp;
        init.ipc = {.memory = ipc->selector(), .page = 0, .address = ipc_base, .pages = 1};
        s = load.write(desc->selector(), 4096, 0, &init, sizeof(init));
        if (s != STATUS_OK)
            return s;
        s = r.prepare(desc->selector(), o.events, cpu);
        if (s != STATUS_OK) return s;
        for (auto [key, cap, rights] : std::array{
            std::tuple{AT_SYS_THREAD, r.initial_thread(), word_t{RIGHT_OBSERVE | RIGHT_INSPECT | RIGHT_DESTROY | RIGHT_DUPLICATE}},
            std::tuple{AT_SYS_SC, r.initial_sc(), word_t{RIGHT_DESTROY | RIGHT_INSPECT}}}) {
            const auto copy = _cap_duplicate(cap, r.cspace(), rights);
            if (copy.status != STATUS_OK) return copy.status;
            for (auto& item : aux) if (item.type == word_t(key)) item.value = copy.value;
        }
        s = load.write(stack->selector(), stack_size, stack_size - initial_bytes + pointers,
            aux.data(), aux.size() * sizeof(AuxEntry));
        if (s == STATUS_OK && o.pager) s = _memory_bind_pager(stack->selector(), o.pager).status;
        return s;
    };
    const auto status = build();
    if (status != STATUS_OK) return std::unexpected(status); // r submits private Scope/Quota closure.
    return r;
}
auto _proc_create(const _proc_image& image, const _proc_opts& opts) noexcept -> std::expected<_proc, status_t> {
    return _proc_create({_startup().alloc(), _startup().domain}, image, opts);
}
auto _proc_spawn(const _proc_auth& auth, const _proc_image& image, const _proc_opts& opts) noexcept -> std::expected<_proc, status_t> {
    auto child = _proc_create(auth, image, opts);
    if (child) if (const auto status = child->start(); status != STATUS_OK) return std::unexpected(status);
    return child;
}
auto _proc_spawn(const _proc_image& image, const _proc_opts& opts) noexcept -> std::expected<_proc, status_t> {
    return _proc_spawn({_startup().alloc(), _startup().domain}, image, opts);
}

// A private dispatcher observes both exit reports and native failures even
// when the owner has no enclosing event loop. Timeout preserves the process.
auto _proc::join(word_t deadline) noexcept -> std::expected<_proc_result, status_t> {
    if (state_ == state::New) return std::unexpected(STATUS_BAD_ARGS);
    if (retired()) return result_;
    _ev_port port;
    if (const auto status = port.open(_startup().alloc(), 2, 2); status != STATUS_OK)
        return std::unexpected(status);
    std::array<_ev_watch, 2> watches;
    std::array<cap_t, 2> sources{};
    for (;;) {
        if (const auto status = poll(); status != STATUS_OK) return std::unexpected(status);
        if (stopped()) if (const auto status = retire(); status != STATUS_OK) return std::unexpected(status);
        if (retired()) return result_;
        const bool executing = state_ <= state::Stopping;
        const std::array<cap_t, 2> desired{executing ? vspace() : scope_ ? scope() : quota(),
            state_ == state::Running ? lifecycle_.ends[0].selector() : cap_t{}};
        for (size_t i = 0; i < watches.size(); ++i) {
            if (sources[i] != desired[i]) {
                if (const auto status = watches[i].close(); status != STATUS_OK) return std::unexpected(status);
                sources[i] = desired[i];
                if (sources[i]) {
                    const auto status = watches[i].open(&port, sources[i], 0, 0, WATCH_CHANGED,
                        i ? SOURCE_READ : executing ? SOURCE_EXIT : SOURCE_STATE);
                    if (status != STATUS_OK) return std::unexpected(status);
                }
            }
            if (watches[i]) {
                const auto facts = _source_info(sources[i], i ? SOURCE_READ : executing ? SOURCE_EXIT : SOURCE_STATE);
                if (facts.status != STATUS_OK) return std::unexpected(facts.status);
                const auto armed = watches[i].arm(facts.value2);
                if (armed.status != STATUS_OK) return std::unexpected(armed.status);
            }
        }
        // Facts might have changed during registration; poll before sleeping.
        const auto before = state_;
        if (const auto status = poll(); status != STATUS_OK) return std::unexpected(status);
        if (retired() || state_ != before || sources[0] != (executing ? vspace() : scope_ ? scope() : quota())) continue;
        const auto waited = _ev_wait(&port, deadline).status;
        if (waited != STATUS_OK) return std::unexpected(waited);
    }
}
