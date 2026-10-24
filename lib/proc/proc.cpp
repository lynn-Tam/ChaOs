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

proc_task::~proc_task() {
    (void)stop();
    if (quota_) (void)sys::quota_close(quota());
}
auto proc_task::open(const proc_auth& auth, const proc_quota& q) noexcept -> status_t {
    if (state_ != state::New || quota_) return STATUS_BUSY;
    const auto quota = sys::quota_create(auth.alloc, q.bytes, q.caps, q.kinds,
        q.max_bytes ? q.max_bytes : q.bytes, q.max_caps ? q.max_caps : q.caps);
    quota_ = cap_owner{quota.value};
    if (quota.status != STATUS_OK) return quota.status;
    const auto scope = sys::scope_create(auth.alloc); scope_ = cap_owner{scope.value};
    if (scope.status != STATUS_OK) return scope.status;
    cpus_ = auth.cpus >= 64 ? ~word_t{} : (word_t{1} << auth.cpus) - 1;
    const auto domain = sys::domain_create(alloc(), auth.domain, 0, 0, 1024, cpus_); domain_ = cap_owner{domain.value};
    if (domain.status != STATUS_OK) return domain.status;
    const auto vm = sys::vspace_create(alloc()); vm_ = cap_owner{vm.value};
    if (vm.status != STATUS_OK) return vm.status;
    const auto cs = sys::cspace_create(alloc(), q.slots, q.pages); cs_ = cap_owner{cs.value};
    if (cs.status != STATUS_OK) return cs.status;
    thread_limit_ = q.threads;
    return lifecycle_.open(alloc(), 4, 2, 3);
}
auto proc_task::supervise(cap_owner cap, word_t role, bool reap) -> status_t {
    if (!reap && threads_.size() >= thread_limit_) return STATUS_EXHAUSTED;
    auto status = sys::object_info(cap.selector()).status;
    if (status != STATUS_OK) return status;
    const auto object = *reinterpret_cast<const ObjectInfo*>(rt_current().ipc);
    if (object.kind != ObjThread) return STATUS_BAD_ARGS;
    const auto found = std::ranges::find_if(threads_, [&](const auto& t) { return t.id == object.object_id; });
    if (reap) {
        const auto ended = sys::exit_query(cap.selector());
        if (ended.status != STATUS_OK) return ended.status;
        if (!ended.value) return STATUS_WOULD_BLOCK;
        if (ended.value2) return static_cast<status_t>(ended.value2);
        if (found != threads_.end()) {
            if (found->id == initial_id_) { (void)thread_.close(); (void)sc_.close(); }
            threads_.erase(found);
        }
        return STATUS_OK;
    }
    if (found != threads_.end()) return STATUS_BAD_ARGS;
    status = sys::scope_query(scope(), object.object_id, SCOPE_LINKS).status;
    if (status != STATUS_OK) return status;
    const auto links = *reinterpret_cast<const LinkRecords*>(rt_current().ipc);
    if (std::ranges::none_of(std::span{links.records, links.count}, [&](const auto& r) { return r.kind == LINK_ROOT && r.object_id == vm_id_; })) return STATUS_DENIED;
    observer record{.thread=std::move(cap), .exit={}, .id=object.object_id, .role=role};
    if (events_) status = record.exit.open(events_, record.thread.selector(), SOURCE_READY, 0, 0, SOURCE_EXIT);
    if (status == STATUS_OK) threads_.push_back(std::move(record));
    return status;
}
auto proc_task::prepare(cap_t desc, cap_t events, uint32_t cpu) noexcept -> status_t {
    if (thread_) return STATUS_BUSY;
    const auto thread = sys::thread_create(alloc(), vspace(), cspace(), desc, 0, cpu); thread_ = cap_owner{thread.value};
    if (thread.status != STATUS_OK) return thread.status;
    const auto sc = sys::sc_create(alloc(), domain(), 0, 0, 1024, cpus_); sc_ = cap_owner{sc.value};
    if (sc.status != STATUS_OK) return sc.status;
    auto status = sys::sc_bind(sc_.selector(), thread_.selector()).status;
    if (status != STATUS_OK) return status;
    events_ = events;
    status = sys::object_info(vspace()).status;
    if (status != STATUS_OK) return status;
    vm_id_ = reinterpret_cast<const ObjectInfo*>(rt_current().ipc)->object_id;
    const auto copy = sys::cap_duplicate(thread_.selector(), rt_startup().cspace, RIGHT_OBSERVE | RIGHT_INSPECT);
    if (copy.status != STATUS_OK) return copy.status;
    status = supervise(cap_owner{copy.value}, 2);
    if (status != STATUS_OK) return status;
    initial_id_ = threads_.back().id;
    if (events_) {
        const std::array<cap_t, 4> sources{domain(), lifecycle_.ends[0].selector(), scope(), quota()};
        for (size_t i = 0; i < sources.size(); ++i) {
            status = watches_[i].open(events_, sources[i], i == 0 ? 0 : i == 1 ? SOURCE_READY | SOURCE_TERMINAL : STATE_DRAINED,
                0, i == 0 ? WATCH_CHANGED : 0, i == 1 ? SOURCE_READ : SOURCE_STATE);
            if (status != STATUS_OK) return status;
        }
    }
    return STATUS_OK;
}
auto proc_task::start() noexcept -> status_t {
    if (!thread_ || state_ != state::New) return STATUS_BAD_ARGS;
    const auto status = sys::execution_set(thread_.selector(), true).status;
    if (status == STATUS_OK) state_ = state::Running;
    return status;
}
auto proc_task::execution_info() const noexcept -> std::expected<DomainInfo, status_t> {
    const auto status = sys::domain_info(domain()).status;
    if (status != STATUS_OK) return std::unexpected(status);
    return *reinterpret_cast<const DomainInfo*>(rt_current().ipc);
}
auto proc_task::memory_info() const noexcept -> std::expected<VSpaceInfo, status_t> {
    const auto status = sys::vspace_info(vspace()).status;
    if (status != STATUS_OK) return std::unexpected(status);
    return *reinterpret_cast<const VSpaceInfo*>(rt_current().ipc);
}
auto proc_task::resource_info() const noexcept -> std::expected<QuotaInfo, status_t> {
    const auto status = sys::quota_info(quota()).status;
    if (status != STATUS_OK) return std::unexpected(status);
    return *reinterpret_cast<const QuotaInfo*>(rt_current().ipc);
}
auto proc_task::observe() noexcept -> sys::SysResult {
    if (state_ == state::Consumed) return {.status=STATUS_INVALID_CAP};
    if (state_ == state::Closing) return {.status=STATUS_OK, .value=1, .value2=static_cast<word_t>(status_)};
    if (state_ != state::Running) return {.status=STATUS_WOULD_BLOCK};
    for (;;) {
        ch_packet packet;
        const auto received = ch_recv(lifecycle_.ends[0].selector(), packet, false).status;
        if (received == STATUS_WOULD_BLOCK) break;
        if (received != STATUS_OK) return {.status=STATUS_OK, .value=1, .value2=word_t(STATUS_PEER_FAULT)};
        if (packet.word_count != 3 || packet.words[0] != rt_life_version) return {.status=STATUS_OK, .value=1, .value2=word_t(STATUS_PEER_FAULT)};
        if (packet.words[1] == rt_life_exit && !packet.called && !packet.count)
            return {.status=STATUS_OK, .value=1, .value2=packet.words[2]};
        status_t status = STATUS_BAD_ARGS;
        if ((packet.words[1] == rt_life_register || packet.words[1] == rt_life_reap) && packet.called && packet.count == 2 && packet.words[2] <= 1)
            status = supervise(std::move(packet.capabilities[1]), packet.words[2], packet.words[1] == rt_life_reap);
        if (packet.called && packet.count) {
            const word_t reply[] = {word_t(status)};
            const auto sent = ch_send(packet.capabilities[0].selector(), reply, false).status;
            if (sent != STATUS_OK && sent != STATUS_STALE && sent != STATUS_CLOSED && sent != STATUS_PEER_CLOSED) return {.status=sent};
        }
    }
    if (events_) {
        const auto status = watches_[1].arm().status;
        if (status != STATUS_OK) return {.status=status};
    }
    for (auto it = threads_.begin(); it != threads_.end();) {
        const auto exited = sys::exit_query(it->thread.selector());
        if (exited.status != STATUS_OK) return {.status=STATUS_OK, .value=1, .value2=word_t(STATUS_PEER_FAULT)};
        if (exited.value) {
            if (exited.value2 || it->role == 1) return {.status=STATUS_OK, .value=1, .value2=exited.value2 ? exited.value2 : word_t(STATUS_INTERNAL)};
            if (it->id == initial_id_) { (void)thread_.close(); (void)sc_.close(); }
            it = threads_.erase(it);
        } else {
            if (events_) { const auto status = it->exit.arm().status; if (status != STATUS_OK) return {.status=status}; }
            ++it;
        }
    }
    return {.status=STATUS_OK};
}
auto proc_task::stop(status_t reason) noexcept -> status_t {
    if (state_ == state::Consumed) return STATUS_INVALID_CAP;
    if (state_ == state::Closing) return STATUS_OK;
    state_ = state::Closing; status_ = reason;
    const auto watch = watches_[1].close(); if (watch != STATUS_OK) return watch;
    const auto life = lifecycle_.close(); if (life != STATUS_OK) return life;
    threads_.clear();
    for (auto* cap : {&thread_, &sc_}) { const auto status = cap->close(); if (status != STATUS_OK) return status; }
    return scope_ ? sys::scope_close(scope()).status : STATUS_OK;
}
auto proc_task::close() noexcept -> status_t {
    if (!quota_) return STATUS_OK;
    if (!closing_) {
        for (auto* cap : {&cs_, &vm_, &domain_}) { const auto status = cap->close(); if (status != STATUS_OK) return status; }
        closing_ = true;
    }
    if (scope_) {
        const auto facts = sys::source_info(scope());
        if (facts.status != STATUS_OK) return facts.status;
        if (!(facts.value & STATE_DRAINED)) return STATUS_WOULD_BLOCK;
        const auto closed = scope_.close(); if (closed != STATUS_OK) return closed;
    }
    const auto facts = sys::source_info(quota());
    if (facts.status != STATUS_OK) return facts.status;
    if (facts.value & STATE_OPEN) { const auto status = sys::quota_close(quota()).status; return status == STATUS_OK ? STATUS_WOULD_BLOCK : status; }
    if (!(facts.value & STATE_DRAINED)) return STATUS_WOULD_BLOCK;
    const auto destroyed = sys::object_destroy(quota()).status; if (destroyed != STATUS_OK) return destroyed;
    for (auto& watch : watches_) { const auto status = watch.close(); if (status != STATUS_OK) return status; }
    return quota_.close();
}
auto proc_task::poll() noexcept -> status_t {
    if (state_ == state::Consumed) return STATUS_INVALID_CAP;
    if (state_ != state::Closing) {
        const auto ended = observe();
        if (ended.status != STATUS_OK) return ended.status;
        if (!ended.value) return STATUS_WOULD_BLOCK;
        const auto status = stop(static_cast<status_t>(ended.value2)); if (status != STATUS_OK) return status;
    }
    return close();
}
auto proc_task::collect() noexcept -> sys::SysResult {
    const auto status = poll(); if (status != STATUS_OK) return {.status=status};
    state_ = state::Consumed;
    return {.status=STATUS_OK, .value=static_cast<word_t>(status_)};
}

static constexpr word_t page_size = 4096;

auto proc_image::map(vm_area& source, cap_owner memory, uint64_t offset, uint64_t size, word_t perms)
    -> std::expected<proc_image, status_t> {
    const auto end = libk::checked_add(offset, size);
    const auto bytes = end ? libk::checked_align_up(*end, page_size).value_or(0) : 0;
    if (!size || !bytes) return std::unexpected(STATUS_BAD_ARGS);
    auto mapping = vm_area::map(rt_vm(), std::move(memory), 0, bytes, VM_READ);
    if (!mapping) return std::unexpected(mapping.error());
    source = std::move(*mapping);
    return proc_image{.bytes={reinterpret_cast<const std::byte*>(source.address + offset), size_t(size)},
        .memory=source.memory.selector(), .offset=offset, .perms=perms};
}

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
        if (r.status == STATUS_PENDING) r.status = op_wait(cap.selector()).status;
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
        return mem_write(mem, offset, {reinterpret_cast<const std::byte*>(src), n});
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
                    if (status == STATUS_OK && (perms & VM_EXECUTE)) status = sys::memory_seal(cap).status;
                    if (status != STATUS_OK) return status;
                }
                const auto status = install(cap, seg.va - skip + offset, length, perms, page,
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
        if (!auth.cpus || o.cpu >= auth.cpus) return STATUS_BAD_ARGS;
        if (!o.va.stack_size || o.va.stack_size % page_size) return STATUS_BAD_ARGS;
        word_t stack_size = o.va.stack_size;
        for (size_t i = 0; i < mod.segment_count(); ++i) {
            const auto seg = *mod.segment(i);
            if (seg.type != 7) continue;
            const auto tls = libk::checked_add(seg.size, std::max(word_t{16}, seg.align) - 1);
            const auto total = tls ? libk::checked_add(stack_size, *tls) : std::nullopt;
            const auto aligned = total ? libk::checked_align_up(*total, page_size) : std::nullopt;
            if (!aligned) return STATUS_BAD_ARGS;
            stack_size = *aligned;
        }
        std::vector<std::pair<word_t, word_t>> ranges{
            {o.va.stack, stack_size}, {o.va.ipc, page_size}};
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
        auto s = r.quota() ? STATUS_OK : r.open(auth, o.resources);
        if (s != STATUS_OK)
            return s;
        proc_loader load{r.alloc(), r.vspace()};
        s = load.map(mod, image);
        if (s != STATUS_OK)
            return s;
        auto stack = load.memory(stack_size);
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
                                  {AT_SYS_IPC, o.va.ipc},
                                  {AT_SYS_STACK, o.va.stack}, {AT_SYS_STACK_SIZE, stack_size},
                                  {AT_SYS_THREAD, 0}, {AT_SYS_SC, 0}};
        for (auto [key, cap, rights] :
             std::array{std::tuple{AT_SYS_STACK_MEM, stack->selector(), word_t{RIGHT_DESTROY | RIGHT_MAP | RIGHT_INSPECT}},
                        std::tuple{AT_SYS_IPC_MEM, ipc->selector(), word_t{RIGHT_DESTROY | RIGHT_MAP | RIGHT_INSPECT}},
                        std::tuple{AT_SYS_QUOTA, r.quota(), o.resources.quota_perms},
                        std::tuple{AT_SYS_DOMAIN, r.domain(), word_t{RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE | RIGHT_DELEGATE}},
                        std::tuple{AT_SYS_SCOPE, r.scope(), word_t{RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE}},
                        std::tuple{AT_SYS_VSPACE, r.vspace(),
                                   word_t{RIGHT_MANAGE | RIGHT_DELEGATE | RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP |
                                          RIGHT_DESTROY | RIGHT_INSPECT}},
                        std::tuple{AT_SYS_CSPACE, r.cspace(), word_t{RIGHT_MANAGE | RIGHT_INSPECT}},
                        std::tuple{AT_SYS_EVENTS, events->selector(),
                                   word_t{RIGHT_MANAGE | RIGHT_RECEIVE | RIGHT_DUPLICATE | RIGHT_DESTROY}}}) {
            const auto copy = sys::cap_duplicate(cap, r.cspace(), rights);
            if (copy.status != STATUS_OK)
                return copy.status;
            aux.push_back({static_cast<word_t>(key), copy.value});
        }
        auto desc = load.memory(page_size);
        if (!desc)
            return desc.error();
        if (o.environment) {
            const auto copy = sys::cap_duplicate(o.environment, r.cspace(), RIGHT_SEND | RIGHT_DUPLICATE);
            if (copy.status != STATUS_OK) return copy.status;
            aux.push_back({AT_SYS_ENV, copy.value});
        }
        const auto lifecycle = sys::channel_mint(r.lifecycle(), r.cspace(), 1, RIGHT_SEND | RIGHT_DUPLICATE);
        if (lifecycle.status != STATUS_OK) return lifecycle.status;
        aux.push_back({AT_SYS_LIFECYCLE, lifecycle.value});
        aux.push_back({AT_NULL, 0});
        const auto pointers = (argv.size() + o.envp.size() + 3) * sizeof(word_t);
        const auto prefix = pointers + aux.size() * sizeof(AuxEntry);
        if (prefix > stack_size || strings > stack_size - prefix)
            return STATUS_BAD_ARGS;
        const auto initial_bytes = (prefix + strings + 15) & ~size_t{15};
        if (initial_bytes > stack_size)
            return STATUS_BAD_ARGS;
        const auto sp = o.va.stack + stack_size - initial_bytes;
        std::vector<uint8_t> initial(initial_bytes);
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
        s = load.write(stack->selector(), stack_size, stack_size - initial_bytes,
                       initial.data(), initial.size());
        if (s == STATUS_OK)
            s = load.install(stack->selector(), o.va.stack, stack_size, VM_READ | VM_WRITE);
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
        s = r.prepare(desc->selector(), o.events, o.cpu);
        if (s != STATUS_OK) return s;
        for (auto [key, cap, rights] : std::array{
            std::tuple{AT_SYS_THREAD, r.initial_thread(), word_t{RIGHT_OBSERVE | RIGHT_INSPECT | RIGHT_DESTROY | RIGHT_DUPLICATE}},
            std::tuple{AT_SYS_SC, r.initial_sc(), word_t{RIGHT_DESTROY | RIGHT_INSPECT}}}) {
            const auto copy = sys::cap_duplicate(cap, r.cspace(), rights);
            if (copy.status != STATUS_OK) return copy.status;
            for (auto& item : aux) if (item.type == word_t(key)) item.value = copy.value;
        }
        return load.write(stack->selector(), stack_size, stack_size - initial_bytes + pointers,
            aux.data(), aux.size() * sizeof(AuxEntry));
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
    while (name.starts_with('/')) name.remove_prefix(1);
    const auto file = files.find(name);
    if (!file) return std::unexpected(STATUS_NOT_FOUND);
    return proc_image{.bytes=file->data};
}

// Samples execution threads, not the scheduling descendants funded by them.
// Native membership/version checks bound the snapshot; the caller discards an
// interval when the actual thread set changes. Quotaing remains native.
auto proc_task::cpu_info() const -> std::expected<std::pair<uint64_t, std::vector<uint64_t>>, status_t> {
    const auto now = sys::clock_now().value;
    // Coalesce observers into one sample per second. The timestamp travels with
    // the data; a cached sample is never relabeled with a newer clock value.
    if (cpu_at_ && now - cpu_at_ < sys::clock_frequency().value) {
        if (cpu_status_ != STATUS_OK) return std::unexpected(cpu_status_);
        return cpu_;
    }
    cpu_at_ = now;
    const auto collect = [&]() -> std::expected<std::vector<uint64_t>, status_t> {
        const auto ipc = reinterpret_cast<void*>(rt_current().ipc);
        const auto identity = sys::object_info(vspace());
        if (identity.status != STATUS_OK) return std::unexpected(identity.status);
        const auto vm = reinterpret_cast<const ObjectInfo*>(ipc)->object_id;
        std::vector<std::array<uint64_t, 2>> threads;
        ScopeCursor cursor{};
        for (size_t pages = 0;; ++pages) {
            if (pages == 128) return std::unexpected(STATUS_EXHAUSTED);
            const auto scan = sys::scope_members(scope(), cursor);
            if (scan.status != STATUS_OK) return std::unexpected(scan.status);
            ScopeMembers page; ::memcpy(&page, ipc, sizeof(page));
            for (const auto& member : std::span{page.records, page.count}) {
                if (member.kind != ObjThread) continue;
                const auto linked = sys::scope_query(scope(), member.object_id, SCOPE_LINKS);
                if (linked.status != STATUS_OK) return std::unexpected(linked.status);
                LinkRecords links; ::memcpy(&links, ipc, sizeof(links));
                if (links.more) return std::unexpected(STATUS_EXHAUSTED);
                if (std::ranges::none_of(std::span{links.records, links.count}, [&](const auto& link) {
                    return link.kind == LINK_ROOT && link.object_id == vm;
                })) continue;
                const auto sampled = sys::scope_query(scope(), member.object_id, SCOPE_USAGE);
                if (sampled.status != STATUS_OK) return std::unexpected(sampled.status);
                threads.push_back({member.object_id, reinterpret_cast<const ThreadInfo*>(ipc)->runtime});
            }
            cursor = page.next;
            if (!page.more) break;
        }
        const auto final = sys::scope_info(scope());
        if (final.status != STATUS_OK) return std::unexpected(final.status);
        if (reinterpret_cast<const ScopeInfo*>(ipc)->epoch != cursor.version) return std::unexpected(STATUS_STALE);
        std::ranges::sort(threads);
        std::vector<uint64_t> result; result.reserve(threads.size() * 2);
        for (const auto& thread : threads) result.insert(result.end(), thread.begin(), thread.end());
        return result;
    };
    auto result = collect(); cpu_status_ = result ? STATUS_OK : result.error();
    if (!result) return std::unexpected(result.error());
    cpu_ = {sys::clock_now().value, std::move(*result)};
    return cpu_;
}
